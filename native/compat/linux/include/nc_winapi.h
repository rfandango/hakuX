/*
 * xbased on Linux x86-64: the few Windows APIs the x86-64 translator and
 * its instruction emulator use (cpu/cpu_x86.cpp, cpu/x64emu.cpp,
 * core/fault_win.c), over POSIX. Memory: mmap/mprotect; vectored exception
 * handlers: one SIGSEGV/SIGBUS handler (seh.c) that hands them a CONTEXT
 * made from the signal's ucontext and puts it back. The CONTEXT keeps
 * Windows' register order (Rax..R15 contiguous): code indexes it so.
 */
#ifndef NC_WINAPI_H
#define NC_WINAPI_H

#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t DWORD;
typedef uint64_t DWORD64;
typedef int32_t LONG;
typedef int64_t LONG64;
typedef uint32_t ULONG;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t SIZE_T;
typedef int BOOL;
typedef void *HANDLE;
typedef void *PVOID;
typedef const void *LPCVOID;
typedef void *LPVOID;
#define TRUE 1
#define FALSE 0
#define WINAPI
#define CALLBACK

/* FXSAVE's image, as Windows names its fields */
typedef struct XMM_SAVE_AREA32 {
    uint16_t ControlWord, StatusWord;
    uint8_t TagWord, Reserved1;
    uint16_t ErrorOpcode;
    uint32_t ErrorOffset;
    uint16_t ErrorSelector, Reserved2;
    uint32_t DataOffset;
    uint16_t DataSelector, Reserved3;
    uint32_t MxCsr, MxCsr_Mask;
    uint8_t rest[480];
} __attribute__((aligned(16))) XMM_SAVE_AREA32;
_Static_assert(sizeof(XMM_SAVE_AREA32) == 512, "the FXSAVE image is 512 bytes");

typedef struct CONTEXT {
    DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15;
    DWORD64 Rip;
    DWORD EFlags;
    DWORD ContextFlags;
    DWORD MxCsr;                            /* FltSave.MxCsr's copy, as Windows keeps it */
    XMM_SAVE_AREA32 FltSave;                /* FXSAVE layout (the kernel's fpstate) */
} CONTEXT, *PCONTEXT;

#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0

typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(PEXCEPTION_POINTERS);
/* seh.c: the handlers, first ones in front; SIGSEGV and SIGBUS go to them */
PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);
ULONG RemoveVectoredExceptionHandler(PVOID h);

/* memory */
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40

static inline int nc_prot(DWORD p)
{
    switch (p) {
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_NONE;
    }
}

static inline LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
    const int pr = (type & MEM_COMMIT) ? nc_prot(prot) : PROT_NONE;
    void *p = mmap(addr, size, pr, MAP_PRIVATE | MAP_ANONYMOUS | (addr ? MAP_FIXED : 0) | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static inline BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    (void)type;
    return munmap(addr, size) == 0;
}

static inline BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD *old)
{
    if (old) {
        *old = 0;
    }
    return mprotect(addr, size, nc_prot(prot)) == 0;
}

typedef struct MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    SIZE_T RegionSize;
    DWORD State;
} MEMORY_BASIC_INFORMATION;

/* Whether the page holding addr is mapped (mincore fails on what isn't),
 * whatever its protection: Windows' MEM_COMMIT, as the translator asks. */
static inline SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len)
{
    (void)len;
    unsigned char v;
    void *page = (void *)((uintptr_t)addr & ~(uintptr_t)0xFFF);
    mbi->BaseAddress = page;
    mbi->RegionSize = 0x1000;
    mbi->State = mincore(page, 0x1000, &v) == 0 ? MEM_COMMIT : 0;
    return sizeof(*mbi);
}

static inline DWORD GetLastError(void)
{
    return (DWORD)errno;
}

static inline void *_aligned_malloc(size_t size, size_t align)
{
    void *p = NULL;
    return posix_memalign(&p, align, size) == 0 ? p : NULL;
}

static inline void _aligned_free(void *p)
{
    free(p);
}

static inline HANDLE GetCurrentProcess(void)
{
    return (HANDLE)-1;
}

static inline BOOL FlushInstructionCache(HANDLE p, LPCVOID a, SIZE_T n)
{
    (void)p;
    (void)a;
    (void)n;
    return TRUE;    /* x86's instruction fetch is coherent with stores */
}

/* locks, atomics, threads, time */
typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void AcquireSRWLockExclusive(SRWLOCK *l)
{
    pthread_mutex_lock(l);
}
static inline void ReleaseSRWLockExclusive(SRWLOCK *l)
{
    pthread_mutex_unlock(l);
}

static inline LONG64 InterlockedIncrement64(volatile LONG64 *v)
{
    return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST);
}

static inline DWORD GetCurrentThreadId(void)
{
    return (DWORD)syscall(SYS_gettid);
}

static inline void Sleep(DWORD ms)
{
    usleep((useconds_t)ms * 1000u);
}

static inline uint64_t GetTickCount64(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

typedef union LARGE_INTEGER {
    int64_t QuadPart;
} LARGE_INTEGER;

static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f)
{
    f->QuadPart = 1000000000;
    return TRUE;
}

static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    c->QuadPart = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
    return TRUE;
}

#ifdef __cplusplus
}
#endif

#endif
