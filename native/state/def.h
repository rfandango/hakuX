/*
 * hakuX native core — expands a section's field tables (a .def file) in the
 * module that owns its structs:
 *
 *   #define NC_DEF_SECTION TIMR                  (lock.h's name)
 *   #define NC_DEF_FILE "native/state/timer.def"
 *   #include "native/state/def.h"
 *
 * A .def file lists records:
 *
 *   NC_SECTION(TIMR)                             first: the section (lock.h's name)
 *   NC_REC(Type, size)                           Type's record; size: sizeof(Type)
 *                                                (tools/native/statelock.py --sizes fills it in)
 *   NC_F(Type, member, U32, count, since, until) a stored member: element type, count,
 *                                                the section versions it is stored in
 *   NC_R(Type, member, Rec, count, since, until) a member of record type Rec (defined above)
 *   NC_SKIP(Type, member)                        a member not stored (host state, rebuilt)
 *   NC_END(Type)
 *
 * and gets, per record, nc_rec_<Type> (state.h's NcRecord). The build stops
 * when a struct's size or a stored member's type changed (list the member,
 * stored or skipped), or when the tables no longer match lock.h's layout
 * hash for the section (run tools/native/statelock.py: it bumps the
 * section's version and logs it in native/state/CHANGELOG.md).
 */
#include "native/state/state.h"

#include <stddef.h>

/* Skipped host synchronization objects have different Bionic/glibc sizes.
 * Both sizes remain checked; serialized field types and hashes are unchanged. */
#ifndef NC_ABI_SIZE
#ifdef __ANDROID__
#define NC_ABI_SIZE(android_size, linux_size) (android_size)
#else
#define NC_ABI_SIZE(android_size, linux_size) (linux_size)
#endif
#endif

#if defined(NC_STATE_PROBE) && !defined(NC_STATE_PROBE_ABI)
#define NC_STATE_PROBE_ABI
#ifdef __ANDROID__
const unsigned long nc_probe_host_abi = 1;
#else
const unsigned long nc_probe_host_abi = 2;
#endif
#endif

#define NC_DEF_CAT2(a, b) a##b
#define NC_DEF_CAT(a, b)  NC_DEF_CAT2(a, b)
#define NC_SECTION(name)

/* checks: sizes and members (NC_STATE_PROBE: statelock.py --sizes reads the
 * sizes from the assembly instead) */
#ifdef NC_STATE_PROBE
#define NC_REC(T, size) const unsigned long nc_probe_size_##T = sizeof(T);
#else
#define NC_REC(T, size)                                                                            \
    _Static_assert(sizeof(T) == (size), #T " changed size: list its new member in its .def (NC_F or " \
                                           "NC_SKIP), then tools/native/statelock.py --sizes");
#endif
#define NC_F(T, m, ty, n, s, u)                                                                    \
    _Static_assert(sizeof(((T *)0)->m) == NC_ESIZE_##ty * (n), #T "." #m ": the .def's type or count " \
                                                               "is not the member's");
#define NC_R(T, m, R, n, s, u)                                                                     \
    _Static_assert(sizeof(((T *)0)->m) == sizeof(R) * (n), #T "." #m ": the .def's count is not the member's");
#define NC_SKIP(T, m) _Static_assert(sizeof(((T *)0)->m) > 0, #T "." #m);
#define NC_END(T)
#include NC_DEF_FILE
#undef NC_REC
#undef NC_F
#undef NC_R
#undef NC_SKIP
#undef NC_END

/* the tables */
#define NC_REC(T, size) static const NcField nc_fields_##T[] = {
#define NC_F(T, m, ty, n, s, u) {#m, offsetof(T, m), NC_FT_##ty, NC_ESIZE_##ty, (n), (s), (u), NULL},
#define NC_R(T, m, R, n, s, u)  {#m, offsetof(T, m), NC_FT_REC, sizeof(R), (n), (s), (u), &nc_rec_##R},
#define NC_SKIP(T, m)
#define NC_END(T)                                                                                  \
    };                                                                                             \
    static const NcRecord nc_rec_##T __attribute__((unused)) = {                                   \
        #T, nc_fields_##T, sizeof(nc_fields_##T) / sizeof(NcField)};
#include NC_DEF_FILE
#undef NC_REC
#undef NC_F
#undef NC_R
#undef NC_SKIP
#undef NC_END

/* each record's layout hash */
#define NC_REC(T, size) enum { nc_layout_##T = (int)((0u
#define NC_F(T, m, ty, n, s, u) + NC_FH(NC_FT_##ty, NC_ESIZE_##ty, n, s, u, sizeof(#m))
#define NC_R(T, m, R, n, s, u)  + NC_FH(NC_FT_REC, (uint32_t)nc_layout_##R, n, s, u, sizeof(#m))
#define NC_SKIP(T, m)
#define NC_END(T) ) & 0x7FFFFFFFu) };
#include NC_DEF_FILE
#undef NC_REC
#undef NC_F
#undef NC_R
#undef NC_SKIP
#undef NC_END

/* the section's: its records' */
#define NC_REC(T, size) + (uint32_t)nc_layout_##T
#define NC_F(T, m, ty, n, s, u)
#define NC_R(T, m, R, n, s, u)
#define NC_SKIP(T, m)
#define NC_END(T)
enum { NC_DEF_CAT(nc_layout_sec_, NC_DEF_SECTION) = (int)((0u
#include NC_DEF_FILE
    ) & 0x7FFFFFFFu) };
#undef NC_REC
#undef NC_F
#undef NC_R
#undef NC_SKIP
#undef NC_END

#ifndef NC_STATE_PROBE
_Static_assert((uint32_t)NC_DEF_CAT(nc_layout_sec_, NC_DEF_SECTION) == NC_DEF_CAT(NC_STATE_H_, NC_DEF_SECTION),
               "a saved struct changed: run tools/native/statelock.py (it bumps the section's version)");
#endif

#undef NC_SECTION
#undef NC_DEF_CAT
#undef NC_DEF_CAT2
