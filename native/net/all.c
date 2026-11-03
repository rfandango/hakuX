/*
 * hakuX native core — the network of a real console's one cable (net=all):
 * the NAT backend (the Internet: Xbox Live through Insignia) and the LAN
 * backend (system link: the consoles on the local networks, Wi-Fi's and a
 * Wi-Fi Direct group's) behind one switch, as a home router's port has both.
 *
 * The guest's frames to the NAT's own addresses (its gateway and DNS, MACs
 * 52:55:...) go to it alone; broadcasts and multicasts (DHCP, ARP, system
 * link's discovery) to both; frames to other consoles to the LAN alone. What
 * either backend has for the guest goes to it; the other consoles' frames
 * never reach the NAT (theirs is their own), so each console keeps its own
 * gateway and address (nat.c leases one from the console's MAC), and they
 * reach each other on 10.0.2.0/24 as consoles on one home network do.
 */
#include "native/net/net.h"
#include "native/core/nc.h"

#include <poll.h>

static int g_nat_fds;                   /* this round's: the NAT's first, then the LAN's */

static bool all_init(void)
{
    return nc_net_nat.init() && nc_net_lan.init();
}

static void all_input(const uint8_t *frame, size_t len)
{
    if (len < 14) {
        return;
    }
    if (frame[0] & 1) {                 /* broadcast, multicast */
        nc_net_nat.input(frame, len);
        nc_net_lan.input(frame, len);
    } else if (nc_net_nat_address(frame)) {
        nc_net_nat.input(frame, len);
    } else {
        nc_net_lan.input(frame, len);
    }
}

static int all_fill(struct pollfd *fds, int max, int *timeout_ms)
{
    g_nat_fds = nc_net_nat.fill(fds, max - 1, timeout_ms);
    int lan_timeout = *timeout_ms;
    const int n = nc_net_lan.fill(fds + g_nat_fds, max - g_nat_fds, &lan_timeout);
    *timeout_ms = lan_timeout < *timeout_ms ? lan_timeout : *timeout_ms;
    return g_nat_fds + n;
}

static void all_poll(struct pollfd *fds, int n)
{
    nc_net_nat.poll(fds, g_nat_fds);
    nc_net_lan.poll(fds + g_nat_fds, n - g_nat_fds);
}

const NcNetBackend nc_net_all = {
    .name = "all",
    .init = all_init,
    .input = all_input,
    .fill = all_fill,
    .poll = all_poll,
};
