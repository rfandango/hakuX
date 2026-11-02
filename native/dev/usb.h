/*
 * hakuX native core — USB devices behind the OHCI controller (ohci.c).
 *
 * A device answers its standard requests from descriptor tables (usb.c);
 * class and vendor requests go to its control callback, other endpoints to
 * its data callback. A hub also finds the devices downstream of it.
 */
#ifndef NATIVE_DEV_USB_H
#define NATIVE_DEV_USB_H

#include "native/core/nc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_USB_STALL (-1)
#define NC_USB_NAK   (-2)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef struct NcUsbDev NcUsbDev;
struct NcUsbDev {
    const char *name;
    uint8_t addr;           /* 0 until SET_ADDRESS */
    uint8_t config;         /* SET_CONFIGURATION value */
    bool low_speed;

    const uint8_t *dev_desc;
    unsigned dev_desc_len;
    const uint8_t *cfg_desc;
    unsigned cfg_desc_len;
    const char *const *strings; /* index 1.. (0 is the language table) */
    unsigned nstrings;

    /* Class and vendor requests. IN: fill data (len = wLength), return the
     * length; OUT: data holds wLength bytes, return 0. NC_USB_STALL refuses. */
    int (*control)(NcUsbDev *d, const uint8_t *setup, uint8_t *data, int len);
    /* Endpoints 1-15: IN returns the bytes written to buf or NC_USB_NAK;
     * OUT returns the bytes taken. */
    int (*data)(NcUsbDev *d, bool in, int ep, uint8_t *buf, int len);
    /* Hubs: the downstream device with this address (after a port reset, the
     * one at address 0), or NULL. */
    NcUsbDev *(*find)(NcUsbDev *d, uint8_t addr);
    /* A bus reset reached the device. */
    void (*reset)(NcUsbDev *d);
    void *opaque;

    /* Endpoint 0's transfer in progress. */
    uint8_t setup[8];
    uint8_t buf[512];
    int len, pos;
    bool in, data_stage, stalled;
    int pending_addr;
};

/* Endpoint 0 stages. Each returns bytes moved, 0 for an empty packet, or
 * NC_USB_STALL. */
int nc_usb_setup(NcUsbDev *d, const uint8_t setup[8]);
int nc_usb_ep0_in(NcUsbDev *d, uint8_t *buf, int len);
int nc_usb_ep0_out(NcUsbDev *d, const uint8_t *buf, int len);
/* A port reset: address 0, unconfigured. */
void nc_usb_reset(NcUsbDev *d);
/* d or a device below it with this address. */
NcUsbDev *nc_usb_find(NcUsbDev *d, uint8_t addr);

/* xblc.c: the Xbox Live Communicator (voice chat) for player 0-3. */
NcUsbDev *nc_xblc_new(unsigned player);

/* usb_hub.c: a hub with nports downstream ports (1-based). */
NcUsbDev *nc_usb_hub_new(unsigned nports);
void nc_usb_hub_attach(NcUsbDev *hub, unsigned port, NcUsbDev *child);

/* Savestates: a hub's ports (status and change, 1-based, up to 4) and the
 * device on each; a port whose device came or went since (the controllers
 * plugged in now aren't the state's) reports the change. */
typedef struct NcUsbHubState {
    uint32_t nports;
    uint16_t status[5], change[5];
} NcUsbHubState;
void nc_usb_hub_state(NcUsbDev *hub, NcUsbHubState *s);
void nc_usb_hub_state_set(NcUsbDev *hub, const NcUsbHubState *s);
NcUsbDev *nc_usb_hub_child(NcUsbDev *hub, unsigned port);
void nc_usb_hub_port_changed(NcUsbDev *hub, unsigned port);

/* xid.c: an Xbox gamepad (Duke) for player 0-3, and its input. */
NcUsbDev *nc_xid_gamepad_new(unsigned player);
typedef struct NcPadState {
    uint16_t buttons;      /* NC_PAD_* digital buttons */
    uint8_t analog[8];     /* A, B, X, Y, black, white, left trigger, right trigger */
    int16_t lx, ly, rx, ry;
} NcPadState;
#define NC_PAD_DPAD_UP    0x0001
#define NC_PAD_DPAD_DOWN  0x0002
#define NC_PAD_DPAD_LEFT  0x0004
#define NC_PAD_DPAD_RIGHT 0x0008
#define NC_PAD_START      0x0010
#define NC_PAD_BACK       0x0020
#define NC_PAD_LTHUMB     0x0040
#define NC_PAD_RTHUMB     0x0080
void nc_pad_set(unsigned player, const NcPadState *s);
/* The last rumble strengths the title sent. */
void nc_pad_rumble(unsigned player, uint16_t *left, uint16_t *right);

/* ohci.c: the Xbox's first USB controller (0xFED00000, IRQ 1), with gamepads
 * for the players in NC_PAD_MASK (or NC_PADS from player 1; default 1). */
struct NcVm;
void nc_dev_ohci_init(struct NcVm *vm);
/* The players with a controller now (bit n: player n + 1): the others'
 * controllers pulled out, theirs plugged in, as the title would see it. */
void nc_dev_ohci_set_pads(unsigned mask);

#ifdef __cplusplus
}
#endif

#endif
