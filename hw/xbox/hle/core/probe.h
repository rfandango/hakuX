/*
 * hakuX HLE — debug probes on title code.
 *
 * HAKUX_HLE_PROBE="va[:n],va[:n]..." (hex VAs, at most 8; n default 16):
 * each time execution reaches va, the registers and the top eight stack
 * words are logged (hakuX-hle-probe) for the first n times and every
 * 1000th after, and the title's code runs on (set it through
 * debug.hakux.env).
 */
#ifndef HW_XBOX_HLE_CORE_PROBE_H
#define HW_XBOX_HLE_CORE_PROBE_H

#include <stdbool.h>
#include <stdint.h>

struct HleCpu;

/* The probes the environment asks for (title start): how many. */
unsigned hle_probe_register(void);
/* The core stops (M2): no probe until the next title's hle_probe_register. */
void hle_probe_reset(void);
/* Whether a probe is at va (the host hooks it). */
bool hle_probe_at(uint32_t va);
/* Execution reached a probe: the registers at it (eip the probe's VA). */
void hle_probe_hit(const struct HleCpu *c);

#endif
