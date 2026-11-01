/*
 * hakuX native core — FEXCore embedding.
 *
 * Shaped like FEX's WOW64 front end: FEXCore as a library, no Linux syscall
 * layer. The guest runs in FEX system mode behind the guest memory base;
 * system ops (including the kernel-call trampolines' INT 0x3F) arrive through
 * the syscall path and are handed to the core's callback.
 */
#include "native/cpu/cpu.h"

#include "Common/HostFeatures.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>

#include <asm/sigcontext.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/mman.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#endif
#include <ucontext.h>

static_assert(NC_SYSOP_CPUID == static_cast<int>(FEXCore::Core::SystemOpKind::Cpuid));
static_assert(NC_SYSOP_OTHER == static_cast<int>(FEXCore::Core::SystemOpKind::Other));
static_assert(NC_SYSOP_INTERRUPT == static_cast<int>(FEXCore::Core::SystemOpKind::Interrupt));
static_assert(NC_SYSOP_HOOK == static_cast<int>(FEXCore::Core::SystemOpKind::Hook));
static_assert(static_cast<int>(NC_EAX) == static_cast<int>(FEXCore::X86State::REG_RAX) &&
              static_cast<int>(NC_EDI) == static_cast<int>(FEXCore::X86State::REG_RDI));

struct NcFrame {
  FEXCore::Core::CpuStateFrame Frame;
};

static FEXCore::Core::CpuStateFrame& F(NcFrame* f) {
  return *reinterpret_cast<FEXCore::Core::CpuStateFrame*>(f);
}
static const FEXCore::Core::CpuStateFrame& F(const NcFrame* f) {
  return *reinterpret_cast<const FEXCore::Core::CpuStateFrame*>(f);
}

extern "C" uint32_t nc_frame_gpr(const NcFrame* f, int reg) {
  return static_cast<uint32_t>(F(f).State.gregs[reg]);
}
extern "C" void nc_frame_set_gpr(NcFrame* f, int reg, uint32_t v) {
  F(f).State.gregs[reg] = v;
}
extern "C" uint32_t nc_frame_eip(const NcFrame* f) {
  return static_cast<uint32_t>(F(f).State.rip);
}
extern "C" void nc_frame_set_eip(NcFrame* f, uint32_t eip) {
  F(f).State.rip = eip;
}
extern "C" bool nc_frame_if(const NcFrame* f) {
  return F(f).State.flags[FEXCore::X86State::RFLAG_IF_LOC] != 0;
}
extern "C" void nc_frame_set_if(NcFrame* f, bool on) {
  F(f).State.flags[FEXCore::X86State::RFLAG_IF_LOC] = on ? 1 : 0;
}

namespace {

class Syscalls final : public FEXCore::HLE::SyscallHandler {
public:
  NcCpuConfig Cfg {};

  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    const uint64_t Info = Frame->SystemOpInfo;
    Frame->SystemOpInfo = 0;
    const auto Op = static_cast<NcSysOp>(Info & 0xff);
    const unsigned Size = (Info >> 8) & 0xff;
    if (Info && Cfg.sysop(Cfg.opaque, reinterpret_cast<NcFrame*>(Frame), Op, Size)) {
      return;
    }
    nc_log("hakuX-native", "cpu: unhandled %s at %08x", Info ? "system op" : "syscall", static_cast<uint32_t>(Frame->State.rip));
    abort();
  }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, 1ull << 32, true};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

class Signals final : public FEXCore::SignalDelegator {};

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  if (Level <= LogMan::DebugLevels::INFO) {
    nc_log("hakuX-native-fex", "%s %s", LogMan::DebugLevelStr(Level), Message);
  }
}

void AssertHandler(const char* Message) {
  nc_log("hakuX-native-fex", "ASSERT %s", Message);
  abort();
}

uint64_t TscRatioQ32() {
  uint64_t Freq;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(Freq));
  return static_cast<uint64_t>((static_cast<unsigned __int128>(733'333'333) << 32) / Freq);
}

} // namespace

namespace {

// The architectural part of FEX's CPUState that belongs to a guest thread
// (FEX's own bookkeeping, e.g. the call-ret stack pointer, stays put).
struct SavedState {
  uint64_t rip;
  uint64_t gregs[16];
  uint32_t pf_raw, af_raw;
  uint8_t flags[48];
  FEXCore::Core::CPUState::XMMRegs xmm;
  uint64_t mm[8][2];
  uint32_t mxcsr;
  uint16_t FCW;
  uint8_t AbridgedFTW;
  uint16_t es_idx, cs_idx, ss_idx, ds_idx, gs_idx, fs_idx;
  uint32_t es_cached, cs_cached, ss_cached, ds_cached;
  uint64_t gs_cached, fs_cached;
};
static_assert(sizeof(SavedState) <= sizeof(NcCpuContext::blob));
static_assert(alignof(SavedState) <= alignof(NcCpuContext));

SavedState& Saved(NcCpuContext* c) {
  return *reinterpret_cast<SavedState*>(c->blob);
}
const SavedState& Saved(const NcCpuContext* c) {
  return *reinterpret_cast<const SavedState*>(c->blob);
}

void Save(const FEXCore::Core::CPUState& S, SavedState& D) {
  D.rip = S.rip;
  memcpy(D.gregs, S.gregs, sizeof(D.gregs));
  D.pf_raw = S.pf_raw;
  D.af_raw = S.af_raw;
  memcpy(D.flags, S.flags, sizeof(D.flags));
  memcpy(&D.xmm, &S.xmm, sizeof(D.xmm));
  memcpy(D.mm, S.mm, sizeof(D.mm));
  D.mxcsr = S.mxcsr;
  D.FCW = S.FCW;
  D.AbridgedFTW = S.AbridgedFTW;
  D.es_idx = S.es_idx, D.cs_idx = S.cs_idx, D.ss_idx = S.ss_idx, D.ds_idx = S.ds_idx, D.gs_idx = S.gs_idx, D.fs_idx = S.fs_idx;
  D.es_cached = S.es_cached, D.cs_cached = S.cs_cached, D.ss_cached = S.ss_cached, D.ds_cached = S.ds_cached;
  D.gs_cached = S.gs_cached, D.fs_cached = S.fs_cached;
}

void Load(FEXCore::Core::CPUState& S, const SavedState& D) {
  S.rip = D.rip;
  memcpy(S.gregs, D.gregs, sizeof(D.gregs));
  S.pf_raw = D.pf_raw;
  S.af_raw = D.af_raw;
  memcpy(S.flags, D.flags, sizeof(D.flags));
  memcpy(&S.xmm, &D.xmm, sizeof(D.xmm));
  memcpy(S.mm, D.mm, sizeof(D.mm));
  S.mxcsr = D.mxcsr;
  S.FCW = D.FCW;
  S.AbridgedFTW = D.AbridgedFTW;
  S.es_idx = D.es_idx, S.cs_idx = D.cs_idx, S.ss_idx = D.ss_idx, S.ds_idx = D.ds_idx, S.gs_idx = D.gs_idx, S.fs_idx = D.fs_idx;
  S.es_cached = D.es_cached, S.cs_cached = D.cs_cached, S.ss_cached = D.ss_cached, S.ds_cached = D.ds_cached;
  S.gs_cached = D.gs_cached, S.fs_cached = D.fs_cached;
}

} // namespace

struct NcCpu {
  fextl::unique_ptr<FEXCore::Context::Context> CTX;
  Syscalls Sys;
  Signals Sig;
  FEXCore::Core::InternalThreadState* Thread {};
  FEXCore::Core::CPUState::gdt_segment Gdt[32] {};
  SavedState Reset {}; // the JIT's reset state, for new guest threads
  bool X87Watch = false;  // reduced x87 with precision control: watch FCW.PC (nc_cpu_x87_single_check)
  bool X87Single = false; // a thread selected single precision: translations round to it
};

static bool HookAdapter(void* Opaque, uint64_t Rip) {
  auto* Cpu = static_cast<NcCpu*>(Opaque);
  return Cpu->Sys.Cfg.hook_query(Cpu->Sys.Cfg.opaque, static_cast<uint32_t>(Rip));
}

extern "C" void nc_frame_save(const NcFrame* f, NcCpuContext* c) {
  Save(F(f).State, Saved(c));
}

extern "C" void nc_frame_load(NcFrame* f, const NcCpuContext* c) {
  Load(F(f).State, Saved(c));
  // The call-ret predictor holds the previous thread's returns: start empty.
  auto* Thread = F(f).Thread;
  F(f).State.callret_sp = reinterpret_cast<uint64_t>(Thread->CallRetStackBase) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
}

extern "C" void nc_context_export(const NcCpuContext* c, NcCpuState* s) {
  const auto& D = Saved(c);
  memset(s, 0, sizeof(*s));
  s->rip = D.rip;
  memcpy(s->gregs, D.gregs, sizeof(s->gregs));
  s->pf_raw = D.pf_raw;
  s->af_raw = D.af_raw;
  memcpy(s->flags, D.flags, sizeof(s->flags));
  static_assert(sizeof(s->xmm) == sizeof(D.xmm.sse.data));
  memcpy(s->xmm, D.xmm.sse.data, sizeof(s->xmm));
  memcpy(s->mm, D.mm, sizeof(s->mm));
  s->mxcsr = D.mxcsr;
  s->fcw = D.FCW;
  s->ftw = D.AbridgedFTW;
  s->es_idx = D.es_idx, s->cs_idx = D.cs_idx, s->ss_idx = D.ss_idx, s->ds_idx = D.ds_idx, s->gs_idx = D.gs_idx, s->fs_idx = D.fs_idx;
  s->es_cached = D.es_cached, s->cs_cached = D.cs_cached, s->ss_cached = D.ss_cached, s->ds_cached = D.ds_cached;
  s->gs_cached = D.gs_cached, s->fs_cached = D.fs_cached;
}

extern "C" void nc_context_import(NcCpuContext* c, const NcCpuState* s) {
  memset(c, 0, sizeof(*c));
  auto& D = Saved(c);
  D.rip = s->rip;
  memcpy(D.gregs, s->gregs, sizeof(D.gregs));
  D.pf_raw = s->pf_raw;
  D.af_raw = s->af_raw;
  memcpy(D.flags, s->flags, sizeof(D.flags));
  memcpy(D.xmm.sse.data, s->xmm, sizeof(s->xmm));
  memcpy(D.mm, s->mm, sizeof(D.mm));
  D.mxcsr = s->mxcsr;
  D.FCW = s->fcw;
  D.AbridgedFTW = s->ftw;
  D.es_idx = s->es_idx, D.cs_idx = s->cs_idx, D.ss_idx = s->ss_idx, D.ds_idx = s->ds_idx, D.gs_idx = s->gs_idx, D.fs_idx = s->fs_idx;
  D.es_cached = s->es_cached, D.cs_cached = s->cs_cached, D.ss_cached = s->ss_cached, D.ds_cached = s->ds_cached;
  D.gs_cached = s->gs_cached, D.fs_cached = s->fs_cached;
}

extern "C" void nc_context_init(NcCpu* cpu, NcCpuContext* c, uint32_t eip, uint32_t esp) {
  memset(c, 0, sizeof(*c));
  auto& D = Saved(c);
  D = cpu->Reset;
  D.rip = eip;
  D.gregs[FEXCore::X86State::REG_RSP] = esp;
}

extern "C" void nc_context_set_gpr(NcCpuContext* c, int reg, uint32_t v) {
  Saved(c).gregs[reg] = v;
}

extern "C" uint32_t nc_context_gpr(const NcCpuContext* c, int reg) {
  return static_cast<uint32_t>(Saved(c).gregs[reg]);
}

extern "C" uint32_t nc_context_eip(const NcCpuContext* c) {
  return static_cast<uint32_t>(Saved(c).rip);
}

extern "C" void nc_context_set_eip(NcCpuContext* c, uint32_t eip) {
  Saved(c).rip = eip;
}

extern "C" NcCpu* nc_cpu_create(const NcCpuConfig* cfg, uint32_t fs_base) {
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  FEXCore::Config::Initialize();
  FEXCore::Config::ReloadMetaLayer();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "1");
  // x87 at 64-bit precision: Xbox titles set the FPU to single/double precision.
  // A/B switch (NC_FEX_X87=80, or debug.hakux.fex_x87=80): the full 80-bit x87.
  bool x87_full = false;
  {
    const char* e = getenv("NC_FEX_X87");
    char v[PROP_VALUE_MAX] = "";
#ifdef __ANDROID__
    hle_prop_get("debug.hakux.fex_x87", v);
#endif
    x87_full = (e && !strcmp(e, "80")) || !strcmp(v, "80");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_X87REDUCEDPRECISION, x87_full ? "0" : "1");
    if (x87_full) {
      nc_log("hakuX-native", "cpu: FEX with the full 80-bit x87");
    }
  }
  // The reduced x87 keeps the precision a title sets when it is single
  // (FCW.PC; D3D-era titles set it: Burnout 3 hung as a race loaded, its
  // voice sort meeting its own float copy above the double in a register).
  // Translations carry it only once a thread has selected single precision
  // (nc_cpu_x87_single_check): a title that never does runs the plain
  // reduced x87. A/B switch (NC_FEX_PC=0, or debug.hakux.fex_pc=0): every
  // result a double.
  bool x87_pc_off = false;
  {
    const char* e = getenv("NC_FEX_PC");
    char v[PROP_VALUE_MAX] = "";
#ifdef __ANDROID__
    hle_prop_get("debug.hakux.fex_pc", v);
#endif
    x87_pc_off = (e && !strcmp(e, "0")) || !strcmp(v, "0");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_X87REDUCEDPRECISIONCONTROL, x87_pc_off ? "0" : "1");
    if (x87_pc_off) {
      nc_log("hakuX-native", "cpu: FEX's reduced x87 without precision control");
    }
  }
  // The dispatcher's inline block lookup: every system op (kernel calls, HLE
  // hooks: a million a second) returns through the loop top, which without
  // the L2 cache looked the block up in C++, two FPCR writes and all.
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLEL2CACHE, "0");
  // A/B switch (NC_FEX_AFP=0, or debug.hakux.fex_afp=0): FEX without the
  // Arm alternate floating point (FPCR.AH/NEP): no FPCR writes around each
  // host call, a little more code for scalar SSE and min/max.
  {
    const char* e = getenv("NC_FEX_AFP");
    char v[PROP_VALUE_MAX] = "";
#ifdef __ANDROID__
    hle_prop_get("debug.hakux.fex_afp", v);
#endif
    if ((e && !strcmp(e, "0")) || !strcmp(v, "0")) {
      FEXCore::Config::Set(FEXCore::Config::CONFIG_HOSTFEATURES, std::to_string(static_cast<uint64_t>(FEXCore::Config::HostFeatures::DISABLEAFP)));
      nc_log("hakuX-native", "cpu: FEX without AFP (FPCR.AH/NEP)");
    }
  }

  auto* Cpu = new NcCpu {};
  Cpu->Sys.Cfg = *cfg;
  Cpu->X87Watch = !x87_full && !x87_pc_off;
  Cpu->CTX = FEXCore::Context::Context::CreateNewContext(FEX::FetchHostFeatures());
  Cpu->CTX->SetSignalDelegator(&Cpu->Sig);
  Cpu->CTX->SetSyscallHandler(&Cpu->Sys);
  Cpu->CTX->SetGuestMemoryBase(reinterpret_cast<uint64_t>(cfg->membase));
  Cpu->CTX->EnableSystemOps();
  Cpu->CTX->SetIDTRegister(cfg->idt_base, cfg->idt_limit);
  if (cfg->hook_query) {
    Cpu->CTX->SetHookQuery(HookAdapter, Cpu);
  }
  Cpu->CTX->EnableExitOnHLT();
  Cpu->CTX->SetGuestCycleCounterRatio(TscRatioQ32());
  if (!Cpu->CTX->InitCore()) {
    nc_log("hakuX-native", "cpu: InitCore failed");
    delete Cpu;
    return nullptr;
  }

  using FEXCore::Core::CPUState;
  auto* cpu = Cpu;
  cpu->Thread = cpu->CTX->CreateThread();
  auto& S = cpu->Thread->CurrentFrame->State;

  // Call-ret stack with a guard page on each side, as FEX's front ends do.
  constexpr size_t CRS = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  void* Map = mmap(nullptr, CRS + 2 * NC_PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Map == MAP_FAILED || mprotect(static_cast<uint8_t*>(Map) + NC_PAGE, CRS, PROT_READ | PROT_WRITE) != 0) {
    nc_log("hakuX-native", "cpu: no call-ret stack (%s)", strerror(errno));
    return nullptr;
  }
  cpu->Thread->CallRetStackBase = static_cast<uint8_t*>(Map) + NC_PAGE;
  S.callret_sp = reinterpret_cast<uint64_t>(cpu->Thread->CallRetStackBase) + CRS / 4;

  // Flat 32-bit segments everywhere; selector 0x28 is FS at the KPCR. LDT mirrors the GDT.
  for (auto& G : cpu->Gdt) {
    CPUState::SetGDTBase(&G, 0);
    CPUState::SetGDTLimit(&G, 0xF'FFFFu);
    G.L = 0;
    G.D = 1;
    G.P = 1;
    G.S = 1;
    G.G = 1;
  }
  constexpr uint16_t FsSelector = 5 << 3;
  CPUState::SetGDTBase(&cpu->Gdt[FsSelector >> 3], fs_base);
  S.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = cpu->Gdt;
  S.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = cpu->Gdt;
  S.cs_idx = CPUState::DEFAULT_USER_CS << 3;
  S.cs_cached = 0;
  S.fs_idx = FsSelector;
  S.fs_cached = fs_base;

  // Reset state for new guest threads: EFLAGS 0x202 in FEX's representation.
  cpu->CTX->SetFlagsFromCompactedEFLAGS(cpu->Thread, 0x202);
  if (!S.mxcsr) {
    S.mxcsr = 0x1F80;
  }
  Save(S, cpu->Reset);
  return cpu;
}

extern "C" void nc_cpu_run(NcCpu* cpu, const NcCpuContext* c) {
  nc_frame_load(reinterpret_cast<NcFrame*>(cpu->Thread->CurrentFrame), c);
  cpu->CTX->ExecuteThread(cpu->Thread);
}

extern "C" void nc_cpu_invalidate(NcCpu* cpu, uint32_t va, uint32_t len) {
  // Each drops the translations on the range's pages (a long function's
  // multiblocks: milliseconds each to translate again): logged, 10 a second.
  {
    static uint64_t Window;
    static unsigned Logged;
    timespec Ts;
    clock_gettime(CLOCK_MONOTONIC, &Ts);
    const uint64_t Now = static_cast<uint64_t>(Ts.tv_sec) * 1000000000ull + Ts.tv_nsec;
    if (Now - Window > 1000000000ull) {
      Window = Now;
      Logged = 0;
    }
    if (Logged++ < 10) {
      nc_log("hakuX-native", "cpu: translations of %08x+%x dropped (from %p)", va, len, __builtin_return_address(0));
    }
  }
  // The thread's own lookup caches too (its L1/L2 entries and call-return
  // stack): with only the code buffers' dropped, a block entered through an
  // indirect call kept running its old translation. D3D's interrupt handler
  // in push-buffer mode did: its register accesses, hooked since, faulted
  // ~130k times a second. Called on the guest's thread, between blocks.
  std::scoped_lock Lock(cpu->CTX->GetCodeInvalidationMutex());
  cpu->CTX->InvalidateCodeBuffersCodeRange(va, len);
  cpu->CTX->InvalidateThreadCachedCodeRange(cpu->Thread, va, len);
}

extern "C" bool nc_cpu_x87_single_check(NcCpu* cpu) {
  if (!cpu->X87Watch || cpu->X87Single) {
    return false;
  }
  if ((cpu->Thread->CurrentFrame->State.FCW >> 8) & 3) {
    return false; // double or extended: the plain reduced x87 is exact or as close as it gets
  }
  cpu->X87Single = true;
  cpu->CTX->EnableX87SinglePrecision();
  nc_log("hakuX-native", "cpu: the title selected x87 single precision (FCW %04x, seen at %08x): translations round to it",
         cpu->Thread->CurrentFrame->State.FCW, static_cast<uint32_t>(cpu->Thread->CurrentFrame->State.rip));
  nc_cpu_invalidate(cpu, 0, 0xFFFFF000u);
  return true;
}

extern "C" void nc_cpu_request_interrupt(NcCpu* cpu) {
  __atomic_store_n(&cpu->Thread->CurrentFrame->InterruptPending, 1u, __ATOMIC_RELEASE);
}

extern "C" void nc_cpu_cycle_offset_add(NcCpu* cpu, uint64_t ticks) {
  cpu->Thread->CurrentFrame->CycleCounterOffset += ticks;
}

extern "C" uint64_t nc_cpu_host_cycles(void) {
  uint64_t v;
  asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
  return v;
}

extern "C" uint64_t nc_cpu_guest_cycles(NcCpu* cpu) {
  return nc_cpu_host_cycles() - cpu->Thread->CurrentFrame->CycleCounterOffset;
}

extern "C" void nc_cpu_guest_cycles_set(NcCpu* cpu, uint64_t ticks) {
  cpu->Thread->CurrentFrame->CycleCounterOffset = nc_cpu_host_cycles() - ticks;
}

extern "C" bool nc_cpu_x87_single(NcCpu* cpu) {
  return cpu->X87Single;
}

extern "C" void nc_cpu_x87_single_restore(NcCpu* cpu) {
  if (cpu->X87Watch && !cpu->X87Single) {
    cpu->X87Single = true;
    cpu->CTX->EnableX87SinglePrecision();
    nc_log("hakuX-native", "cpu: x87 single precision, as the state's title had selected it");
  }
}

extern "C" void nc_frame_clear_interrupt(NcFrame* f) {
  __atomic_store_n(&F(f).InterruptPending, 0u, __ATOMIC_RELAXED);
}

extern "C" void nc_frame_bypass_hook(NcFrame* f) {
  F(f).HookBypass = static_cast<uint32_t>(F(f).State.rip);
}

extern "C" uint16_t nc_frame_cs(const NcFrame* f) {
  return F(f).State.cs_idx;
}

extern "C" uint32_t nc_frame_eflags(NcFrame* f) {
  return F(f).Thread->CTX->ReconstructCompactedEFLAGS(F(f).Thread, false, nullptr, 0);
}

extern "C" uint64_t* nc_frame_regs(NcFrame* f) {
  return F(f).State.gregs;
}

extern "C" uint64_t* nc_frame_rip(NcFrame* f) {
  return &F(f).State.rip;
}

extern "C" void nc_frame_set_eflags(NcFrame* f, uint32_t eflags) {
  F(f).Thread->CTX->SetFlagsFromCompactedEFLAGS(F(f).Thread, eflags);
}

extern "C" uint32_t nc_cpu_guest_eip_at(NcCpu* cpu, uintptr_t host_pc) {
  if (!cpu || !cpu->Thread) {
    return 0;
  }
  return static_cast<uint32_t>(cpu->CTX->RestoreRIPFromHostPC(cpu->Thread, host_pc));
}

// Both FEX calls only read the thread's frame and the code buffer list.
extern "C" bool nc_cpu_sample_pc(NcCpu* cpu, uintptr_t host_pc, uint32_t* eip) {
  if (!cpu || !cpu->Thread) {
    *eip = 0;
    return false;
  }
  const bool jit = cpu->CTX->IsAddressInCodeBuffer(cpu->Thread, host_pc);
  *eip = static_cast<uint32_t>(jit ? cpu->CTX->RestoreRIPFromHostPC(cpu->Thread, host_pc)
                                   : cpu->Thread->CurrentFrame->State.rip);
  return jit;
}

// A record of the host signal frame's extension area (fpsimd_context, esr_context).
static const _aarch64_ctx* SigRecord(const ucontext_t* uc, uint32_t Magic) {
  const auto* Area = reinterpret_cast<const uint8_t*>(uc->uc_mcontext.__reserved);
  for (size_t Off = 0; Off + sizeof(_aarch64_ctx) <= sizeof(uc->uc_mcontext.__reserved);) {
    const auto* Head = reinterpret_cast<const _aarch64_ctx*>(Area + Off);
    if (Head->magic == Magic) {
      return Head;
    }
    if (!Head->magic || !Head->size) {
      break;
    }
    Off += Head->size;
  }
  return nullptr;
}

extern "C" bool nc_cpu_fault_is_write(const void* ucontext) {
  const auto* Esr = reinterpret_cast<const esr_context*>(SigRecord(static_cast<const ucontext_t*>(ucontext), ESR_MAGIC));
  return Esr && (Esr->esr >> 6) & 1; // ISS.WnR of a data abort
}

// Fixes up ECX and EDI (and ESI for MOVS) when a REP MOVS or REP STOS faults
// partway.
//
// On x86, a fault in the middle of a REP string instruction leaves ECX, ESI
// and EDI at the element that faulted. If a handler maps the page and
// continues, the copy picks up from there.
//
// FEX runs the whole instruction as one MemCpy or MemSet loop instead. The
// loop tracks its progress in TMP2 (x1, the host address of the next
// destination element) and writes ECX, ESI and EDI back only when it
// finishes. At a fault the frame still holds their values from the start of
// the instruction, so resuming would restart the copy from the beginning.
//
// Titles' file streams can depend on this: some commit 64 KiB windows as a
// REP MOVSD reads through them, and decommit the window behind. A copy
// restarted from the beginning faulted on that decommitted window again and
// again.
//
// Only forward copies (DF clear) are handled: the backward loops offset TMP2
// by a chunk, so x1 doesn't map back to an element as simply.
static void RepProgress(NcCpu* cpu, FEXCore::Core::CPUState& S, const ucontext_t* uc, uint32_t EFlags) {
  if (EFlags & (1u << 10)) {
    return;
  }
  const auto* Membase = static_cast<const uint8_t*>(cpu->Sys.Cfg.membase);
  const auto* Insn = Membase + static_cast<uint32_t>(S.rip);
  bool Rep = false, Op16 = false;
  unsigned p = 0;
  for (; p < 8; p++) {
    const uint8_t b = Insn[p];
    if (b == 0xF2 || b == 0xF3) {
      Rep = true;
    } else if (b == 0x66) {
      Op16 = true;
    } else if (b != 0x26 && b != 0x2E && b != 0x36 && b != 0x3E && b != 0x64 && b != 0x65) {
      break;
    }
  }
  const uint8_t Opc = Insn[p];
  const bool Movs = Opc == 0xA4 || Opc == 0xA5;
  if (!Rep || (!Movs && Opc != 0xAA && Opc != 0xAB)) {
    return;
  }
  const uint32_t Size = (Opc & 1) ? (Op16 ? 2 : 4) : 1;
  const uint32_t Edi = static_cast<uint32_t>(S.gregs[FEXCore::X86State::REG_RDI]);
  const uint32_t Ecx = static_cast<uint32_t>(S.gregs[FEXCore::X86State::REG_RCX]);
  const uint32_t Next = static_cast<uint32_t>(uc->uc_mcontext.regs[1] - reinterpret_cast<uintptr_t>(Membase));
  const uint32_t Done = (Next - Edi) / Size;
  if (Done == 0 || Done > Ecx) {
    return;
  }
  S.gregs[FEXCore::X86State::REG_RDI] = Edi + Done * Size;
  S.gregs[FEXCore::X86State::REG_RCX] = Ecx - Done;
  if (Movs) {
    S.gregs[FEXCore::X86State::REG_RSI] = static_cast<uint32_t>(S.gregs[FEXCore::X86State::REG_RSI]) + Done * Size;
  }
}

// As FEX's front ends do before they deliver a guest signal: the guest PC from the
// host PC, the statically allocated registers and the flags from the host context.
extern "C" NcFrame* nc_cpu_fault_frame(NcCpu* cpu, void* ucontext) {
  auto* Thread = cpu ? cpu->Thread : nullptr;
  auto* uc = static_cast<ucontext_t*>(ucontext);
  if (!Thread || !cpu->CTX->IsAddressInCodeBuffer(Thread, uc->uc_mcontext.pc) || Thread->CurrentFrame->InSyscallInfo) {
    return nullptr;
  }
  const auto* Fp = reinterpret_cast<const fpsimd_context*>(SigRecord(uc, FPSIMD_MAGIC));
  if (!Fp) {
    return nullptr;
  }
  const auto& Config = cpu->Sig.GetConfig();
  auto& S = Thread->CurrentFrame->State;
  S.rip = cpu->CTX->RestoreRIPFromHostPC(Thread, uc->uc_mcontext.pc);
  for (size_t i = 0; i < Config.SRAGPRCount; i++) {
    S.gregs[i] = uc->uc_mcontext.regs[Config.SRAGPRMapping[i]];
  }
  for (size_t i = 0; i < Config.SRAFPRCount; i++) {
    memcpy(&S.xmm.sse.data[i][0], &Fp->vregs[Config.SRAFPRMapping[i]], sizeof(__uint128_t));
  }
  const uint32_t EFlags = cpu->CTX->ReconstructCompactedEFLAGS(
    Thread, true, reinterpret_cast<const uint64_t*>(uc->uc_mcontext.regs), uc->uc_mcontext.pstate);
  cpu->CTX->SetFlagsFromCompactedEFLAGS(Thread, EFlags);
  RepProgress(cpu, S, uc, EFlags);
  return reinterpret_cast<NcFrame*>(Thread->CurrentFrame);
}

// Back to the dispatcher's loop top on its own stack, which refills the registers
// from the frame and runs from its EIP.
extern "C" void nc_cpu_fault_resume(NcCpu* cpu, void* ucontext) {
  auto& mc = static_cast<ucontext_t*>(ucontext)->uc_mcontext;
  mc.pc = cpu->Sig.GetConfig().AbsoluteLoopTopAddressFillSRA;
  mc.sp = cpu->Thread->CurrentFrame->ReturningStackLocation;
  mc.regs[1] = 0; // ENTRY_FILL_SRA_SINGLE_INST_REG: a normal block
}

// As FEX's Linux front end does (SignalDelegator's SIGBUS handler): ARMv8.0-8.3
// atomics fault on an address their size doesn't divide, where x86's locked
// instructions don't. FEX emulates the access, or backpatches a load/store to a
// plain one, and says how far the PC moves. TSO is off: plain accesses then.
extern "C" bool nc_cpu_unaligned_fault(NcCpu* cpu, int code, void* ucontext) {
  auto* uc = static_cast<ucontext_t*>(ucontext);
  if (code != BUS_ADRALN || !cpu || !cpu->Thread || !cpu->CTX->IsAddressInCodeBuffer(cpu->Thread, uc->uc_mcontext.pc)) {
    return false;
  }
  const auto Moved = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
    cpu->Thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::NonAtomic, uc->uc_mcontext.pc,
    reinterpret_cast<uint64_t*>(uc->uc_mcontext.regs));
  if (!Moved) {
    return false;
  }
  uc->uc_mcontext.pc += *Moved;
  return true;
}

extern "C" bool nc_cpu_callret_fault(NcCpu* cpu, uintptr_t fault_addr, void* ucontext) {
  if (!cpu || !cpu->Thread || !cpu->Thread->CallRetStackBase) {
    return false;
  }
  constexpr size_t CRS = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  const uintptr_t Base = reinterpret_cast<uintptr_t>(cpu->Thread->CallRetStackBase);
  if (fault_addr < Base - NC_PAGE || fault_addr >= Base + CRS + NC_PAGE) {
    return false;
  }
  // REG_CALLRET_SP (x25) back to the default location, leaving room both ways.
  static_cast<ucontext_t*>(ucontext)->uc_mcontext.regs[25] = Base + CRS / 4;
  return true;
}
