/*
 *  Host code generation
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include "trace.h"
#include "disas/disas.h"
#include "tcg/tcg.h"
#include "exec/mmap-lock.h"
#include "tb-internal.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "qemu/cacheinfo.h"
#include "qemu/target-info.h"
#include "exec/log.h"
#include "exec/icount.h"
#include "accel/tcg/cpu-ops.h"
#include "tb-jmp-cache.h"
#include "tb-hash.h"
#include "tb-context.h"
#include "tb-internal.h"
#include "internal-common.h"
#include "tcg/perf.h"
#include "tcg/insn-start-words.h"
#include "tb-cache-hints.h"
#ifdef XBOX
#include "tb-code-hash.h"
#ifndef TCG_HIGHWATER
#define TCG_HIGHWATER 1024
#endif
#endif

#if defined(CONFIG_VTUNE_JITPROFILING)
#include <jitprofiling.h>
#endif

TBContext tb_ctx;

/*
 * Encode VAL as a signed leb128 sequence at P.
 * Return P incremented past the encoded value.
 */
static uint8_t *encode_sleb128(uint8_t *p, int64_t val)
{
    int more, byte;

    do {
        byte = val & 0x7f;
        val >>= 7;
        more = !((val == 0 && (byte & 0x40) == 0)
                 || (val == -1 && (byte & 0x40) != 0));
        if (more) {
            byte |= 0x80;
        }
        *p++ = byte;
    } while (more);

    return p;
}

/*
 * Decode a signed leb128 sequence at *PP; increment *PP past the
 * decoded value.  Return the decoded value.
 */
static int64_t decode_sleb128(const uint8_t **pp)
{
    const uint8_t *p = *pp;
    int64_t val = 0;
    int byte, shift = 0;

    do {
        byte = *p++;
        val |= (int64_t)(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) {
        val |= -(int64_t)1 << shift;
    }

    *pp = p;
    return val;
}

/* Encode the data collected about the instructions while compiling TB.
   Place the data at BLOCK, and return the number of bytes consumed.

   The logical table consists of INSN_START_WORDS uint64_t's,
   which come from the target's insn_start data, followed by a uintptr_t
   which comes from the host pc of the end of the code implementing the insn.

   Each line of the table is encoded as sleb128 deltas from the previous
   line.  The seed for the first line is { tb->pc, 0..., tb->tc.ptr }.
   That is, the first column is seeded with the guest pc, the last column
   with the host pc, and the middle columns with zeros.  */

static int encode_search(TranslationBlock *tb, uint8_t *block)
{
    uint8_t *highwater = tcg_ctx->code_gen_highwater;
    uint64_t *insn_data = tcg_ctx->gen_insn_data;
    uint16_t *insn_end_off = tcg_ctx->gen_insn_end_off;
    uint8_t *p = block;
    int i, j, n;

    for (i = 0, n = tb->icount; i < n; ++i) {
        uint64_t prev, curr;

        for (j = 0; j < INSN_START_WORDS; ++j) {
            if (i == 0) {
                prev = (!(tb_cflags(tb) & CF_PCREL) && j == 0 ? tb->pc : 0);
            } else {
                prev = insn_data[(i - 1) * INSN_START_WORDS + j];
            }
            curr = insn_data[i * INSN_START_WORDS + j];
            p = encode_sleb128(p, curr - prev);
        }
        prev = (i == 0 ? 0 : insn_end_off[i - 1]);
        curr = insn_end_off[i];
        p = encode_sleb128(p, curr - prev);

        /* Test for (pending) buffer overflow.  The assumption is that any
           one row beginning below the high water mark cannot overrun
           the buffer completely.  Thus we can test for overflow after
           encoding a row without having to check during encoding.  */
        if (unlikely(p > highwater)) {
            return -1;
        }
    }

    return p - block;
}

static int cpu_unwind_data_from_tb(TranslationBlock *tb, uintptr_t host_pc,
                                   uint64_t *data)
{
    uintptr_t iter_pc = (uintptr_t)tb->tc.ptr;
    const uint8_t *p = tb->tc.ptr + tb->tc.size;
    int i, j, num_insns = tb->icount;

    host_pc -= GETPC_ADJ;

    if (host_pc < iter_pc) {
        return -1;
    }

    memset(data, 0, sizeof(uint64_t) * INSN_START_WORDS);
    if (!(tb_cflags(tb) & CF_PCREL)) {
        data[0] = tb->pc;
    }

    /*
     * Reconstruct the stored insn data while looking for the point
     * at which the end of the insn exceeds host_pc.
     */
    for (i = 0; i < num_insns; ++i) {
        for (j = 0; j < INSN_START_WORDS; ++j) {
            data[j] += decode_sleb128(&p);
        }
        iter_pc += decode_sleb128(&p);
        if (iter_pc > host_pc) {
            return num_insns - i;
        }
    }
    return -1;
}

/*
 * The cpu state corresponding to 'host_pc' is restored in
 * preparation for exiting the TB.
 */
void cpu_restore_state_from_tb(CPUState *cpu, TranslationBlock *tb,
                               uintptr_t host_pc)
{
    uint64_t data[INSN_START_WORDS];
    int insns_left = cpu_unwind_data_from_tb(tb, host_pc, data);

    if (insns_left < 0) {
        return;
    }

    if (tb_cflags(tb) & CF_USE_ICOUNT) {
        assert(icount_enabled());
        /*
         * Reset the cycle counter to the start of the block and
         * shift if to the number of actually executed instructions.
         */
        cpu->neg.icount_decr.u16.low += insns_left;
    }

    cpu->cc->tcg_ops->restore_state_to_opc(cpu, tb, data);
}

bool cpu_restore_state(CPUState *cpu, uintptr_t host_pc)
{
    /*
     * The host_pc has to be in the rx region of the code buffer.
     * If it is not we will not be able to resolve it here.
     * The two cases where host_pc will not be correct are:
     *
     *  - fault during translation (instruction fetch)
     *  - fault from helper (not using GETPC() macro)
     *
     * Either way we need return early as we can't resolve it here.
     */
    if (in_code_gen_buffer((const void *)(host_pc - tcg_splitwx_diff))) {
        TranslationBlock *tb = tcg_tb_lookup(host_pc);
        if (tb) {
            cpu_restore_state_from_tb(cpu, tb, host_pc);
            return true;
        }
    }
    return false;
}

bool cpu_unwind_state_data(CPUState *cpu, uintptr_t host_pc, uint64_t *data)
{
    if (in_code_gen_buffer((const void *)(host_pc - tcg_splitwx_diff))) {
        TranslationBlock *tb = tcg_tb_lookup(host_pc);
        if (tb) {
            return cpu_unwind_data_from_tb(tb, host_pc, data) >= 0;
        }
    }
    return false;
}

void page_init(void)
{
    page_table_config_init();
}

#ifdef XBOX
static bool tb_pc_cmp(const void *p, const void *d)
{
    const TranslationBlock *tb = p;
    const vaddr *target = d;
    return !(tb_cflags(tb) & CF_INVALID) && tb->pc == *target;
}
#endif

/*
 * Isolate the portion of code gen which can setjmp/longjmp.
 * Return the size of the generated code, or negative on error.
 */
static int setjmp_gen_code(CPUArchState *env, TranslationBlock *tb,
                           vaddr pc, void *host_pc,
                           int *max_insns, int64_t *ti)
{
    int ret = sigsetjmp(tcg_ctx->jmp_trans, 0);
    if (unlikely(ret != 0)) {
        return ret;
    }

    tcg_func_start(tcg_ctx);

    CPUState *cs = env_cpu(env);
    tcg_ctx->cpu = cs;
    cs->cc->tcg_ops->translate_code(cs, tb, max_insns, pc, host_pc);

    assert(tb->size != 0);
    tcg_ctx->cpu = NULL;
    *max_insns = tb->icount;

#ifdef XBOX
    /*
     * Populate successor CC defines for cross-TB dead flag elimination.
     * jmp_target_addr is now set (translate_code just completed).
     * Best-effort lookup of each successor in the TB hash table.
     */
    tcg_ctx->succ_cc_defines[0] = 0;
    tcg_ctx->succ_cc_defines[1] = 0;
    if (tb_cflags(tb) & CF_TIER1) {
        for (int n = 0; n < 2; n++) {
            vaddr target = tb->jmp_target_addr[n];
            if (target == 0 || target == (vaddr)-1) {
                continue;
            }
            uint32_t lc = tb_cflags(tb) & ~(CF_TIER1 | CF_INVALID);
            uint32_t h = tb_hash_func(target, target,
                                       tb->flags, tb->cs_base, lc);
            TranslationBlock *succ = qht_lookup_custom(
                &tb_ctx.htable, &target, h, tb_pc_cmp);
            if (succ) {
                tcg_ctx->succ_cc_defines[n] = succ->cc_defines_first;
            }
        }
    }
#endif

    return tcg_gen_code(tcg_ctx, tb, pc);
}

/* Called with mmap_lock held for user mode emulation.  */
TranslationBlock *tb_gen_code(CPUState *cpu, TCGTBCPUState s)
{
    CPUArchState *env = cpu_env(cpu);
    TranslationBlock *tb, *existing_tb;
    tb_page_addr_t phys_pc, phys_p2;
    tcg_insn_unit *gen_code_buf;
    int gen_code_size, search_size, max_insns;
    int64_t ti;
    void *host_pc;
    bool recycled = false;

    assert_memory_lock();
    qemu_thread_jit_write();

    phys_pc = get_page_addr_code_hostp(env, s.pc, &host_pc);

    if (phys_pc == -1) {
        /* Generate a one-shot TB with 1 insn in it */
        s.cflags = (s.cflags & ~CF_COUNT_MASK) | 1;
    }

    max_insns = s.cflags & CF_COUNT_MASK;
    if (max_insns == 0) {
        max_insns = TCG_MAX_INSNS;
    }
    QEMU_BUILD_BUG_ON(CF_COUNT_MASK + 1 != TCG_MAX_INSNS);

    tb = inv_tb_htable_lookup(cpu, s);
    if (tb) {
#ifdef XBOX
        /*
         * If there is a pending tier-1 promotion request for this PC,
         * do NOT recycle the old (tier-0) code.  Remove the stale entry
         * from inv_htable and fall through to fresh generation so the
         * tier1_consume_request check triggers and CF_TIER1 is set.
         */
        if (tier1_has_pending_request(s.pc, s.cs_base, s.flags)) {
            uint32_t orig = tb_cflags(tb);
            uint32_t h = tb_hash_func(phys_pc,
                                      (orig & CF_PCREL ? 0 : tb->pc),
                                      tb->flags, tb->cs_base,
                                      orig & ~CF_INVALID);
            qht_remove(&tb_ctx.inv_htable, tb, h);
            tb = NULL;
            goto skip_recycle;
        }
#endif
        qemu_spin_lock(&tb->jmp_lock);
        qatomic_set(&tb->cflags, tb->cflags & ~CF_INVALID);
        qemu_spin_unlock(&tb->jmp_lock);
        uint32_t h = tb_hash_func(phys_pc, (tb->cflags & CF_PCREL ? 0 : tb->pc),
                                  tb->flags, tb->cs_base, tb->cflags);
        bool removed = qht_remove(&tb_ctx.inv_htable, tb, h);
        g_assert(removed);
        if (phys_pc != -1) {
            tb_lock_page0(phys_pc);
            if (tb->page_addr[1] != -1) {
                tb_lock_page1(phys_pc, tb->page_addr[1]);
            }
        }
        recycled = true;
        goto recycle_tb;
    }
#ifdef XBOX
 skip_recycle:
#endif

#ifdef XBOX
    bool hot_alloc = false;
    void *saved_cgp = NULL, *saved_cgb = NULL, *saved_cghw = NULL;
    size_t saved_cgbs = 0;
#endif

 buffer_overflow:
    assert_no_pages_locked();

#ifdef XBOX
    if (!hot_alloc && (s.cflags & CF_TIER1) && tcg_ctx->hot_arena_start) {
        tb = tcg_tb_alloc_hot(tcg_ctx);
        if (tb) {
            hot_alloc = true;
            saved_cgp  = tcg_ctx->code_gen_ptr;
            saved_cgb  = tcg_ctx->code_gen_buffer;
            saved_cgbs = tcg_ctx->code_gen_buffer_size;
            saved_cghw = tcg_ctx->code_gen_highwater;
            tcg_ctx->code_gen_ptr         = tcg_ctx->hot_arena_ptr;
            tcg_ctx->code_gen_buffer      = tcg_ctx->hot_arena_start;
            tcg_ctx->code_gen_buffer_size = tcg_ctx->hot_arena_end
                                          - tcg_ctx->hot_arena_start;
            tcg_ctx->code_gen_highwater   = tcg_ctx->hot_arena_end
                                          - TCG_HIGHWATER;
            goto got_tb;
        }
    }
#endif

    tb = tcg_tb_alloc(tcg_ctx);
    if (unlikely(!tb)) {
        /* flush must be done */
        if (cpu_in_serial_context(cpu)) {
            trace_tb_gen_code_buffer_overflow("tcg_tb_alloc");
            tb_flush__exclusive_or_serial();
            goto buffer_overflow;
        }
        queue_tb_flush(cpu);
        mmap_unlock();
        /* Make the execution loop process the flush as soon as possible.  */
        cpu->exception_index = EXCP_INTERRUPT;
        cpu_loop_exit(cpu);
    }

#ifdef XBOX
 got_tb:
#endif
    gen_code_buf = tcg_ctx->code_gen_ptr;
    tb->tc.ptr = tcg_splitwx_to_rx(gen_code_buf);
    if (!(s.cflags & CF_PCREL)) {
        tb->pc = s.pc;
    }
    tb->cs_base = s.cs_base;
    tb->flags = s.flags;
    tb->cflags = s.cflags;
#ifdef XBOX
    tb->exec_count = 0;
    tb->tier = 0;
    tb->cc_defines_first = 0;
    tb->chain_count[0] = 0;
    tb->chain_count[1] = 0;
    tb->superblock = NULL;

    /* Check if this PC has a pending tier-1 promotion request. */
    {
        uint32_t tier1_cflags = tb->cflags;
        int saved_exec = tier1_consume_request(s.pc, s.cs_base, s.flags,
                                               &tier1_cflags);
        if (saved_exec >= 0) {
            tb->cflags = tier1_cflags;
            s.cflags = tier1_cflags;
            tb->tier = 1;
            tb->exec_count = (uint32_t)saved_exec;
        }
    }
#endif
    tb_set_page_addr0(tb, phys_pc);
    tb_set_page_addr1(tb, -1);
    if (phys_pc != -1) {
        tb_lock_page0(phys_pc);
    }

    tcg_ctx->gen_tb = tb;
    tcg_ctx->addr_type = target_long_bits() == 32 ? TCG_TYPE_I32 : TCG_TYPE_I64;
    tcg_ctx->guest_mo = cpu->cc->tcg_ops->guest_default_memory_order;

 restart_translate:
    trace_translate_block(tb, s.pc, tb->tc.ptr);

    gen_code_size = setjmp_gen_code(env, tb, s.pc, host_pc, &max_insns, &ti);
    if (unlikely(gen_code_size < 0)) {
        switch (gen_code_size) {
        case -1:
            trace_tb_gen_code_buffer_overflow("setjmp_gen_code");
            /*
             * Overflow of code_gen_buffer, or the current slice of it.
             *
             * TODO: We don't need to re-do tcg_ops->translate_code, nor
             * should we re-do the tcg optimization currently hidden
             * inside tcg_gen_code.  All that should be required is to
             * flush the TBs, allocate a new TB, re-initialize it per
             * above, and re-do the actual code generation.
             */
            qemu_log_mask(CPU_LOG_TB_OP | CPU_LOG_TB_OP_OPT,
                          "Restarting code generation for "
                          "code_gen_buffer overflow\n");
            tb_unlock_pages(tb);
            tcg_ctx->gen_tb = NULL;
#ifdef XBOX
            if (hot_alloc) {
                tcg_ctx->code_gen_ptr         = saved_cgp;
                tcg_ctx->code_gen_buffer      = saved_cgb;
                tcg_ctx->code_gen_buffer_size = saved_cgbs;
                tcg_ctx->code_gen_highwater   = saved_cghw;
                hot_alloc = false;
            }
#endif
            goto buffer_overflow;

        case -2:
            /*
             * The code generated for the TranslationBlock is too large.
             * The maximum size allowed by the unwind info is 64k.
             * There may be stricter constraints from relocations
             * in the tcg backend.
             *
             * Try again with half as many insns as we attempted this time.
             * If a single insn overflows, there's a bug somewhere...
             */
            assert(max_insns > 1);
            max_insns /= 2;
            qemu_log_mask(CPU_LOG_TB_OP | CPU_LOG_TB_OP_OPT,
                          "Restarting code generation with "
                          "smaller translation block (max %d insns)\n",
                          max_insns);

            /*
             * The half-sized TB may not cross pages.
             * TODO: Fix all targets that cross pages except with
             * the first insn, at which point this can't be reached.
             */
            phys_p2 = tb_page_addr1(tb);
            if (unlikely(phys_p2 != -1)) {
                tb_unlock_page1(phys_pc, phys_p2);
                tb_set_page_addr1(tb, -1);
            }
            goto restart_translate;

        case -3:
            /*
             * We had a page lock ordering problem.  In order to avoid
             * deadlock we had to drop the lock on page0, which means
             * that everything we translated so far is compromised.
             * Restart with locks held on both pages.
             */
            qemu_log_mask(CPU_LOG_TB_OP | CPU_LOG_TB_OP_OPT,
                          "Restarting code generation with re-locked pages");
            goto restart_translate;

        default:
            g_assert_not_reached();
        }
    }
    tcg_ctx->gen_tb = NULL;

    search_size = encode_search(tb, (void *)gen_code_buf + gen_code_size);
    if (unlikely(search_size < 0)) {
        trace_tb_gen_code_buffer_overflow("encode_search");
        tb_unlock_pages(tb);
        goto buffer_overflow;
    }
    tb->tc.size = gen_code_size;

    /*
     * For CF_PCREL, attribute all executions of the generated code
     * to its first mapping.
     */
    perf_report_code(s.pc, tb, tcg_splitwx_to_rx(gen_code_buf));

    if (qemu_loglevel_mask(CPU_LOG_TB_OUT_ASM) &&
        qemu_log_in_addr_range(s.pc)) {
        FILE *logfile = qemu_log_trylock();
        if (logfile) {
            int code_size, data_size;
            const tcg_target_ulong *rx_data_gen_ptr;
            size_t chunk_start;
            int insn = 0;

            if (tcg_ctx->data_gen_ptr) {
                rx_data_gen_ptr = tcg_splitwx_to_rx(tcg_ctx->data_gen_ptr);
                code_size = (const void *)rx_data_gen_ptr - tb->tc.ptr;
                data_size = gen_code_size - code_size;
            } else {
                rx_data_gen_ptr = 0;
                code_size = gen_code_size;
                data_size = 0;
            }

            /* Dump header and the first instruction */
            fprintf(logfile, "OUT: [size=%d]\n", gen_code_size);
            fprintf(logfile,
                    "  -- guest addr 0x%016" PRIx64 " + tb prologue\n",
                    tcg_ctx->gen_insn_data[insn * INSN_START_WORDS]);
            chunk_start = tcg_ctx->gen_insn_end_off[insn];
            disas(logfile, tb->tc.ptr, chunk_start);

            /*
             * Dump each instruction chunk, wrapping up empty chunks into
             * the next instruction. The whole array is offset so the
             * first entry is the beginning of the 2nd instruction.
             */
            while (insn < tb->icount) {
                size_t chunk_end = tcg_ctx->gen_insn_end_off[insn];
                if (chunk_end > chunk_start) {
                    fprintf(logfile, "  -- guest addr 0x%016" PRIx64 "\n",
                            tcg_ctx->gen_insn_data[insn * INSN_START_WORDS]);
                    disas(logfile, tb->tc.ptr + chunk_start,
                          chunk_end - chunk_start);
                    chunk_start = chunk_end;
                }
                insn++;
            }

            if (chunk_start < code_size) {
                fprintf(logfile, "  -- tb slow paths + alignment\n");
                disas(logfile, tb->tc.ptr + chunk_start,
                      code_size - chunk_start);
            }

            /* Finally dump any data we may have after the block */
            if (data_size) {
                int i;
                fprintf(logfile, "  data: [size=%d]\n", data_size);
                for (i = 0; i < data_size / sizeof(tcg_target_ulong); i++) {
                    if (sizeof(tcg_target_ulong) == 8) {
                        fprintf(logfile,
                                "0x%08" PRIxPTR ":  .quad  0x%016" TCG_PRIlx "\n",
                                (uintptr_t)&rx_data_gen_ptr[i], rx_data_gen_ptr[i]);
                    } else if (sizeof(tcg_target_ulong) == 4) {
                        fprintf(logfile,
                                "0x%08" PRIxPTR ":  .long  0x%08" TCG_PRIlx "\n",
                                (uintptr_t)&rx_data_gen_ptr[i], rx_data_gen_ptr[i]);
                    } else {
                        qemu_build_not_reached();
                    }
                }
            }
            fprintf(logfile, "\n");
            qemu_log_unlock(logfile);
        }
    }

#ifdef XBOX
    if (hot_alloc) {
        tcg_ctx->hot_arena_ptr = (void *)
            ROUND_UP((uintptr_t)gen_code_buf + gen_code_size + search_size,
                     CODE_GEN_ALIGN);
        tcg_ctx->code_gen_ptr         = saved_cgp;
        tcg_ctx->code_gen_buffer      = saved_cgb;
        tcg_ctx->code_gen_buffer_size = saved_cgbs;
        tcg_ctx->code_gen_highwater   = saved_cghw;
    } else
#endif
    {
        qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
            ROUND_UP((uintptr_t)gen_code_buf + gen_code_size + search_size,
                     CODE_GEN_ALIGN));
    }

    /* init jump list */
    qemu_spin_init(&tb->jmp_lock);

recycle_tb:
    tb->jmp_list_head = (uintptr_t)NULL;
    tb->jmp_list_next[0] = (uintptr_t)NULL;
    tb->jmp_list_next[1] = (uintptr_t)NULL;
    tb->jmp_dest[0] = (uintptr_t)NULL;
    tb->jmp_dest[1] = (uintptr_t)NULL;

    /* init original jump addresses which have been set during tcg_gen_code() */
    if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
        tb_reset_jump(tb, 0);
    }
    if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
        tb_reset_jump(tb, 1);
    }

    /*
     * Insert TB into the corresponding region tree before publishing it
     * through QHT. Otherwise rewinding happened in the TB might fail to
     * lookup itself using host PC.
     */
    tcg_tb_insert(tb);

    /*
     * If the TB is not associated with a physical RAM page then it must be
     * a temporary one-insn TB.
     *
     * Such TBs must be added to region trees in order to make sure that
     * restore_state_to_opc() - which on some architectures is not limited to
     * rewinding, but also affects exception handling! - is called when such a
     * TB causes an exception.
     *
     * At the same time, temporary one-insn TBs must be executed at most once,
     * because subsequent reads from, e.g., I/O memory may return different
     * values. So return early before attempting to link to other TBs or add
     * to the QHT.
     */
    if (tb_page_addr0(tb) == -1) {
        assert_no_pages_locked();
        return tb;
    }

#ifdef XBOX
    /*
     * Strip CF_TIER1 BEFORE tb_link_page hashes the TB.  CF_TIER1 is
     * only used during code generation to trigger tier-1 optimisation
     * passes; keeping it in cflags would place the TB in the wrong
     * hash bucket, making it unfindable by normal lookups.
     */
    if (tb->cflags & CF_TIER1) {
        tb->cflags &= ~CF_TIER1;
    }
#endif

    /*
     * No explicit memory barrier is required -- tb_link_page() makes the
     * TB visible in a consistent state.
     */
    existing_tb = tb_link_page(tb);
    assert_no_pages_locked();

    /* if the TB already exists, discard what we just translated */
    if (unlikely(existing_tb != tb)) {
        if (!recycled) {
            uintptr_t orig_aligned = (uintptr_t)gen_code_buf;

            orig_aligned -= ROUND_UP(sizeof(*tb), qemu_icache_linesize);
#ifdef XBOX
            if (hot_alloc) {
                tcg_ctx->hot_arena_ptr = (void *)orig_aligned;
            } else
#endif
            {
                qatomic_set(&tcg_ctx->code_gen_ptr, (void *)orig_aligned);
            }
        }
        tcg_tb_remove(tb);
        return existing_tb;
    }

#ifdef XBOX
    tb_cache_record_hint(tb);
#endif

#if defined(CONFIG_VTUNE_JITPROFILING)
    if (iJIT_IsProfilingActive() == iJIT_SAMPLING_ON && !recycled) {
        iJIT_Method_Load *jmethod = g_malloc0(sizeof(iJIT_Method_Load));
        jmethod->method_id = iJIT_GetNewMethodID();
        jmethod->method_name = g_strdup_printf("G@0x%x", pc);
        jmethod->class_file_name = NULL;
        jmethod->source_file_name = NULL;
        jmethod->method_load_address = (void*)tb->tc.ptr;
        jmethod->method_size = tb->tc.size;

        iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void*)jmethod);
    }
#endif

    return tb;
}

/* user-mode: call with mmap_lock held */
void tb_check_watchpoint(CPUState *cpu, uintptr_t retaddr)
{
    TranslationBlock *tb;

    assert_memory_lock();

    tb = tcg_tb_lookup(retaddr);
    if (tb) {
        /* We can use retranslation to find the PC.  */
        cpu_restore_state_from_tb(cpu, tb, retaddr);
        tb_phys_invalidate(tb, -1);
    } else {
        /* The exception probably happened in a helper.  The CPU state should
           have been saved before calling it. Fetch the PC from there.  */
        CPUArchState *env = cpu_env(cpu);
        TCGTBCPUState s = cpu->cc->tcg_ops->get_tb_cpu_state(cpu);
        tb_page_addr_t addr = get_page_addr_code(env, s.pc);

        if (addr != -1) {
            tb_invalidate_phys_range(cpu, addr, addr);
        }
    }
}

#ifndef CONFIG_USER_ONLY
/*
 * In deterministic execution mode, instructions doing device I/Os
 * must be at the end of the TB.
 *
 * Called by softmmu_template.h, with iothread mutex not held.
 */
void cpu_io_recompile(CPUState *cpu, uintptr_t retaddr)
{
    TranslationBlock *tb;
    CPUClass *cc;
    uint32_t n;

    tb = tcg_tb_lookup(retaddr);
    if (!tb) {
        cpu_abort(cpu, "cpu_io_recompile: could not find TB for pc=%p",
                  (void *)retaddr);
    }
    cpu_restore_state_from_tb(cpu, tb, retaddr);

    /*
     * Some guests must re-execute the branch when re-executing a delay
     * slot instruction.  When this is the case, adjust icount and N
     * to account for the re-execution of the branch.
     */
    n = 1;
    cc = cpu->cc;
    if (cc->tcg_ops->io_recompile_replay_branch &&
        cc->tcg_ops->io_recompile_replay_branch(cpu, tb)) {
        cpu->neg.icount_decr.u16.low++;
        n = 2;
    }

    /*
     * Exit the loop and potentially generate a new TB executing the
     * just the I/O insns. We also limit instrumentation to memory
     * operations only (which execute after completion) so we don't
     * double instrument the instruction. Also don't let an IRQ sneak
     * in before we execute it.
     */
    cpu->cflags_next_tb = curr_cflags(cpu) | CF_MEMI_ONLY | CF_NOIRQ | n;

    if (qemu_loglevel_mask(CPU_LOG_EXEC)) {
        vaddr pc = cpu->cc->get_pc(cpu);
        if (qemu_log_in_addr_range(pc)) {
            qemu_log("cpu_io_recompile: rewound execution of TB to %016"
                     VADDR_PRIx "\n", pc);
        }
    }

    cpu_loop_exit_noexc(cpu);
}

#endif /* CONFIG_USER_ONLY */

/*
 * Called by generic code at e.g. cpu reset after cpu creation,
 * therefore we must be prepared to allocate the jump cache.
 */
void tcg_flush_jmp_cache(CPUState *cpu)
{
    CPUJumpCache *jc = cpu->tb_jmp_cache;

    /* During early initialization, the cache may not yet be allocated. */
    if (unlikely(jc == NULL)) {
        return;
    }

    for (int i = 0; i < TB_JMP_CACHE_SIZE; i++) {
        qatomic_set(&jc->array[i].tb, NULL);
    }
}

/* ================================================================== */
/*  Superblock Formation: merge TB A and TB B into a single TB        */
/* ================================================================== */

#ifdef XBOX

#include "tcg/tcg-op-common.h"
#include "exec/translator.h"
#ifdef __ANDROID__
#include <android/log.h>
#define SB_LOG(...) __android_log_print(ANDROID_LOG_INFO, "superblock", __VA_ARGS__)
#else
#define SB_LOG(...) do {} while (0)
#endif

/*
 * IR surgery: find the goto_tb + exit_tb pair for a given exit slot.
 * Walks backward from the end of the ops list.
 * Returns the goto_tb op, or NULL if not found.
 * Also sets *out_exit_tb to the corresponding exit_tb op.
 */
static TCGOp *sb_find_exit_ops(TCGContext *s, int slot, TCGOp **out_exit_tb)
{
    TCGOp *found_exit = NULL;
    TCGOp *found_goto = NULL;
    TCGOp *op;

    *out_exit_tb = NULL;

    QTAILQ_FOREACH_REVERSE(op, &s->ops, link) {
        if (op->opc == INDEX_op_exit_tb) {
            uintptr_t val = op->args[0];
            /*
             * exit_tb encodes (tb_ptr | exit_idx).  The bottom bits
             * give the slot index.  TB_EXIT_REQUESTED is a special
             * value; skip it.
             */
            int idx = val & 3;
            if (val != 0 && idx == slot) {
                found_exit = op;
                /* The goto_tb for this slot should be shortly before. */
                TCGOp *prev = QTAILQ_PREV(op, link);
                while (prev) {
                    if (prev->opc == INDEX_op_goto_tb &&
                        (int)prev->args[0] == slot) {
                        found_goto = prev;
                        *out_exit_tb = found_exit;
                        return found_goto;
                    }
                    /* Don't search too far back. */
                    if (prev->opc == INDEX_op_set_label ||
                        prev->opc == INDEX_op_call) {
                        break;
                    }
                    prev = QTAILQ_PREV(prev, link);
                }
            }
        }
    }
    return NULL;
}

/*
 * Remap an exit_tb op to use a new slot index and TB pointer.
 * Also remap the corresponding goto_tb slot number.
 */
static void sb_remap_exit(TCGOp *goto_op, TCGOp *exit_op,
                          int new_slot, TranslationBlock *new_tb)
{
    goto_op->args[0] = new_slot;
    exit_op->args[0] = (uintptr_t)new_tb | new_slot;
}

/*
 * Detach the trailing exitreq epilogue (set_label + exit_tb EXIT_REQUESTED)
 * from the ops list.  Save the ops for later re-attachment.
 */
static bool sb_detach_exitreq(TCGContext *s,
                              TCGOp **out_label, TCGOp **out_exit)
{
    *out_label = NULL;
    *out_exit = NULL;

    /* The exitreq exit_tb should be the very last op. */
    TCGOp *last = QTAILQ_LAST(&s->ops);
    if (!last || last->opc != INDEX_op_exit_tb) {
        return false;
    }

    /* The set_label should be just before it. */
    TCGOp *prev = QTAILQ_PREV(last, link);
    if (!prev || prev->opc != INDEX_op_set_label) {
        return false;
    }

    *out_exit = last;
    *out_label = prev;

    QTAILQ_REMOVE(&s->ops, last, link);
    s->nb_ops--;
    QTAILQ_REMOVE(&s->ops, prev, link);
    s->nb_ops--;

    return true;
}

/*
 * Re-attach the exitreq epilogue at the tail, updating the TB pointer.
 */
static void sb_reattach_exitreq(TCGContext *s,
                                TCGOp *label_op, TCGOp *exit_op,
                                TranslationBlock *sb)
{
    /* Update the exit_tb arg to point to the superblock. */
    exit_op->args[0] = (uintptr_t)sb | TB_EXIT_REQUESTED;

    QTAILQ_INSERT_TAIL(&s->ops, label_op, link);
    s->nb_ops++;
    QTAILQ_INSERT_TAIL(&s->ops, exit_op, link);
    s->nb_ops++;
}

/*
 * tb_gen_superblock -- merge TB A and TB B into a single superblock.
 *
 * This is the main formation function.  It:
 *   1. Translates A's guest code into TCG IR
 *   2. Performs IR surgery to remove A's dominant exit
 *   3. Translates B's guest code, appending to the same IR
 *   4. Remaps exit slots and reattaches the exitreq epilogue
 *   5. Generates native code via tcg_gen_code()
 *
 * Returns the new superblock TB, or NULL on failure.
 */
TranslationBlock *tb_gen_superblock(CPUState *cpu,
                                     TranslationBlock *tb_a,
                                     int dominant_exit)
{
    CPUArchState *env = cpu_env(cpu);
    TranslationBlock *tb, *existing_tb;
    tb_page_addr_t phys_pc_a, phys_pc_b;
    void *host_pc_a, *host_pc_b;
    tcg_insn_unit *gen_code_buf;
    int gen_code_size, search_size, max_insns;
    int64_t ti;
    int non_dominant = 1 - dominant_exit;

    /* Look up TB B from A's jump destination. */
    uintptr_t dest = qatomic_read(&tb_a->jmp_dest[dominant_exit]);
    if (dest == (uintptr_t)NULL || (dest & 1)) {
        return NULL;
    }
    TranslationBlock *tb_b = (TranslationBlock *)dest;

    /* Resolve physical addresses and host pointers. */
    phys_pc_a = get_page_addr_code_hostp(env, tb_a->pc, &host_pc_a);
    phys_pc_b = get_page_addr_code_hostp(env, tb_b->pc, &host_pc_b);
    if (phys_pc_a == -1 || phys_pc_b == -1) {
        return NULL;
    }

    /*
     * Invalidate the original TB A before creating the superblock.
     * Pass -1 so the TB is properly removed from the page list.
     */
    tb_phys_invalidate(tb_a, -1);

    assert_memory_lock();
    qemu_thread_jit_write();

    /* Allocate a new TB for the superblock. */
    tb = tcg_tb_alloc(tcg_ctx);
    if (!tb) {
        return NULL;
    }

    gen_code_buf = tcg_ctx->code_gen_ptr;
    tb->tc.ptr = tcg_splitwx_to_rx(gen_code_buf);
    tb->pc = tb_a->pc;
    tb->cs_base = tb_a->cs_base;
    tb->flags = tb_a->flags;
    tb->cflags = (tb_a->cflags & ~(CF_COUNT_MASK | CF_INVALID | CF_TIER1))
                 | CF_TIER1 | CF_SUPERBLOCK;
    tb->exec_count = 0;
    tb->tier = 2;
    tb->chain_count[0] = 0;
    tb->chain_count[1] = 0;
    tb->superblock = NULL;
    tb_set_page_addr0(tb, phys_pc_a);
    tb_set_page_addr1(tb, (phys_pc_a != phys_pc_b) ? phys_pc_b : -1);
    tb_lock_page0(phys_pc_a);
    if (phys_pc_a != phys_pc_b) {
        tb_lock_page1(phys_pc_a, phys_pc_b);
    }

    tcg_ctx->gen_tb = tb;
    tcg_ctx->addr_type = target_long_bits() == 32 ? TCG_TYPE_I32 : TCG_TYPE_I64;
    tcg_ctx->guest_mo = cpu->cc->tcg_ops->guest_default_memory_order;

    /* Step 1: Translate A's instruction range (standard path). */
    int ret = sigsetjmp(tcg_ctx->jmp_trans, 0);
    if (ret != 0) {
        /* Translation error -- bail out. */
        tb_unlock_pages(tb);
        tcg_ctx->gen_tb = NULL;
        return NULL;
    }

    tcg_func_start(tcg_ctx);
    tcg_ctx->cpu = cpu;

    max_insns = tb_a->icount;
    if (max_insns == 0) {
        max_insns = TCG_MAX_INSNS;
    }

    cpu->cc->tcg_ops->translate_code(cpu, tb, &max_insns, tb_a->pc, host_pc_a);

    int a_insns = tb->icount;
    int a_size = tb->size;

    /* Step 2: Detach the exitreq epilogue. */
    TCGOp *exitreq_label, *exitreq_exit;
    if (!sb_detach_exitreq(tcg_ctx, &exitreq_label, &exitreq_exit)) {
        tb_unlock_pages(tb);
        tcg_ctx->gen_tb = NULL;
        tcg_ctx->cpu = NULL;
        return NULL;
    }

    /* Step 3: Find and remove the dominant exit (goto_tb + exit_tb). */
    TCGOp *dom_goto, *dom_exit;
    dom_goto = sb_find_exit_ops(tcg_ctx, dominant_exit, &dom_exit);
    if (!dom_goto || !dom_exit) {
        /* Can't find the exit -- reattach exitreq and bail. */
        sb_reattach_exitreq(tcg_ctx, exitreq_label, exitreq_exit, tb);
        tb_unlock_pages(tb);
        tcg_ctx->gen_tb = NULL;
        tcg_ctx->cpu = NULL;
        return NULL;
    }

    /* Remove the dominant exit ops. */
    tcg_op_remove(tcg_ctx, dom_goto);
    tcg_op_remove(tcg_ctx, dom_exit);

    /* Step 4: Remap non-dominant exit to slot 0. */
    TCGOp *nd_goto, *nd_exit;
    nd_goto = sb_find_exit_ops(tcg_ctx, non_dominant, &nd_exit);
    if (nd_goto && nd_exit) {
        sb_remap_exit(nd_goto, nd_exit, 0, tb);
    }

    /* Step 5: Translate B's instructions (appended to existing IR).
     * Set superblock_append so translator_loop skips gen_tb_start/gen_tb_end. */
    int b_max = tb_b->icount;
    if (b_max == 0) {
        b_max = TCG_MAX_INSNS;
    }

    tcg_ctx->superblock_append = true;
#ifdef CONFIG_DEBUG_TCG
    tcg_ctx->goto_tb_issue_mask = 0;
#endif
    cpu->cc->tcg_ops->translate_code(cpu, tb, &b_max, tb_b->pc, host_pc_b);
    tcg_ctx->superblock_append = false;

    int b_insns = tb->icount;  /* translate_code updates tb->icount */
    /* tb->size was updated by translate_code to B's size; save it. */
    int b_size = tb->size;

    /* Step 6: Remap B's exits.
     * IMPORTANT: Remap slot 1 first, then slot 0, to avoid finding
     * a just-remapped op when searching.
     *
     * B's exit slot 1 -> remove goto_tb, convert to indirect lookup
     * B's exit slot 0 -> superblock slot 1
     */
    TCGOp *b_goto1, *b_exit1;
    b_goto1 = sb_find_exit_ops(tcg_ctx, 1, &b_exit1);
    if (b_goto1 && b_exit1) {
        /*
         * Convert B's second exit to an indirect lookup.
         * Remove goto_tb, keep exit_tb with val=0 (triggers epilogue
         * return with NULL, which the main loop handles as a full lookup).
         */
        tcg_op_remove(tcg_ctx, b_goto1);
        b_exit1->args[0] = 0;  /* exit_tb(NULL, 0) -> full lookup */
    }

    TCGOp *b_goto0, *b_exit0;
    b_goto0 = sb_find_exit_ops(tcg_ctx, 0, &b_exit0);
    if (b_goto0 && b_exit0) {
        sb_remap_exit(b_goto0, b_exit0, 1, tb);
    }

    /* Step 7: Reattach exitreq epilogue. */
    sb_reattach_exitreq(tcg_ctx, exitreq_label, exitreq_exit, tb);

    /* Step 8: Generate native code (Tier 1 passes apply automatically). */
    tb->size = a_size;  /* Restore A's size for the TB entry point range */
    tb->icount = a_insns + b_insns;
    (void)b_size;  /* b_size tracked in SuperblockInfo */

    gen_code_size = tcg_gen_code(tcg_ctx, tb, tb_a->pc);
    tcg_ctx->cpu = NULL;
    tcg_ctx->gen_tb = NULL;

    if (gen_code_size < 0) {
        /* Code generation failed -- clean up. */
        tb_unlock_pages(tb);
        return NULL;
    }

    search_size = encode_search(tb, (void *)gen_code_buf + gen_code_size);
    if (search_size < 0) {
        tb_unlock_pages(tb);
        tcg_ctx->gen_tb = NULL;
        return NULL;
    }
    tb->tc.size = gen_code_size;

    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
        ROUND_UP((uintptr_t)gen_code_buf + gen_code_size + search_size,
                 CODE_GEN_ALIGN));

    /* Init jump list. */
    qemu_spin_init(&tb->jmp_lock);
    tb->jmp_list_head = (uintptr_t)NULL;
    tb->jmp_list_next[0] = (uintptr_t)NULL;
    tb->jmp_list_next[1] = (uintptr_t)NULL;
    tb->jmp_dest[0] = (uintptr_t)NULL;
    tb->jmp_dest[1] = (uintptr_t)NULL;

    if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
        tb_reset_jump(tb, 0);
    }
    if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
        tb_reset_jump(tb, 1);
    }

    /*
     * Compute ihash over the superblock's guest code range (A's code).
     * This allows proper TB recycling if the superblock is later
     * invalidated and re-requested.
     */
    tb->ihash = tb_code_hash_func(env, tb->pc, tb->size);

    /*
     * Strip CF_TIER1 | CF_SUPERBLOCK BEFORE insertion so the QHT hash
     * matches what normal lookups compute (they never include these flags).
     * tcg_gen_code already used CF_TIER1 for Tier 1 optimization passes.
     */
    tb->cflags &= ~(CF_TIER1 | CF_SUPERBLOCK);
    tb->tier = 2;

    tcg_tb_insert(tb);

    if (tb_page_addr0(tb) == -1) {
        assert_no_pages_locked();
        goto fill_superblock_info;
    }

    existing_tb = tb_link_page(tb);
    assert_no_pages_locked();

    if (existing_tb != tb) {
        tcg_tb_remove(tb);
        return NULL;
    }

fill_superblock_info:
    ; /* empty statement to satisfy C99 label-before-declaration rule */
    /* Step 9: Fill in SuperblockInfo. */
    SuperblockInfo *sbi = g_malloc0(sizeof(SuperblockInfo));
    sbi->pc_b = tb_b->pc;
    sbi->size_b = tb_b->size;
    sbi->icount_b = b_insns;
    sbi->phys_pc_b = phys_pc_b;
    tb->superblock = sbi;

    SB_LOG("formed A=0x%" PRIx64 " + B=0x%" PRIx64
           ", combined %d insns, pages=%s",
           (uint64_t)tb_a->pc, (uint64_t)tb_b->pc,
           a_insns + b_insns,
           (phys_pc_a != phys_pc_b) ? "2" : "1");

    return tb;
}

#endif /* XBOX */
