/*
 * hakuX native core — the LAN backend (net=lan): the console's Ethernet on a
 * UDP socket, one frame a datagram (as xemu's UDP tunnel carries them), so
 * the consoles on a local network form one Ethernet segment (system link).
 *
 * Broadcast and multicast frames (system link's discovery) go to the
 * networks' broadcast addresses (Wi-Fi's, a Wi-Fi Direct group's: lanbcast
 * names up to NC_NET_BCASTS), to every peer named in the options and to
 * the consoles heard from outside the local network (a VPN's: one console
 * naming the other is enough); a frame to a MAC seen before goes straight to
 * the address it came from, one not seen yet is flooded as a broadcast is.
 * The named peers get a hello every HELLO_S (a frame of our own type, never
 * delivered) so that they hear of us while the game sends nothing. Frames the
 * console sent itself (its broadcasts come back) are dropped. Every console
 * uses the same port.
 *
 * A console reached over two networks at once (Wi-Fi and a Wi-Fi Direct
 * group) sends each broadcast over both: the second copy, within DUP_MS, is
 * dropped, and the console's address changes network only once the one in
 * use has been silent for MOVE_S.
 */
#include "native/net/net.h"
#include "native/core/nc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MACS 64
#define MAC_TTL_S 300
#define HELLO_S 10
#define HELLO_TYPE 0x88B5               /* IEEE 802 local experimental */
#define DUP_MS 100
#define DUPS 256               /* the frames received in DUP_MS, from up to 8 consoles */
#define MOVE_S 3

typedef struct Station {
    uint8_t mac[6];
    struct sockaddr_in at;
    time_t seen;
    time_t at_seen;                     /* the last frame from at */
    bool remote;                        /* outside the local network: flooded to */
} Station;

static struct {
    int fd;
    struct sockaddr_in bcast[NC_NET_BCASTS];
    unsigned nbcast;
    uint8_t own[6];                     /* the console's MAC (its frames' source) */
    bool own_known;
    Station st[MACS];
    time_t hello;                       /* the last hello sent */
    uint64_t flooded, unicast, dropped_own, dropped_dup;
    struct { uint64_t hash, ms; } dup[DUPS];   /* frames received lately */
    unsigned dup_next;
} g_lan = { .fd = -1 };

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Whether this frame came in already, within DUP_MS (over the other network). */
static bool duplicate(const uint8_t *frame, size_t len)
{
    uint64_t h = 1469598103934665603ull;    /* FNV-1a */
    for (size_t i = 0; i < len; i++) {
        h = (h ^ frame[i]) * 1099511628211ull;
    }
    const uint64_t now = now_ms();
    for (unsigned i = 0; i < DUPS; i++) {
        if (g_lan.dup[i].hash == h && now - g_lan.dup[i].ms < DUP_MS) {
            return true;
        }
    }
    g_lan.dup[g_lan.dup_next] = (typeof(g_lan.dup[0])){ h, now };
    g_lan.dup_next = (g_lan.dup_next + 1) % DUPS;
    return false;
}

static bool lan_init(void)
{
    const NcNetConfig *c = nc_net_config();
    g_lan.fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_lan.fd < 0) {
        nc_log("hakuX-native-net", "lan: socket: %s", strerror(errno));
        return false;
    }
    const int one = 1;
    setsockopt(g_lan.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(g_lan.fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    const int buf = 1 << 20;
    setsockopt(g_lan.fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(c->lan_port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(g_lan.fd, (struct sockaddr *)&me, sizeof(me)) != 0) {
        nc_log("hakuX-native-net", "lan: port %u: %s", c->lan_port, strerror(errno));
        close(g_lan.fd);
        g_lan.fd = -1;
        return false;
    }
    g_lan.nbcast = c->nbcast ? c->nbcast : 1;
    char list[NC_NET_BCASTS * 24] = "";
    for (unsigned i = 0; i < g_lan.nbcast; i++) {
        g_lan.bcast[i] = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(c->lan_port),
                                               .sin_addr.s_addr = c->nbcast ? c->lan_bcast[i] : INADDR_BROADCAST };
        char b[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &g_lan.bcast[i].sin_addr, b, sizeof(b));
        snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s (/%d)", i ? ", " : "", b,
                 __builtin_popcount(ntohl(c->lan_mask[i])));
    }
    nc_log("hakuX-native-net", "lan: UDP port %u, broadcast to %s, %u peers named", c->lan_port, list, c->npeers);
    return true;
}

static void send_to(const struct sockaddr_in *to, const uint8_t *frame, size_t len)
{
    if (sendto(g_lan.fd, frame, len, 0, (const struct sockaddr *)to, sizeof(*to)) < 0 && errno != EAGAIN) {
        static unsigned s_logged;
        if (s_logged++ < 8) {
            nc_log("hakuX-native-net", "lan: send: %s", strerror(errno));
        }
    }
}

static bool named(const struct sockaddr_in *a)
{
    const NcNetConfig *c = nc_net_config();
    for (unsigned i = 0; i < c->npeers; i++) {
        if (c->peer_ip[i] == a->sin_addr.s_addr && htons(c->peer_port[i]) == a->sin_port) {
            return true;
        }
    }
    return false;
}

static void to_peers(const uint8_t *frame, size_t len)
{
    const NcNetConfig *c = nc_net_config();
    for (unsigned i = 0; i < c->npeers; i++) {
        const struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(c->peer_port[i]),
                                        .sin_addr.s_addr = c->peer_ip[i] };
        send_to(&to, frame, len);
    }
}

static void flood(const uint8_t *frame, size_t len)
{
    for (unsigned i = 0; i < g_lan.nbcast; i++) {
        send_to(&g_lan.bcast[i], frame, len);
    }
    to_peers(frame, len);
    const time_t now = time(NULL);
    for (unsigned i = 0; i < MACS; i++) {
        const Station *s = &g_lan.st[i];
        if (s->remote && s->seen && now - s->seen < MAC_TTL_S && !named(&s->at)) {
            send_to(&s->at, frame, len);
        }
    }
    g_lan.flooded++;
}

static void hello(void)
{
    uint8_t f[60] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    memcpy(f + 6, g_lan.own, 6);
    f[12] = HELLO_TYPE >> 8;
    f[13] = HELLO_TYPE & 0xff;
    memcpy(f + 14, "hakuX hello", 11);
    to_peers(f, sizeof(f));
    g_lan.hello = time(NULL);
}

static bool is_hello(const uint8_t *frame, size_t len)
{
    return len >= 25 && frame[12] == (HELLO_TYPE >> 8) && frame[13] == (HELLO_TYPE & 0xff) &&
           !memcmp(frame + 14, "hakuX hello", 11);
}

static Station *station(const uint8_t *mac)
{
    const time_t now = time(NULL);
    for (unsigned i = 0; i < MACS; i++) {
        if (g_lan.st[i].seen && now - g_lan.st[i].seen < MAC_TTL_S && !memcmp(g_lan.st[i].mac, mac, 6)) {
            return &g_lan.st[i];
        }
    }
    return NULL;
}

/* Our MAC: the NIC's station address, else the first frame's source. */
static bool own(const uint8_t *frame)
{
    if (g_lan.own_known) {
        return true;
    }
    if (!nc_net_mac(g_lan.own)) {
        if (!frame) {
            return false;
        }
        memcpy(g_lan.own, frame + 6, 6);
    }
    g_lan.own_known = true;
    nc_log("hakuX-native-net", "lan: this console is %02x:%02x:%02x:%02x:%02x:%02x", g_lan.own[0], g_lan.own[1],
           g_lan.own[2], g_lan.own[3], g_lan.own[4], g_lan.own[5]);
    return true;
}

static void lan_input(const uint8_t *frame, size_t len)
{
    if (len < 14) {
        return;
    }
    own(frame);
    const Station *s = (frame[0] & 1) ? NULL : station(frame);
    if (s) {
        send_to(&s->at, frame, len);
        g_lan.unicast++;
    } else {
        flood(frame, len);
    }
}

static void learn(const uint8_t *mac, const struct sockaddr_in *from)
{
    if (mac[0] & 1) {
        return;                         /* not a station's address */
    }
    const NcNetConfig *c = nc_net_config();
    /* outside every local network known (their prefixes) */
    bool remote = false;
    for (unsigned i = 0; i < c->nbcast; i++) {
        if (!c->lan_mask[i]) {
            remote = false;
            break;
        }
        remote = (from->sin_addr.s_addr & c->lan_mask[i]) != (c->lan_bcast[i] & c->lan_mask[i]);
        if (!remote) {
            break;
        }
    }
    const time_t now = time(NULL);
    Station *free_slot = NULL, *oldest = &g_lan.st[0];
    for (unsigned i = 0; i < MACS; i++) {
        Station *s = &g_lan.st[i];
        if (s->seen && !memcmp(s->mac, mac, 6)) {
            s->seen = now;
            const bool same = s->at.sin_addr.s_addr == from->sin_addr.s_addr && s->at.sin_port == from->sin_port;
            if (same) {
                s->at_seen = now;
            } else if (now - s->at_seen >= MOVE_S) {
                char a[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from->sin_addr, a, sizeof(a));
                nc_log("hakuX-native-net", "lan: %02x:%02x:%02x:%02x:%02x:%02x now at %s:%u", mac[0], mac[1], mac[2],
                       mac[3], mac[4], mac[5], a, ntohs(from->sin_port));
                s->at = *from;
                s->at_seen = now;
                s->remote = remote;
            }
            return;
        }
        if (!s->seen && !free_slot) {
            free_slot = s;
        }
        if (s->seen < oldest->seen) {
            oldest = s;
        }
    }
    Station *s = free_slot ? free_slot : oldest;
    memcpy(s->mac, mac, 6);
    s->at = *from;
    s->seen = now;
    s->at_seen = now;
    s->remote = remote;
    char a[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from->sin_addr, a, sizeof(a));
    nc_log("hakuX-native-net", "lan: console %02x:%02x:%02x:%02x:%02x:%02x at %s:%u%s", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5], a, ntohs(from->sin_port), remote ? " (outside the local network)" : "");
}

static int lan_fill(struct pollfd *fds, int max, int *timeout_ms)
{
    fds[0] = (struct pollfd){ .fd = g_lan.fd, .events = POLLIN };
    return 1;
}

/* Whether a is one of this device's addresses. */
static bool local_address(const struct sockaddr_in *a)
{
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) {
        return true;                        /* can't tell: say nothing */
    }
    bool mine = false;
    for (const struct ifaddrs *i = list; i && !mine; i = i->ifa_next) {
        mine = i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
               ((const struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == a->sin_addr.s_addr;
    }
    freeifaddrs(list);
    return mine;
}

/* A frame with our MAC from another device: a console with this one's
 * EEPROM (a hard disk's copy restored on two), which neither hears. */
static void twin(const struct sockaddr_in *from)
{
    static uint32_t s_self, s_told;     /* our broadcasts' source; the one logged */
    if (from->sin_addr.s_addr == s_self || from->sin_addr.s_addr == s_told) {
        return;
    }
    if (local_address(from)) {
        s_self = from->sin_addr.s_addr;
        return;
    }
    s_told = from->sin_addr.s_addr;
    char a[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from->sin_addr, a, sizeof(a));
    nc_log("hakuX-native-net", "lan: the console at %s has this one's MAC (its EEPROM a copy of this one's): "
           "they can't see each other", a);
    nc_net_twin(from->sin_addr.s_addr);     /* the app says so */
}

static void lan_poll(struct pollfd *fds, int n)
{
    if (nc_net_config()->npeers && time(NULL) - g_lan.hello >= HELLO_S && own(NULL)) {
        hello();
    }
    if (!(fds[0].revents & POLLIN)) {
        return;
    }
    uint8_t buf[NC_NET_FRAME_MAX + 64];
    for (unsigned i = 0; i < 256; i++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const ssize_t r = recvfrom(g_lan.fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (r < 0) {
            break;
        }
        if (r < 14 || r > NC_NET_FRAME_MAX) {
            continue;                       /* not one of our frames */
        }
        if (g_lan.own_known && !memcmp(buf + 6, g_lan.own, 6)) {
            g_lan.dropped_own++;            /* our own broadcast, back */
            twin(&from);
            continue;
        }
        if (duplicate(buf, (size_t)r)) {
            g_lan.dropped_dup++;
            continue;
        }
        learn(buf + 6, &from);
        if (!is_hello(buf, (size_t)r)) {
            nc_net_deliver(buf, (size_t)r);
        }
    }
}

const NcNetBackend nc_net_lan = {
    .name = "lan",
    .init = lan_init,
    .input = lan_input,
    .fill = lan_fill,
    .poll = lan_poll,
};
