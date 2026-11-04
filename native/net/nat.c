/*
 * hakuX native core — the NAT backend (net=nat): the Internet through a
 * user-mode TCP/IP stack (libslirp, as xemu's NAT), so the console reaches
 * Xbox Live through Insignia with no host privileges.
 *
 * The console's network is 10.0.2.0/24: the gateway is 10.0.2.2 and DHCP
 * leases from 10.0.2.15. The DNS server DHCP gives is the configured one
 * itself (dns=, Insignia's by default), outside that network: the queries
 * leave as any UDP does, so libslirp never looks for the host's resolver
 * (Android has no /etc/resolv.conf) and the console needs no settings. One
 * host socket serves each port the console sends from, whoever it sends to,
 * and takes datagrams from anyone (Xbox Live's peer-to-peer traffic on UDP
 * 3074). The phone's own loopback stays out of reach.
 *
 * Everything runs on the network thread: libslirp's descriptors join its
 * poll (fill/poll), its timers are checked there, and the guest's frames are
 * handed to it there (input).
 */
#include "native/net/net.h"
#include "native/core/nc.h"

#include <libslirp.h>

#include <arpa/inet.h>
#include <poll.h>
#include <string.h>
#include <time.h>

#define INSIGNIA_DNS "46.101.64.175"
#define TIMERS 4

typedef struct Timer {
    bool used, armed;
    int64_t expire_ms;
    SlirpTimerId id;
    void *cb_opaque;
} Timer;

static struct {
    Slirp *slirp;
    Timer timer[TIMERS];
    struct pollfd *fds;                 /* this round's, while fill and poll run */
    int nfds, maxfds;
    uint64_t overflow;                  /* descriptors that didn't fit */
} g_nat;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ------------------------------------------------- libslirp's callbacks */

static slirp_ssize_t cb_send_packet(const void *buf, size_t len, void *opaque)
{
    nc_net_deliver(buf, len);
    return (slirp_ssize_t)len;
}

static void cb_guest_error(const char *msg, void *opaque)
{
    nc_log("hakuX-native-net", "nat: guest error: %s", msg);
}

static int64_t cb_clock_get_ns(void *opaque)
{
    return now_ns();
}

static void *cb_timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque)
{
    for (unsigned i = 0; i < TIMERS; i++) {
        Timer *t = &g_nat.timer[i];
        if (!t->used) {
            *t = (Timer){ .used = true, .id = id, .cb_opaque = cb_opaque };
            return t;
        }
    }
    nc_log("hakuX-native-net", "nat: no timer free");
    return NULL;
}

static void cb_timer_free(void *timer, void *opaque)
{
    if (timer) {
        *(Timer *)timer = (Timer){ 0 };
    }
}

static void cb_timer_mod(void *timer, int64_t expire_ms, void *opaque)
{
    if (timer) {
        ((Timer *)timer)->expire_ms = expire_ms;
        ((Timer *)timer)->armed = true;
    }
}

static void cb_poll_fd(int fd, void *opaque)
{
}

static void cb_notify(void *opaque)
{
    /* nothing to wake: libslirp only ever runs on the network thread */
}

static const SlirpCb g_cb = {
    .send_packet = cb_send_packet,
    .guest_error = cb_guest_error,
    .clock_get_ns = cb_clock_get_ns,
    .timer_free = cb_timer_free,
    .timer_mod = cb_timer_mod,
    .register_poll_fd = cb_poll_fd,
    .unregister_poll_fd = cb_poll_fd,
    .notify = cb_notify,
    .timer_new_opaque = cb_timer_new_opaque,
};

/* ------------------------------------------------------------ backend */

static bool nat_init(void)
{
    const NcNetConfig *c = nc_net_config();
    struct in_addr dns = { .s_addr = c->dns[0] };
    if (!dns.s_addr) {
        inet_pton(AF_INET, INSIGNIA_DNS, &dns);
    }
    SlirpConfig cfg = {
        .version = 4,
        .in_enabled = true,
        .vhostname = "hakuX",
        .vnameserver = dns,
        .disable_host_loopback = true,
        .disable_dns = true,            /* dns is a real server: no redirect */
    };
    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
    g_nat.slirp = slirp_new(&cfg, &g_cb, NULL);
    if (!g_nat.slirp) {
        nc_log("hakuX-native-net", "nat: libslirp didn't start");
        return false;
    }
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dns, b, sizeof(b));
    nc_log("hakuX-native-net", "nat: libslirp %s, 10.0.2.0/24, DNS %s", slirp_version_string(), b);
    return true;
}

static void nat_input(const uint8_t *frame, size_t len)
{
    slirp_input(g_nat.slirp, frame, (int)len);
}

static int add_poll(int fd, int events, void *opaque)
{
    if (g_nat.nfds >= g_nat.maxfds) {
        g_nat.overflow++;
        return -1;
    }
    struct pollfd *p = &g_nat.fds[g_nat.nfds];
    p->fd = fd;
    p->events = (short)((events & SLIRP_POLL_IN ? POLLIN : 0) | (events & SLIRP_POLL_OUT ? POLLOUT : 0) |
                        (events & SLIRP_POLL_PRI ? POLLPRI : 0));
    p->revents = 0;
    return g_nat.nfds++;
}

static int get_revents(int idx, void *opaque)
{
    if (idx < 0 || idx >= g_nat.nfds) {
        return 0;
    }
    const short r = g_nat.fds[idx].revents;
    return (r & POLLIN ? SLIRP_POLL_IN : 0) | (r & POLLOUT ? SLIRP_POLL_OUT : 0) |
           (r & POLLPRI ? SLIRP_POLL_PRI : 0) | (r & POLLERR ? SLIRP_POLL_ERR : 0) |
           (r & POLLHUP ? SLIRP_POLL_HUP : 0);
}

static int nat_fill(struct pollfd *fds, int max, int *timeout_ms)
{
    g_nat.fds = fds;
    g_nat.maxfds = max;
    g_nat.nfds = 0;
    uint32_t timeout = (uint32_t)*timeout_ms;
    slirp_pollfds_fill(g_nat.slirp, &timeout, add_poll, NULL);
    const int64_t now = now_ns() / 1000000;
    for (unsigned i = 0; i < TIMERS; i++) {
        const Timer *t = &g_nat.timer[i];
        if (t->used && t->armed) {
            const int64_t left = t->expire_ms > now ? t->expire_ms - now : 0;
            timeout = left < timeout ? (uint32_t)left : timeout;
        }
    }
    *timeout_ms = (int)timeout;
    return g_nat.nfds;
}

static void nat_poll(struct pollfd *fds, int n)
{
    g_nat.fds = fds;
    g_nat.nfds = n;
    slirp_pollfds_poll(g_nat.slirp, 0, get_revents, NULL);
    const int64_t now = now_ns() / 1000000;
    for (unsigned i = 0; i < TIMERS; i++) {
        Timer *t = &g_nat.timer[i];
        if (t->used && t->armed && t->expire_ms <= now) {
            t->armed = false;
            slirp_handle_timer(g_nat.slirp, t->id, t->cb_opaque);
        }
    }
    static unsigned s_logged;
    if (g_nat.overflow && s_logged++ < 8) {
        nc_log("hakuX-native-net", "nat: %llu descriptors over the poll's %d", (unsigned long long)g_nat.overflow,
               g_nat.maxfds);
    }
    g_nat.overflow = 0;
}

const NcNetBackend nc_net_nat = {
    .name = "nat",
    .init = nat_init,
    .input = nat_input,
    .fill = nat_fill,
    .poll = nat_poll,
};
