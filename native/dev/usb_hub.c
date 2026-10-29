/*
 * hakuX native core — a USB 1.1 hub (USB 1.1 chapter 11): the front panel's,
 * and the one inside each Xbox controller (gamepad on port 1, memory unit
 * slots on 2-3). Its descriptors and port behaviour are those of the hub the
 * Xbox USB stack is known to enumerate (QEMU's usb-hub, as xemu attaches it):
 * no power switching, so ports are powered and report connections from reset.
 */
#include "native/dev/usb.h"

#include <stdlib.h>

#define PORT_CONNECTION 0x0001
#define PORT_ENABLE     0x0002
#define PORT_SUSPEND    0x0004
#define PORT_RESET      0x0010
#define PORT_POWER      0x0100
#define PORT_LOW_SPEED  0x0200

#define C_CONNECTION 0x0001
#define C_ENABLE     0x0002
#define C_SUSPEND    0x0004
#define C_OVERCURRENT 0x0008
#define C_RESET      0x0010

/* Port feature selectors. */
enum {
    F_PORT_CONNECTION = 0, F_PORT_ENABLE = 1, F_PORT_SUSPEND = 2, F_PORT_OVER_CURRENT = 3, F_PORT_RESET = 4,
    F_PORT_POWER = 8, F_PORT_LOW_SPEED = 9, F_C_PORT_CONNECTION = 16, F_C_PORT_ENABLE = 17,
    F_C_PORT_SUSPEND = 18, F_C_PORT_OVER_CURRENT = 19, F_C_PORT_RESET = 20,
};

#define MAX_PORTS 4

typedef struct Hub {
    NcUsbDev dev;
    unsigned nports;
    NcUsbDev *child[MAX_PORTS + 1];
    uint16_t status[MAX_PORTS + 1], change[MAX_PORTS + 1];
} Hub;

static const uint8_t k_dev_desc[18] = {
    18, 1, 0x10, 0x01, /* USB 1.1 */
    9, 0, 0,           /* hub class */
    8,                 /* max packet 0 */
    0x09, 0x04, 0xaa, 0x55, /* 0409:55aa */
    0x01, 0x01, 1, 2, 3, 1,
};

static const uint8_t k_cfg_desc[25] = {
    9, 2, 25, 0, 1, 1, 0, 0xE0, 0,       /* config: self powered, remote wakeup */
    9, 4, 0, 0, 1, 9, 0, 0, 0,           /* interface: hub class, one endpoint */
    7, 5, 0x81, 0x03, 2, 0, 255,         /* status change: interrupt IN */
};

static const char *const k_strings[] = {"hakuX", "USB Hub", "1"};

static int hub_control(NcUsbDev *d, const uint8_t *s, uint8_t *data, int len)
{
    Hub *h = (Hub *)d;
    const uint8_t type = s[0], req = s[1];
    const uint16_t value = nc_rd16(s + 2), index = nc_rd16(s + 4);
    switch ((type << 8) | req) {
    case 0xA006: /* GET_DESCRIPTOR (hub) */
        data[0] = 9;
        data[1] = 0x29;
        data[2] = (uint8_t)h->nports;
        data[3] = 0x0a; /* no power switching, per-port over-current */
        data[4] = 0x00;
        data[5] = 1;    /* power on to power good, 2 ms units */
        data[6] = 0;
        data[7] = 0x00; /* removable devices */
        data[8] = 0xFF;
        return MIN(9, len);
    case 0xA000: /* GET_STATUS (hub) */
        memset(data, 0, 4);
        return MIN(4, len);
    case 0xA300: /* GET_STATUS (port) */
        if (!index || index > h->nports) {
            return NC_USB_STALL;
        }
        nc_wr32(data, h->status[index] | (uint32_t)h->change[index] << 16);
        return MIN(4, len);
    case 0x2301: /* CLEAR_FEATURE (port) */
    case 0x2303: /* SET_FEATURE (port) */
        if (!index || index > h->nports) {
            return NC_USB_STALL;
        }
        {
            const bool set = req == 0x03;
            uint16_t *st = &h->status[index], *ch = &h->change[index];
            switch (value) {
            case F_PORT_ENABLE:
                if (!set) {
                    *st &= ~PORT_ENABLE;
                }
                break;
            case F_PORT_SUSPEND:
                *st = set ? (*st | PORT_SUSPEND) : (*st & ~PORT_SUSPEND);
                break;
            case F_PORT_RESET:
                if (set && h->child[index]) {
                    nc_usb_reset(h->child[index]);
                    *st |= PORT_ENABLE;
                    *ch |= C_RESET;
                }
                break;
            case F_PORT_POWER:
                if (set) {
                    *st |= PORT_POWER;
                    if (h->child[index] && !(*st & PORT_CONNECTION)) {
                        *st |= PORT_CONNECTION | (h->child[index]->low_speed ? PORT_LOW_SPEED : 0);
                        *ch |= C_CONNECTION;
                    }
                } else {
                    *st &= ~(PORT_POWER | PORT_ENABLE);
                }
                break;
            case F_C_PORT_CONNECTION:
                *ch &= ~C_CONNECTION;
                break;
            case F_C_PORT_ENABLE:
                *ch &= ~C_ENABLE;
                break;
            case F_C_PORT_SUSPEND:
                *ch &= ~C_SUSPEND;
                break;
            case F_C_PORT_OVER_CURRENT:
                *ch &= ~C_OVERCURRENT;
                break;
            case F_C_PORT_RESET:
                *ch &= ~C_RESET;
                break;
            default:
                break;
            }
        }
        return 0;
    case 0x2001: /* CLEAR_FEATURE (hub) */
    case 0x2003: /* SET_FEATURE (hub) */
        return 0;
    default:
        return NC_USB_STALL;
    }
}

/* The status change endpoint: bit n for a change on port n. */
static int hub_data(NcUsbDev *d, bool in, int ep, uint8_t *buf, int len)
{
    Hub *h = (Hub *)d;
    if (!in || ep != 1 || len < 1) {
        return NC_USB_STALL;
    }
    uint8_t bits = 0;
    for (unsigned p = 1; p <= h->nports; p++) {
        if (h->change[p]) {
            bits |= (uint8_t)(1u << p);
        }
    }
    if (!bits) {
        return NC_USB_NAK;
    }
    buf[0] = bits;
    return 1;
}

static NcUsbDev *hub_find(NcUsbDev *d, uint8_t addr)
{
    Hub *h = (Hub *)d;
    for (unsigned p = 1; p <= h->nports; p++) {
        if (h->child[p] && (h->status[p] & PORT_ENABLE) && !(h->status[p] & PORT_SUSPEND)) {
            NcUsbDev *f = nc_usb_find(h->child[p], addr);
            if (f) {
                return f;
            }
        }
    }
    return NULL;
}

static void hub_reset(NcUsbDev *d)
{
    Hub *h = (Hub *)d;
    for (unsigned p = 1; p <= h->nports; p++) {
        h->status[p] = PORT_POWER;
        h->change[p] = 0;
        if (h->child[p]) {
            h->status[p] |= PORT_CONNECTION | (h->child[p]->low_speed ? PORT_LOW_SPEED : 0);
            h->change[p] |= C_CONNECTION;
        }
    }
}

/* A hub (its find) frees what hangs off it first; a hub and a gamepad both
 * have their NcUsbDev first (NcUsbDev's layout is the savestate's: no room
 * for a destroy callback). */
void nc_usb_dev_free(NcUsbDev *d)
{
    if (d && d->find == hub_find) {
        Hub *h = (Hub *)d;
        for (unsigned p = 0; p <= MAX_PORTS; p++) {
            nc_usb_dev_free(h->child[p]);
        }
    }
    free(d);
}

NcUsbDev *nc_usb_hub_new(unsigned nports)
{
    Hub *h = calloc(1, sizeof(Hub));
    h->nports = nports > MAX_PORTS ? MAX_PORTS : nports;
    h->dev.name = "hub";
    h->dev.dev_desc = k_dev_desc;
    h->dev.dev_desc_len = sizeof(k_dev_desc);
    h->dev.cfg_desc = k_cfg_desc;
    h->dev.cfg_desc_len = sizeof(k_cfg_desc);
    h->dev.strings = k_strings;
    h->dev.nstrings = 3;
    h->dev.control = hub_control;
    h->dev.data = hub_data;
    h->dev.find = hub_find;
    h->dev.reset = hub_reset;
    h->dev.pending_addr = -1;
    return &h->dev;
}

void nc_usb_hub_attach(NcUsbDev *hub, unsigned port, NcUsbDev *child)
{
    Hub *h = (Hub *)hub;
    if (port && port <= h->nports) {
        h->child[port] = child;
        hub_reset(hub);
    }
}

void nc_usb_hub_state(NcUsbDev *hub, NcUsbHubState *s)
{
    const Hub *h = (const Hub *)hub;
    memset(s, 0, sizeof(*s));
    s->nports = h->nports;
    _Static_assert(sizeof(s->status) == sizeof(h->status), "hub ports");
    memcpy(s->status, h->status, sizeof(s->status));
    memcpy(s->change, h->change, sizeof(s->change));
}

void nc_usb_hub_state_set(NcUsbDev *hub, const NcUsbHubState *s)
{
    Hub *h = (Hub *)hub;
    memcpy(h->status, s->status, sizeof(h->status));
    memcpy(h->change, s->change, sizeof(h->change));
}

NcUsbDev *nc_usb_hub_child(NcUsbDev *hub, unsigned port)
{
    const Hub *h = (const Hub *)hub;
    return port && port <= h->nports ? h->child[port] : NULL;
}

void nc_usb_hub_port_changed(NcUsbDev *hub, unsigned port)
{
    Hub *h = (Hub *)hub;
    if (port && port <= h->nports) {
        h->status[port] = h->child[port] ? (uint16_t)(PORT_CONNECTION | (h->child[port]->low_speed ? PORT_LOW_SPEED : 0))
                                         : 0;
        h->change[port] |= C_CONNECTION;
    }
}
