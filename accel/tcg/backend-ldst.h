/*
 * Internal memory barrier helpers for QEMU (target agnostic)
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef ACCEL_TCG_BACKEND_LDST_H
#define ACCEL_TCG_BACKEND_LDST_H

#include "tcg-target-mo.h"

/**
 * tcg_req_mo:
 * @guest_mo: Guest default memory order
 * @type: TCGBar
 *
 * Filter @type to the barrier that is required for the guest
 * memory ordering vs the host memory ordering.  A non-zero
 * result indicates that some barrier is required.
 */
#define tcg_req_mo(guest_mo, type) \
    ((type) & guest_mo & ~TCG_TARGET_DEFAULT_MO)

/**
 * cpu_req_mo:
 * @cpu: CPUState
 * @type: TCGBar
 *
 * If tcg_req_mo indicates a barrier for @type is required
 * for the guest memory model, issue a host memory barrier.
 *
 * Xbox has a single CPU — inter-vCPU memory ordering barriers are
 * never needed, so we compile them out entirely.
 */
#ifdef XBOX
#define cpu_req_mo(cpu, type)  do { (void)(cpu); } while (0)
#else
#define cpu_req_mo(cpu, type)                                           \
    do {                                                                \
        unsigned _mo = tcg_req_mo(                                      \
            cpu->cc->tcg_ops->guest_default_memory_order, type);        \
        if (_mo & (TCG_MO_ST_ST | TCG_MO_ST_LD)) {                     \
            smp_mb();                                                   \
        } else if (_mo) {                                               \
            smp_rmb();                                                  \
        }                                                               \
    } while (0)
#endif

#endif
