---
name: Xbox TCG ARM64 Optimizations
overview: "Implement Xbox/Pentium III-specific TCG optimizations targeting the ARM64 backend to improve xemu emulation performance on Android devices. Focuses on the five highest-impact areas: flag computation elimination, x87 FPU acceleration, MMX/SSE1 NEON lowering, TLB fast-path optimization, and profiling infrastructure."
todos:
  - id: profiling
    content: Build TCG profiling plugin to identify hot x86 opcodes, CC_OP distribution, and helper call frequency in Xbox game workloads
    status: cancelled
  - id: ram-fastpath
    content: Implement direct memory mapping fast path for Xbox RAM region (0 to 128MB) in the ARM64 TLB code generator, bypassing softmmu for known-RAM addresses
    status: cancelled
  - id: hard-fpu
    content: Enable and implement ARM64 hard FPU path for x87 operations (FADD/FSUB/FMUL/FDIV/FSQRT/FCOM) using native double-precision, gated behind a config option
    status: completed
  - id: flag-fusion
    content: Add setcond+brcond fusion peephole in tcg/optimize.c and CMP+branch fusion in the ARM64 backend to eliminate redundant flag materialization
    status: completed
  - id: neon-helpers
    content: Replace scalar MMX/SSE1 helper loops with ARM64 NEON intrinsics for high-frequency operations (PADD/PSUB/PMUL/PAND/POR/PXOR/ADDPS/MULPS)
    status: completed
  - id: dead-cc-stores
    content: Add dead-store elimination for cc_op/cc_dst/cc_src writes in tcg/optimize.c when next instruction overwrites without reading
    status: cancelled
  - id: inline-vec-ops
    content: Emit TCG vector ops directly for simple SIMD operations (PAND/POR/PXOR/PADDB) in emit.c.inc instead of calling helpers
    status: completed
---

# Xbox Pentium III TCG ARM64 Optimization Plan

This plan targets the five highest-impact optimization areas for running original Xbox games on ARM64 devices, ordered by estimated performance impact.

## 0. Profiling Infrastructure (Do This First)

Before optimizing, instrument TCG to identify actual hot paths in Xbox game workloads.

**Approach:** Use QEMU's existing TCG plugin infrastructure to build a lightweight profiling plugin that logs:

- Most frequently executed translation blocks (by guest PC)
- Most frequently translated x86 opcodes
- CC_OP distribution (which flag operations dominate)
- Helper call frequency (which softfloat/MMX/SSE helpers are hot)

**Files:**

- Create a new plugin in `plugins/` or use the existing `contrib/plugins/` pattern
- Hook into `accel/tcg/plugin-gen.c` callbacks

**Why first:** Without profiling data from real Xbox games, optimizations risk targeting cold paths. The Pentium III subset is small but game workloads may cluster on specific patterns.

---

## 1. Flag Computation Elimination (Highest Impact)

**Problem:** The x86 frontend uses lazy flag evaluation via `cc_op` (stores CC_DST, CC_SRC, CC_OP and only computes EFLAGS on demand). This is already good, but common patterns still generate excessive TCG IR:

- `CMP + Jcc` sequences generate: store to CC_DST, store to CC_SRC, set CC_OP, then on the Jcc read back CC_DST/CC_SRC, compute condition, branch
- `TEST + Jcc` sequences have similar overhead
- `SUB + Jcc` (where SUB result is discarded) generates full flag materialization

On ARM64, a `CMP + B.cond` is a single comparison + conditional branch (2 instructions). Currently TCG generates 8-12 instructions for the same pattern.

**Key files:**

- `target/i386/tcg/translate.c` -- `gen_prepare_eflags_c/z/s` (lines 1018-1164) already have per-CC_OP fast paths
- `target/i386/tcg/emit.c.inc` -- instruction emission, where CMP/TEST/Jcc patterns originate
- `tcg/optimize.c` -- the TCG optimization pass (line 3098: `tcg_optimize()`)
- `tcg/aarch64/tcg-target.c.inc` -- ARM64 backend, `tgen_brcondi` (line 1428) already has CBZ/CBNZ peepholes

**Implementation:**

### 1a. TCG IR peephole: fuse setcond + brcond

Add a pass in `tcg/optimize.c` that detects:

```
setcond tmp, a, b, cond
brcond tmp, 0, NE, label
```

And fuses to:

```
brcond a, b, cond, label
```

This eliminates the intermediate boolean materialization that the x86 frontend generates for conditional jumps.

### 1b. Eliminate dead CC_OP stores

In `tcg/optimize.c`, add dead-store elimination for writes to `cc_op`, `cc_dst`, `cc_src` TCG globals when the next instruction overwrites them without reading. The existing dead code elimination handles temps but not stores to CPU state (which are treated as potentially observable side effects). For the known `cc_*` fields, if no helper call or TB exit intervenes, the store is dead.

### 1c. ARM64 backend: fuse CMP/SUB + conditional branch

In `tcg/aarch64/tcg-target.c.inc`, extend the existing peephole in `tgen_brcondi` to also fuse:

- `sub_i32 dst, a, b` followed by `brcond dst, 0, cond, label` into `SUBS + B.cond` (eliminating the dead DST when only flags are needed)

This requires tracking the last emitted instruction in the ARM64 backend to recognize fusible sequences.

---

## 2. x87 FPU ARM64 Native Float Acceleration

**Problem:** x87 operations go through QEMU's softfloat library on ARM64. The hard FPU path in `fpu_helper.c` (line 76) is gated behind `#if defined(XBOX) && defined(__x86_64__)` -- only enabled on x86_64 hosts. On ARM64, every FADD/FMUL/FDIV/FSQRT is a function call into softfloat.

**Key files:**

- `target/i386/tcg/fpu_helper.c` -- all x87 helpers (3517 lines)
- `target/i386/tcg/fpu_helper_hard.c` -- hard FPU wrappers (x86_64 only)
- `target/i386/ops_fpu.h` -- FPU operation macros

**Implementation:**

### 2a. Enable hard FPU path for ARM64

Extend the `#if` guard in `fpu_helper.c` (line 76) to include `__aarch64__`:

```c
#if defined(XBOX) && (defined(__x86_64__) || defined(__aarch64__))
```

ARM64's FP unit supports IEEE 754 double precision natively. The `floatx80` (80-bit extended precision) used by x87 will need to be rounded to `double` (64-bit) for ARM64 native operations, which introduces precision differences.

**Important caveat:** x87 uses 80-bit extended precision internally. ARM64 only has 64-bit double. For most Xbox games this is acceptable (games typically use `float`/`double` via the compiler, not raw x87 extended precision), but the rounding behavior will differ. This should be gated behind a configuration option (e.g., `g_config.perf.fast_fpu`).

### 2b. Implement ARM64 hard FPU wrappers

Create ARM64-specific wrappers that:

1. Convert `floatx80` to `double` (truncation)
2. Perform the operation using ARM64 native FP (`FADD`, `FMUL`, `FDIV`, `FSQRT` instructions via C operators)
3. Convert result back to `floatx80`
4. Handle rounding mode changes (ARM64 FPCR vs x87 control word)

The hot operations to target: `FADD`, `FSUB`, `FMUL`, `FDIV`, `FSQRT`, `FCOM`/`FCOMP` (comparisons), `FLD`/`FST` (loads/stores already avoid softfloat).

---

## 3. MMX/SSE1 NEON Lowering

**Problem:** All MMX and SSE1 helpers in `target/i386/ops_sse.h` are implemented as scalar C loops. For example, `PADDB` (packed add bytes) iterates byte-by-byte. ARM64 NEON can do this in a single instruction (`ADD V0.16B, V1.16B, V2.16B`).

**Key files:**

- `target/i386/ops_sse.h` (2677 lines) -- all MMX/SSE helper implementations
- `target/i386/tcg/ops_sse_header.h.inc` -- helper declarations
- `target/i386/tcg/emit.c.inc` -- where helpers are called during translation

**Implementation:**

### 3a. NEON-accelerated MMX/SSE helpers

For the most common operations used by Xbox games, provide `#ifdef __aarch64__` alternatives using ARM64 NEON intrinsics (`<arm_neon.h>`). Target these first (highest frequency in game workloads):

**Arithmetic:**

- `PADDB/W/D` (packed add) -> `vaddq_u8/u16/u32`
- `PSUBB/W/D` (packed sub) -> `vsubq_u8/u16/u32`
- `PMULLW` (packed multiply low) -> `vmulq_s16`
- `ADDPS` (SSE1 packed float add) -> `vaddq_f32`
- `MULPS` (SSE1 packed float mul) -> `vmulq_f32`

**Comparison/Logic:**

- `PCMPEQB/W/D` -> `vceqq_u8/u16/u32`
- `PCMPGTB/W/D` -> `vcgtq_s8/s16/s32`
- `PAND/POR/PXOR` -> `vandq_u64/vorrq_u64/veorq_u64`
- `CMPPS` (SSE1) -> `vceqq_f32/vcltq_f32/vcleq_f32`

**Shuffle/Pack:**

- `PUNPCKLBW/WD/DQ` -> `vzip1q_*`
- `PUNPCKHBW/WD/DQ` -> `vzip2q_*`
- `PACKSSWB/PACKUSWB` -> `vqmovn_s16/vqmovun_s16`

### 3b. Inline TCG generation for simple ops

For the simplest cases (PAND, POR, PXOR, PADDB), instead of calling helpers, emit TCG vector ops directly in `emit.c.inc`. TCG already has vector opcodes (`INDEX_op_vec_add`, etc.) and the ARM64 backend can lower them to NEON instructions directly. This avoids the helper call overhead entirely.

---

## 4. Xbox Memory Map TLB Optimization

**Problem:** The Xbox has a fixed, small memory map (64MB or 128MB RAM at address 0). QEMU's softmmu TLB is designed for arbitrary 64-bit address spaces. The TLB lookup for every memory access involves hash computation, comparison, and potential slow-path calls.

**Key files:**

- `accel/tcg/cputlb.c` -- TLB implementation (`mmu_lookup1` at line 1650)
- `tcg/aarch64/tcg-target.c.inc` -- ARM64 TLB fast path code generation (prologue at line 3450 references `TCG_REG_GUEST_BASE`)
- `hw/xbox/xbox.c` -- Xbox memory init (line 176: RAM mapped at 0)

**Implementation:**

### 4a. Direct memory mapping for Xbox RAM region

Since Xbox RAM is a contiguous region starting at address 0, and xemu already allocates it as a single `MemoryRegion`, we can map the entire guest RAM into the host process and use `guest_base + masked_address` for direct access, bypassing the TLB entirely for addresses in the RAM range.

In `tcg/aarch64/tcg-target.c.inc`, for `qemu_ld`/`qemu_st` operations, add an Xbox-specific fast path:

```
// If addr < ram_size (128MB = 0x8000000):
//   host_addr = guest_base + addr
// Else:
//   fall through to normal TLB lookup
```

On ARM64 this is: `CMP addr, #ram_size; B.HS slow_path; LDR result, [guest_base, addr]` -- 3 instructions vs. the current ~15 for a full TLB lookup.

### 4b. Reduce TLB lookup for MMIO

For addresses above the RAM region (NV2A registers at `0xFD000000`, PCI config space, etc.), keep the standard TLB path but tune the TLB size and hash function for the Xbox's known MMIO regions.

---

## 5. Implementation Order and Estimates

| Phase | Task                      | Est. Impact                   | Est. Effort |

| ----- | ------------------------- | ----------------------------- | ----------- |

| 0     | Profiling plugin          | Enables data-driven decisions | 1-2 days    |

| 1a    | setcond+brcond fusion     | 5-10% overall                 | 2-3 days    |

| 1b    | Dead CC store elimination | 3-5% overall                  | 3-5 days    |

| 2a    | ARM64 hard FPU enable     | 10-20% in FPU-heavy scenes    | 2-3 days    |

| 3a    | NEON MMX/SSE helpers      | 5-15% in SIMD-heavy code      | 5-7 days    |

| 4a    | Direct RAM mapping        | 10-20% overall                | 3-5 days    |

| 1c    | ARM64 CMP+branch fusion   | 3-5% overall                  | 3-5 days    |

| 2b    | ARM64 hard FPU wrappers   | Part of 2a                    | Included    |

| 3b    | Inline TCG vector ops     | 2-5% in SIMD code             | 3-5 days    |

| 4b    | TLB tuning for MMIO       | 1-3% in MMIO-heavy scenes     | 1-2 days    |

**Recommended order:** 0 -> 4a -> 2a -> 1a -> 3a -> 1b -> 1c -> 3b -> 4b

Start with profiling (0), then direct RAM mapping (4a) and hard FPU (2a) for the biggest wins with the least complexity. Follow with flag optimization (1a) and NEON helpers (3a) for the next tier.