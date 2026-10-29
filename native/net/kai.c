/*
 * hakuX native core — the XLink Kai backend (net=kai): system link over the
 * Internet through an XLink Kai engine on the local network (Windows, macOS,
 * Linux, a Raspberry Pi; it doesn't run on Android), port 34523.
 *
 * The emulator protocol Dolphin speaks to the engine (its XLINK_KAI_BBA.cpp),
 * one UDP datagram each:
 *   connect;<id>;<emulator>;<40 zeros>   us, until the engine answers (id:
 *                                        hakuX-<8 hex digits>, this run's)
 *   connected                            the engine: frames flow
 *   e;e;<Ethernet frame>                 both ways
 *   keepalive...                         the engine: echoed back
 *   disconnected                         the engine let us go: connect again
 *   message;... chat;... directmessage;...   the engine's texts (logged)
 * A datagram that is a bare Ethernet frame is taken too (xemu sends the
 * engine its frames without the protocol). Only the engine's datagrams are
 * read; after SILENT_S without one (an engine restarted knows us no more)
 * we ask again. The engine may be named (kai=HOST): looked up then.
 */
#include "native/net/net.h"
#include "native/core/nc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CONNECT_EVERY_S 2
#define SILENT_S 10

static struct {
    int fd;
    struct sockaddr_in engine;
    bool found;                         /* engine: its address known */
    char connect[96];                   /* our connect command */
    bool connected;
    time_t asked;                       /* the last connect sent */
    time_t heard;                       /* the engine's last datagram */
    uint64_t frames_out, frames_in;
} g_kai = { .fd = -1 };

static void send_raw(const void *p, size_t len)
{
    if (!g_kai.found) {
        return;
    }
    if (sendto(g_kai.fd, p, len, 0, (const struct sockaddr *)&g_kai.engine, sizeof(g_kai.engine)) < 0 &&
        errno != EAGAIN) {
        static unsigned s_logged;
        if (s_logged++ < 8) {
            nc_log("hakuX-native-net", "kai: send: %s", strerror(errno));
        }
    }
}

/* The engine's address: given, or its name looked up (on the network
 * thread, as it connects: a name server can take its time). */
static bool find_engine(void)
{
    const NcNetConfig *c = nc_net_config();
    uint32_t ip = c->kai_ip;
    if (!ip) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
        const int e = getaddrinfo(c->kai_host, NULL, &hints, &res);
        if (e || !res) {
            static unsigned s_logged;
            if (s_logged++ < 4) {
                nc_log("hakuX-native-net", "kai: no address for %s: %s", c->kai_host, gai_strerror(e));
            }
            return false;
        }
        ip = ((const struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
    }
    g_kai.engine = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(c->kai_port), .sin_addr.s_addr = ip };
    g_kai.found = true;
    char a[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &g_kai.engine.sin_addr, a, sizeof(a));
    nc_log("hakuX-native-net", "kai: engine at %s:%u, we are %.14s", a, c->kai_port, g_kai.connect + 8);
    return true;
}

static void ask(void)
{
    g_kai.asked = time(NULL);
    if (g_kai.found || find_engine()) {
        send_raw(g_kai.connect, strlen(g_kai.connect));
    }
}

static bool kai_init(void)
{
    const NcNetConfig *c = nc_net_config();
    if (!c->kai_host[0]) {
        nc_log("hakuX-native-net", "kai: no engine address (kai=HOST)");
        return false;
    }
    g_kai.fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_kai.fd < 0) {
        nc_log("hakuX-native-net", "kai: socket: %s", strerror(errno));
        return false;
    }
    snprintf(g_kai.connect, sizeof(g_kai.connect),
             "connect;hakuX-%08x;hakuX;0000000000000000000000000000000000000000", arc4random());
    return true;                        /* the first connect from the network thread */
}

static void kai_input(const uint8_t *frame, size_t len)
{
    uint8_t buf[NC_NET_FRAME_MAX + 4];
    if (len > NC_NET_FRAME_MAX) {
        return;
    }
    memcpy(buf, "e;e;", 4);
    memcpy(buf + 4, frame, len);
    send_raw(buf, len + 4);
    g_kai.frames_out++;
}

static int kai_fill(struct pollfd *fds, int max, int *timeout_ms)
{
    fds[0] = (struct pollfd){ .fd = g_kai.fd, .events = POLLIN };
    *timeout_ms = CONNECT_EVERY_S * 1000;
    return 1;
}

static bool starts(const uint8_t *p, size_t n, const char *s)
{
    const size_t k = strlen(s);
    return n >= k && !memcmp(p, s, k);
}

static void kai_poll(struct pollfd *fds, int n)
{
    const time_t now = time(NULL);
    const bool silent = g_kai.connected && now - g_kai.heard >= SILENT_S;
    if ((!g_kai.connected || silent) && now - g_kai.asked >= CONNECT_EVERY_S) {
        ask();
    }
    if (!(fds[0].revents & POLLIN)) {
        return;
    }
    uint8_t buf[NC_NET_FRAME_MAX + 64];
    for (unsigned i = 0; i < 256; i++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const ssize_t r = recvfrom(g_kai.fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) {
            break;
        }
        if (from.sin_addr.s_addr != g_kai.engine.sin_addr.s_addr || from.sin_port != g_kai.engine.sin_port) {
            continue;                       /* not the engine's */
        }
        g_kai.heard = time(NULL);
        const size_t len = (size_t)r;
        if (len > 4 && buf[2] == 'e' && buf[3] == ';') {
            if (len - 4 <= NC_NET_FRAME_MAX) {
                nc_net_deliver(buf + 4, len - 4);
                g_kai.frames_in++;
            }
        } else if (starts(buf, len, "keepalive")) {
            send_raw(buf, len);
        } else if (starts(buf, len, "connected")) {
            if (!g_kai.connected) {
                nc_log("hakuX-native-net", "kai: connected to the engine");
            }
            g_kai.connected = true;
        } else if (starts(buf, len, "disconnected")) {
            nc_log("hakuX-native-net", "kai: the engine disconnected us");
            g_kai.connected = false;
            ask();
        } else if (starts(buf, len, "message;") || starts(buf, len, "chat;") || starts(buf, len, "directmessage;")) {
            buf[len] = 0;
            nc_log("hakuX-native-net", "kai: %s", (const char *)buf);
        } else if (len >= 14 && len <= NC_NET_FRAME_MAX) {
            nc_net_deliver(buf, len);       /* a bare frame */
            g_kai.frames_in++;
        }
    }
}

static void kai_close(void)
{
    if (g_kai.fd >= 0) {
        close(g_kai.fd);
    }
    memset(&g_kai, 0, sizeof(g_kai));
    g_kai.fd = -1;
}

const NcNetBackend nc_net_kai = {
    .name = "kai",
    .init = kai_init,
    .close = kai_close,
    .input = kai_input,
    .fill = kai_fill,
    .poll = kai_poll,
};
