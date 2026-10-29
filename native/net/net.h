/*
 * hakuX native core — the network behind the emulated NIC (native/dev/nvnet.c).
 *
 * The NIC hands whole Ethernet frames to the network and takes the frames it
 * received; in between, one host thread runs the backend:
 *
 *   off  no network: the link is down (titles see no cable)
 *   nat  a user-mode TCP/IP stack and NAT (libslirp): DHCP, the Internet;
 *        Xbox Live through Insignia, its DNS server given by DHCP
 *   lan  Ethernet frames in UDP datagrams among the consoles on the local
 *        network (system link): broadcast to the subnet, unicast once a MAC
 *        is known; xemu's UDP tunnel carries the same datagrams, and peers
 *        named directly reach consoles on a VPN (ZeroTier, Tailscale)
 *   kai  system link over the Internet through XLink Kai: frames to and from
 *        a Kai engine on the local network (a PC, a Raspberry Pi), the
 *        emulator protocol Dolphin speaks to it
 *
 * The network thread never touches guest memory: frames go through two
 * queues, guest to network (nc_net_send, any thread) and network to guest
 * (nc_net_recv, the NIC's thread).
 */
#ifndef NATIVE_NET_NET_H
#define NATIVE_NET_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NcNetMode { NC_NET_OFF, NC_NET_NAT, NC_NET_LAN, NC_NET_KAI } NcNetMode;

#define NC_NET_FRAME_MAX 1536           /* an Ethernet frame, header included, no FCS */

/* Options (native/core/options.c), before the boot:
 *   net=off|nat|lan|kai
 *   dns=A.B.C.D[,A.B.C.D]   nat: the DNS servers DHCP gives the guest
 *   lanport=N               lan: the UDP port (all consoles the same)
 *   lanbcast=A.B.C.D[/BITS] lan: the local network's broadcast address
 *                           (and its prefix: who is outside it)
 *   lanpeers=IP[:PORT],...  lan: consoles also reached directly (another
 *                           subnet, a tunnel's far end)
 *   kai=HOST[:PORT]         kai: the XLink Kai engine (port 34523)
 *   pcap=PATH               debug: every frame sent and received, to a
 *                           capture file (Wireshark's)
 * True if arg is one of them. */
bool nc_net_option(const char *arg);
NcNetMode nc_net_mode(void);
/* The boot: the backend's thread starts (nothing in mode off). */
void nc_net_start(void);
/* After the NIC stops: join the backend and release this run's queues,
 * sockets and capture file. Configuration is kept for the next boot. */
void nc_net_stop(void);
/* Whether the NIC's link is up (a backend running). */
bool nc_net_link(void);

/* The NIC: a frame to send (copied; dropped when the queue is full). */
void nc_net_send(const uint8_t *frame, size_t len);
/* The NIC: the station address the guest gave it (its frames' source).
 * nc_net_mac: it, copied to mac; false while the guest hasn't set one. */
void nc_net_set_mac(const uint8_t mac[6]);
bool nc_net_mac(uint8_t mac[6]);
/* The NIC's thread: the oldest frame received, copied to buf (cap bytes);
 * its length, 0 when none. nc_net_peek gives the next one's length without
 * taking it (a frame the guest has no buffer for yet stays queued). */
size_t nc_net_recv(uint8_t *buf, size_t cap);
size_t nc_net_peek(void);
/* The NIC's thread waits for a received frame, at most timeout_ns. */
void nc_net_wait_rx(uint64_t timeout_ns);

/* A console with this one's MAC (its EEPROM a copy of this one's, the app's
 * hard disk copied from device to device), heard at ip (network order): the
 * two can't see each other. nc_net_take_twin, for the app: its address, once
 * each console; false when no new one. */
void nc_net_twin(uint32_t ip);
bool nc_net_take_twin(char *addr, size_t len);

/* ---- backends (native/net/nat.c, lan.c) ---- */

struct pollfd;
typedef struct NcNetBackend {
    const char *name;
    bool (*init)(void);
    void (*close)(void);                /* after its thread has joined; partial init too */
    /* a guest frame, on the network thread */
    void (*input)(const uint8_t *frame, size_t len);
    /* the descriptors to poll (up to max) and the longest wait (ms);
     * returns how many */
    int (*fill)(struct pollfd *fds, int max, int *timeout_ms);
    /* after the poll (the same descriptors, their revents) */
    void (*poll)(struct pollfd *fds, int n);
} NcNetBackend;

extern const NcNetBackend nc_net_nat, nc_net_lan, nc_net_kai;

/* A backend, on the network thread: a frame for the guest. */
void nc_net_deliver(const uint8_t *frame, size_t len);

/* Backends' configuration (the options). */
typedef struct NcNetConfig {
    uint32_t dns[2];                    /* network order, 0: none */
    uint16_t lan_port;
    uint32_t lan_bcast;                 /* network order, 0: 255.255.255.255 */
    uint32_t lan_mask;                  /* network order, 0: not known */
    unsigned npeers;
    uint32_t peer_ip[8];                /* network order */
    uint16_t peer_port[8];
    char kai_host[64];                  /* as given, "" none */
    uint32_t kai_ip;                    /* network order, 0: kai_host is a name */
    uint16_t kai_port;
} NcNetConfig;
const NcNetConfig *nc_net_config(void);

#ifdef __cplusplus
}
#endif

#endif
