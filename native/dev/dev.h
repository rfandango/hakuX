/*
 * hakuX native core — devices behind MMIO windows.
 */
#ifndef NATIVE_DEV_DEV_H
#define NATIVE_DEV_DEV_H

#include "native/core/nc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NV2A register space at 0xFD000000 and its vblank clock. */
void nc_dev_nv2a_init(void);
uint64_t nc_dev_nv2a_vblanks(void);
/* The video mode's refresh rate (50 or 60 Hz): the vblank clock's. */
void nc_dev_nv2a_refresh(unsigned hz);
/* The video mode's frame buffer pitch and format (AvSetDisplayMode): the
 * push-buffer decoder's display size; fb, the frame buffer it shows (a title
 * drawing it with the CPU). */
void nc_dev_nv2a_display_mode(uint32_t pitch, uint32_t xfmt, unsigned hz, uint32_t fb);
/* A mod's vblank: n times the video mode's rate (1-4; before the title boots). */
void nc_dev_nv2a_vblank_multiply(unsigned n);
/* A vsync of the panel the frames are shown on (host monotonic ns, the
 * Choreographer's frame time): the vblank locks to the panel's. */
void nc_dev_nv2a_display_vsync(uint64_t ns);
/* The presenter looked for a new front image (host monotonic ns): the vblank
 * keeps half a panel period from its pickups. */
void nc_dev_nv2a_display_pickup(uint64_t ns);
/* The panel's next vsync after `after` (host monotonic ns); 0 if not known. */
uint64_t nc_dev_nv2a_next_vsync(uint64_t after);
/* Register files for the remaining windows (0xFE000000-0xFFFFFFFF). */
void nc_dev_misc_init(void);

/* The MCPX APU (0xFE800000, IRQ 5) over guest RAM (native/dev/apu). */
struct NcVm;
void nc_dev_apu_init(struct NcVm *vm);
/* A CPU thread waits for the GP DSP to take a DirectSound command (the word
 * at host address word to clear): the APU runs its frames to completion
 * meanwhile; returns when the word clears or after about a millisecond. */
void nc_dev_apu_spin_wait(const uint32_t *word);
/* The guest's time left out ns while the guest's CPU was held (host.h
 * hle_host_guest_hold), the APU playing on: its sample counter goes on
 * with it. */
void nc_dev_apu_time_held(uint64_t ns);
/* aci.c: the AC'97 audio codec interface (0xFEC00000, IRQ 6). */
void nc_dev_aci_init(struct NcVm *vm);
/* nvnet.c: the Ethernet controller (0xFEF00000, IRQ 4) and its link state for
 * PhyGetLinkState (XNET_ETHERNET_LINK_*; 0: no link). */
void nc_dev_nvnet_init(struct NcVm *vm);
/* PhyInitialize: the PHY negotiates its link (up a moment later, with its
 * interrupt), as the kernel's has it do. */
void nc_dev_nvnet_phy_init(void);
uint32_t nc_dev_nvnet_link_state(void);

/* x86 I/O ports (IN/OUT); bytes is 1, 2 or 4. */
uint32_t nc_port_in(uint16_t port, unsigned bytes);
void nc_port_out(uint16_t port, unsigned bytes, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif
