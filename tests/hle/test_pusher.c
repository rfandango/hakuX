#include "hw/xbox/hle/pb/pusher.h"
#include "hle_test.h"

/* Guest RAM at physical 0 (16 KiB). */
static uint32_t ram[4096];

static const uint32_t *fetch(void *ctx, uint32_t pa, uint32_t n)
{
    return pa % 4 == 0 && pa / 4 + n <= 4096 ? &ram[pa / 4] : NULL;
}

/* What the sink saw: (subchannel, method, value) per method word. */
typedef struct Log {
    unsigned n;
    uint32_t sub[64], m[64], v[64];
    uint32_t stall_at;              /* a method that stalls (0: none) */
    uint32_t hold_at;               /* a method held while hold is set */
    bool hold;
    bool batch;                     /* take data in runs */
    unsigned runs;
} Log;

static int method(void *opaque, unsigned subch, uint32_t m, uint32_t v)
{
    Log *l = opaque;
    if (l->hold && m == l->hold_at) {
        return HLE_PB_HOLD;
    }
    if (l->n < 64) {
        l->sub[l->n] = subch;
        l->m[l->n] = m;
        l->v[l->n] = v;
        l->n++;
    }
    return m && m == l->stall_at ? HLE_PB_STALL : HLE_PB_GO;
}

static uint32_t run(void *opaque, unsigned subch, uint32_t m, bool noninc, const uint32_t *w,
                    uint32_t n, int *stall)
{
    Log *l = opaque;
    if (!l->batch) {
        return 0;
    }
    l->runs++;
    for (uint32_t i = 0; i < n; i++) {
        method(opaque, subch, noninc ? m : m + 4 * i, w[i]);
    }
    return n;
}

static uint32_t hdr(unsigned subch, uint32_t m, unsigned count)
{
    return (count << 18) | (subch << 13) | m;
}

static uint32_t hdr_ni(unsigned subch, uint32_t m, unsigned count)
{
    return 0x40000000u | hdr(subch, m, count);
}

static void pusher(HlePusher *p, uint32_t get)
{
    hle_pb_pusher_init(p, 0, sizeof(ram) - 1);
    p->get = get;
}

static void test_increasing_and_noninc(void)
{
    memset(ram, 0, sizeof(ram));
    uint32_t *w = &ram[0x100 / 4];
    w[0] = hdr(1, 0x0200, 3);                       /* 0x200, 0x204, 0x208 */
    w[1] = 11, w[2] = 12, w[3] = 13;
    w[4] = hdr_ni(2, 0x1818, 2);                    /* 0x1818 twice */
    w[5] = 21, w[6] = 22;
    w[7] = hdr(0, 0x0100, 0);                       /* an empty packet */
    HlePusher p;
    pusher(&p, 0x100);
    Log l = { 0 };
    HlePbSink s = { &l, method, run, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x120, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_U32(p.get, 0x120);
    HT_EQ_INT(l.n, 5);
    HT_EQ_U32(l.m[0], 0x200);
    HT_EQ_U32(l.m[2], 0x208);
    HT_EQ_U32(l.v[2], 13);
    HT_EQ_INT(l.sub[0], 1);
    HT_EQ_U32(l.m[3], 0x1818);
    HT_EQ_U32(l.m[4], 0x1818);
    HT_EQ_U32(l.v[4], 22);
    HT_EQ_INT(l.sub[4], 2);
    HT_EQ_INT(p.headers, 3);
    HT_EQ_INT(p.data, 5);
}

static void test_batched_data(void)
{
    memset(ram, 0, sizeof(ram));
    uint32_t *w = &ram[0];
    w[0] = hdr(0, 0x0B80, 8);
    for (int i = 0; i < 8; i++) {
        w[1 + i] = 100 + i;
    }
    HlePusher p;
    pusher(&p, 0);
    Log l = { .batch = true };
    HlePbSink s = { &l, method, run, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 36, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_INT(l.runs, 1);
    HT_EQ_INT(l.n, 8);
    HT_EQ_U32(l.m[7], 0x0B80 + 28);
    HT_EQ_U32(l.v[7], 107);
}

/* PUT in a packet's data: the rest comes with the next kick. */
static void test_packet_across_kicks(void)
{
    memset(ram, 0, sizeof(ram));
    uint32_t *w = &ram[0x40 / 4];
    w[0] = hdr(0, 0x0300, 4);
    w[1] = 1, w[2] = 2, w[3] = 3, w[4] = 4;
    HlePusher p;
    pusher(&p, 0x40);
    Log l = { 0 };
    HlePbSink s = { &l, method, NULL, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x4C, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_INT(l.n, 2);
    HT_EQ_INT(p.left, 2);
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x54, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_INT(l.n, 4);
    HT_EQ_U32(l.m[3], 0x030C);
    HT_EQ_U32(l.v[3], 4);
}

static void test_jumps(void)
{
    memset(ram, 0, sizeof(ram));
    ram[0x000 / 4] = 0x00000800u | 1;               /* jump to 0x800 */
    ram[0x800 / 4] = hdr(0, 0x0400, 1);
    ram[0x804 / 4] = 7;
    ram[0x808 / 4] = 0x20000000u | 0x0C00;          /* old jump to 0xC00 */
    ram[0xC00 / 4] = hdr(0, 0x0404, 1);
    ram[0xC04 / 4] = 8;
    HlePusher p;
    pusher(&p, 0);
    Log l = { 0 };
    HlePbSink s = { &l, method, NULL, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 0xC08, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_INT(l.n, 2);
    HT_EQ_U32(l.v[0], 7);
    HT_EQ_U32(l.v[1], 8);
    HT_EQ_INT(p.branches, 2);
}

static void test_call_return(void)
{
    memset(ram, 0, sizeof(ram));
    ram[0x000 / 4] = 0x00000400u | 2;               /* call 0x400 */
    ram[0x004 / 4] = hdr(0, 0x0500, 1);
    ram[0x008 / 4] = 2;
    ram[0x400 / 4] = hdr(0, 0x0504, 1);
    ram[0x404 / 4] = 1;
    ram[0x408 / 4] = 0x00020000u;                   /* return */
    HlePusher p;
    pusher(&p, 0);
    Log l = { 0 };
    HlePbSink s = { &l, method, NULL, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x00C, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_INT(l.n, 2);
    HT_EQ_U32(l.m[0], 0x0504);
    HT_EQ_U32(l.m[1], 0x0500);
    HT_CHECK(!p.sub_active);
}

/* The sink's branch hook clears the subroutine state before a CALL, as the
 * guest's DMA_SUBROUTINE write does (4928's runs: CALL in, JUMP back). */
static void clear_sub(void *opaque, struct HlePusher *p)
{
    unsigned *calls = opaque;
    (*calls)++;
    p->sub_active = false;
}

static int nop_method(void *opaque, unsigned subch, uint32_t m, uint32_t v)
{
    return HLE_PB_GO;
}

static void test_call_left_by_jump(void)
{
    memset(ram, 0, sizeof(ram));
    ram[0x000 / 4] = 0x00000400u | 2;               /* call 0x400 */
    ram[0x004 / 4] = 0x00000800u | 2;               /* call 0x800: the first one never returned */
    ram[0x400 / 4] = 0x00000004u | 1;               /* jump back after the call */
    ram[0x800 / 4] = 0x00000008u | 1;               /* jump back after the second */
    HlePusher p;
    pusher(&p, 0);
    HlePbSink plain = { NULL, nop_method, NULL, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x008, fetch, NULL, &plain), HLE_PB_ERR_CALL);
    unsigned calls = 0;
    HlePbSink s = { &calls, nop_method, NULL, clear_sub, NULL };
    pusher(&p, 0);
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x008, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_U32(p.get, 0x008);
    HT_EQ_INT(calls, 2);
}

static void test_branch_errors(void)
{
    memset(ram, 0, sizeof(ram));
    ram[0] = 0x00000100u | 2;                       /* call 0x100 */
    ram[0x100 / 4] = 0x00000200u | 2;               /* call inside a call */
    HlePusher p;
    pusher(&p, 0);
    Log l = { 0 };
    HlePbSink s = { &l, method, NULL, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x40, fetch, NULL, &s), HLE_PB_ERR_CALL);
    HT_EQ_U32(p.error_get, 0x100);
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x40, fetch, NULL, &s), HLE_PB_ERR_CALL);  /* sticky */

    ram[0] = 0x00020000u;                           /* return outside a call */
    pusher(&p, 0);
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x40, fetch, NULL, &s), HLE_PB_ERR_RETURN);

    ram[0] = 0x80000000u;                           /* not a command */
    pusher(&p, 0);
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x40, fetch, NULL, &s), HLE_PB_ERR_INVALID);
    HT_EQ_U32(p.error_word, 0x80000000u);
    HT_EQ_INT(l.n, 0);

    ram[0] = 0x00000000u | 1;                       /* jump to itself, PUT elsewhere */
    pusher(&p, 0);
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x40, fetch, NULL, &s), HLE_PB_ERR_LOOP);
}

/* Methods and a jump back to their start, PUT not among them (a reader left
 * in an old push buffer, Halo 2's boot): only the budget ends the run. */
static void test_budget(void)
{
    memset(ram, 0, sizeof(ram));
    ram[0x100 / 4] = hdr(0, 0x0100, 1);
    ram[0x104 / 4] = 7;
    ram[0x108 / 4] = 0;                             /* an empty header */
    ram[0x10C / 4] = 0x00000100u | 1;               /* jump to 0x100 */
    HlePusher p;
    pusher(&p, 0x100);
    p.budget = 1000;
    Log l = { 0 };
    HlePbSink s = { &l, method, NULL, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x40, fetch, NULL, &s), HLE_PB_ERR_LOOP);
    HT_CHECK(p.words > 1000 && p.words <= 1004);
    HT_EQ_U32(p.error_word, 0);

    ram[0x100 / 4] = 0x00000040u | 1;               /* the same budget, PUT reached */
    ram[0x40 / 4] = hdr(0, 0x0100, 1);
    ram[0x44 / 4] = 5;
    pusher(&p, 0x100);
    p.budget = 1000;
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x48, fetch, NULL, &s), HLE_PB_OK);
}

static void test_limits(void)
{
    memset(ram, 0, sizeof(ram));
    ram[0] = 0x00100000u | 1;                       /* jump past RAM */
    HlePusher p;
    pusher(&p, 0);
    Log l = { 0 };
    HlePbSink s = { &l, method, NULL, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x40, fetch, NULL, &s), HLE_PB_ERR_LIMIT);

    hle_pb_pusher_init(&p, 0, 0x1FFF);              /* an object of 8 KiB */
    ram[0] = 0x00003000u | 1;
    HT_EQ_INT(hle_pb_pusher_run(&p, 0x40, fetch, NULL, &s), HLE_PB_ERR_LIMIT);

    hle_pb_pusher_init(&p, 0x2000, 0x0FFF);         /* based: offsets into it */
    ram[0x2000 / 4] = hdr(0, 0x0600, 1);
    ram[0x2004 / 4] = 9;
    HT_EQ_INT(hle_pb_pusher_run(&p, 8, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_INT(l.n, 1);
    HT_EQ_U32(l.v[0], 9);
}

/* A trap stalls the pusher after the method; the next run goes on. */
static void test_stall(void)
{
    memset(ram, 0, sizeof(ram));
    ram[0] = hdr(0, 0x0100, 3);                     /* 0x100 stalls */
    ram[1] = 1, ram[2] = 2, ram[3] = 3;
    HlePusher p;
    pusher(&p, 0);
    Log l = { .stall_at = 0x0100 };
    HlePbSink s = { &l, method, NULL, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 16, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_U32(p.get, 8);
    HT_EQ_INT(l.n, 1);
    HT_EQ_INT(hle_pb_pusher_run(&p, 16, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_U32(p.get, 16);
    HT_EQ_INT(l.n, 3);
}

/* A held method isn't taken: GET stays on it, and it comes again. */
static void test_hold(void)
{
    memset(ram, 0, sizeof(ram));
    ram[0] = hdr(0, 0x0200, 2);
    ram[1] = 1, ram[2] = 2;
    HlePusher p;
    pusher(&p, 0);
    Log l = { .hold_at = 0x0204, .hold = true };
    HlePbSink s = { &l, method, NULL, NULL, NULL };
    HT_EQ_INT(hle_pb_pusher_run(&p, 12, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_U32(p.get, 8);
    HT_EQ_INT(l.n, 1);
    HT_EQ_INT(p.left, 1);
    l.hold = false;
    HT_EQ_INT(hle_pb_pusher_run(&p, 12, fetch, NULL, &s), HLE_PB_OK);
    HT_EQ_U32(p.get, 12);
    HT_EQ_INT(l.n, 2);
    HT_EQ_U32(l.v[1], 2);
}

int main(void)
{
    HT_RUN(test_increasing_and_noninc);
    HT_RUN(test_batched_data);
    HT_RUN(test_packet_across_kicks);
    HT_RUN(test_jumps);
    HT_RUN(test_call_return);
    HT_RUN(test_call_left_by_jump);
    HT_RUN(test_branch_errors);
    HT_RUN(test_budget);
    HT_RUN(test_limits);
    HT_RUN(test_stall);
    HT_RUN(test_hold);
    HT_DONE();
}
