/*
 * hakuX native core — the network thread and the NIC's frame queues (net.h).
 */
#include "native/net/net.h"
#include "native/core/nc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define TX_SLOTS 128
#define RX_SLOTS 256
#define MAX_FDS 64

typedef struct Frame {
    uint16_t len;
    uint8_t data[NC_NET_FRAME_MAX];
} Frame;

typedef struct Ring {
    Frame *slot;
    unsigned n, head, tail;             /* head - tail frames queued */
    uint64_t dropped;
} Ring;

static struct {
    NcNetMode mode;
    NcNetConfig cfg;
    const NcNetBackend *be;
    bool running;
    pthread_t thread;
    bool started, quit;
    pthread_mutex_t lock;               /* both rings */
    pthread_cond_t rx_cond;
    Ring tx, rx;
    int wake_fd;                        /* eventfd: frames to send */
    uint8_t mac[6];                     /* the NIC's station address, 0: none yet */
    uint64_t sent, received;
    char pcap_path[128];                /* pcap=: "" none */
    FILE *pcap;                         /* under lock */
    uint32_t twin;                      /* a console with our MAC, not told yet; 0: none */
} g_net = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .rx_cond = PTHREAD_COND_INITIALIZER,
    .wake_fd = -1,
    .cfg = { .lan_port = 3075, .kai_port = 34523 },
};

static bool ring_init(Ring *r, unsigned n)
{
    r->slot = calloc(n, sizeof(Frame));
    r->n = n;
    return r->slot != NULL;
}

static bool ring_put(Ring *r, const uint8_t *frame, size_t len)
{
    if (!r->slot || len > NC_NET_FRAME_MAX || r->head - r->tail >= r->n) {
        r->dropped++;
        return false;
    }
    Frame *f = &r->slot[r->head % r->n];
    f->len = (uint16_t)len;
    memcpy(f->data, frame, len);
    r->head++;
    return true;
}

/* ------------------------------------------------------------- options */

static bool parse_ip(const char *s, uint32_t *ip)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) {
        return false;
    }
    *ip = a.s_addr;
    return true;
}

static void parse_peers(const char *v)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", v);
    g_net.cfg.npeers = 0;
    for (char *save = NULL, *t = strtok_r(buf, ",", &save); t && g_net.cfg.npeers < 8;
         t = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(t, ':');
        unsigned port = g_net.cfg.lan_port;
        if (colon) {
            *colon = 0;
            port = (unsigned)atoi(colon + 1);
        }
        uint32_t ip;
        if (parse_ip(t, &ip) && port && port < 65536) {
            g_net.cfg.peer_ip[g_net.cfg.npeers] = ip;
            g_net.cfg.peer_port[g_net.cfg.npeers++] = (uint16_t)port;
        }
    }
}

bool nc_net_option(const char *arg)
{
    if (!strncmp(arg, "net=", 4)) {
        const char *v = arg + 4;
        g_net.mode = !strcmp(v, "nat") ? NC_NET_NAT : !strcmp(v, "lan") ? NC_NET_LAN
                     : !strcmp(v, "kai") ? NC_NET_KAI : NC_NET_OFF;
    } else if (!strncmp(arg, "dns=", 4)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", arg + 4);
        char *comma = strchr(buf, ',');
        if (comma) {
            *comma = 0;
            parse_ip(comma + 1, &g_net.cfg.dns[1]);
        }
        parse_ip(buf, &g_net.cfg.dns[0]);
    } else if (!strncmp(arg, "lanport=", 8)) {
        const int p = atoi(arg + 8);
        if (p > 0 && p < 65536) {
            g_net.cfg.lan_port = (uint16_t)p;
        }
    } else if (!strncmp(arg, "lanbcast=", 9)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s", arg + 9);
        char *slash = strchr(buf, '/');
        if (slash) {
            *slash = 0;
            const int bits = atoi(slash + 1);
            if (bits > 0 && bits <= 32) {
                g_net.cfg.lan_mask = htonl(bits == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> bits));
            }
        }
        parse_ip(buf, &g_net.cfg.lan_bcast);
    } else if (!strncmp(arg, "lanpeers=", 9)) {
        parse_peers(arg + 9);
    } else if (!strncmp(arg, "kai=", 4)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", arg + 4);
        char *colon = strchr(buf, ':');
        if (colon) {
            *colon = 0;
            const int p = atoi(colon + 1);
            g_net.cfg.kai_port = p > 0 && p < 65536 ? (uint16_t)p : g_net.cfg.kai_port;
        }
        snprintf(g_net.cfg.kai_host, sizeof(g_net.cfg.kai_host), "%s", buf);
        g_net.cfg.kai_ip = 0;
        parse_ip(buf, &g_net.cfg.kai_ip);       /* else a name, looked up as it connects */
    } else if (!strncmp(arg, "pcap=", 5)) {
        snprintf(g_net.pcap_path, sizeof(g_net.pcap_path), "%s", arg + 5);
    } else {
        return false;
    }
    return true;
}

NcNetMode nc_net_mode(void)
{
    return g_net.mode;
}

const NcNetConfig *nc_net_config(void)
{
    return &g_net.cfg;
}

bool nc_net_link(void)
{
    return __atomic_load_n(&g_net.running, __ATOMIC_ACQUIRE);
}

/* ---------------------------------------------------------- the capture */

static void pcap_open(void)
{
    if (!g_net.pcap_path[0]) {
        return;
    }
    g_net.pcap = fopen(g_net.pcap_path, "wb");
    if (!g_net.pcap) {
        nc_log("hakuX-native-net", "pcap: %s: %s", g_net.pcap_path, strerror(errno));
        return;
    }
    /* magic, version 2.4, GMT offset, accuracy, snapshot length, Ethernet */
    const uint32_t head[6] = { 0xA1B2C3D4, 0x00040002, 0, 0, 65535, 1 };
    fwrite(head, sizeof(head), 1, g_net.pcap);
    fflush(g_net.pcap);
    nc_log("hakuX-native-net", "pcap: frames to %s", g_net.pcap_path);
}

/* A frame to the capture; under lock. */
static void pcap_frame(const uint8_t *frame, size_t len)
{
    if (!g_net.pcap) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const uint32_t rec[4] = { (uint32_t)ts.tv_sec, (uint32_t)(ts.tv_nsec / 1000), (uint32_t)len, (uint32_t)len };
    fwrite(rec, sizeof(rec), 1, g_net.pcap);
    fwrite(frame, len, 1, g_net.pcap);
    fflush(g_net.pcap);
}

/* ---------------------------------------------------------- the queues */

void nc_net_send(const uint8_t *frame, size_t len)
{
    if (!nc_net_link()) {
        return;
    }
    pthread_mutex_lock(&g_net.lock);
    const bool ok = ring_put(&g_net.tx, frame, len);
    pthread_mutex_unlock(&g_net.lock);
    if (ok) {
        const uint64_t one = 1;
        (void)!write(g_net.wake_fd, &one, sizeof(one));
    }
}

void nc_net_set_mac(const uint8_t mac[6])
{
    pthread_mutex_lock(&g_net.lock);
    memcpy(g_net.mac, mac, 6);
    pthread_mutex_unlock(&g_net.lock);
}

bool nc_net_mac(uint8_t mac[6])
{
    static const uint8_t none[6];
    pthread_mutex_lock(&g_net.lock);
    memcpy(mac, g_net.mac, 6);
    pthread_mutex_unlock(&g_net.lock);
    return memcmp(mac, none, 6) != 0;
}

void nc_net_twin(uint32_t ip)
{
    pthread_mutex_lock(&g_net.lock);
    g_net.twin = ip;
    pthread_mutex_unlock(&g_net.lock);
}

bool nc_net_take_twin(char *addr, size_t len)
{
    pthread_mutex_lock(&g_net.lock);
    const struct in_addr a = { .s_addr = g_net.twin };
    g_net.twin = 0;
    pthread_mutex_unlock(&g_net.lock);
    return a.s_addr && inet_ntop(AF_INET, &a, addr, (socklen_t)len);
}

void nc_net_deliver(const uint8_t *frame, size_t len)
{
    pthread_mutex_lock(&g_net.lock);
    pcap_frame(frame, len);
    if (ring_put(&g_net.rx, frame, len)) {
        g_net.received++;
        pthread_cond_signal(&g_net.rx_cond);
    }
    pthread_mutex_unlock(&g_net.lock);
}

size_t nc_net_peek(void)
{
    pthread_mutex_lock(&g_net.lock);
    const Ring *r = &g_net.rx;
    const size_t len = r->head != r->tail ? r->slot[r->tail % r->n].len : 0;
    pthread_mutex_unlock(&g_net.lock);
    return len;
}

size_t nc_net_recv(uint8_t *buf, size_t cap)
{
    size_t len = 0;
    pthread_mutex_lock(&g_net.lock);
    Ring *r = &g_net.rx;
    if (r->head != r->tail) {
        const Frame *f = &r->slot[r->tail % r->n];
        len = f->len <= cap ? f->len : 0;
        memcpy(buf, f->data, len);
        r->tail++;
    }
    pthread_mutex_unlock(&g_net.lock);
    return len;
}

void nc_net_wait_rx(uint64_t timeout_ns)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);   /* rx_cond's default clock */
    ts.tv_sec += (time_t)(timeout_ns / 1000000000);
    ts.tv_nsec += (long)(timeout_ns % 1000000000);
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&g_net.lock);
    if (g_net.rx.head == g_net.rx.tail) {
        pthread_cond_timedwait(&g_net.rx_cond, &g_net.lock, &ts);
    }
    pthread_mutex_unlock(&g_net.lock);
}

/* ---------------------------------------------------------- the thread */

static void *net_thread(void *arg)
{
    const NcNetBackend *be = g_net.be;
    static Frame f;
    struct pollfd fds[MAX_FDS];
    time_t last_log = 0;
    while (!__atomic_load_n(&g_net.quit, __ATOMIC_ACQUIRE)) {
        fds[0] = (struct pollfd){ .fd = g_net.wake_fd, .events = POLLIN };
        int timeout = 1000;
        const int n = be->fill(fds + 1, MAX_FDS - 1, &timeout);
        if (poll(fds, (nfds_t)(1 + n), timeout) < 0 && errno != EINTR) {
            nc_log("hakuX-native-net", "poll: %s", strerror(errno));
            usleep(10000);
            continue;
        }
        if (fds[0].revents & POLLIN) {
            uint64_t v;
            (void)!read(g_net.wake_fd, &v, sizeof(v));
        }
        if (__atomic_load_n(&g_net.quit, __ATOMIC_ACQUIRE)) {
            break;
        }
        for (;;) {                      /* the guest's frames, one at a time */
            pthread_mutex_lock(&g_net.lock);
            Ring *r = &g_net.tx;
            const bool any = r->head != r->tail;
            if (any) {
                f = r->slot[r->tail % r->n];
                r->tail++;
                pcap_frame(f.data, f.len);
            }
            pthread_mutex_unlock(&g_net.lock);
            if (!any) {
                break;
            }
            g_net.sent++;
            be->input(f.data, f.len);
        }
        be->poll(fds + 1, n);
        const time_t now = time(NULL);
        if (now - last_log >= 30) {
            last_log = now;
            nc_log("hakuX-native-net", "%s: %llu frames sent, %llu received (dropped: %llu out, %llu in)", be->name,
                   (unsigned long long)g_net.sent, (unsigned long long)g_net.received,
                   (unsigned long long)g_net.tx.dropped, (unsigned long long)g_net.rx.dropped);
        }
    }
    return NULL;
}

void nc_net_start(void)
{
    nc_net_stop();
    /* debug: NC_NET="option option ..." over the app's (via debug.hakux.env) */
    const char *env = getenv("NC_NET");
    if (env) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", env);
        for (char *save = NULL, *t = strtok_r(buf, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
            nc_net_option(t);
        }
    }
    const NcNetBackend *be = g_net.mode == NC_NET_NAT   ? &nc_net_nat
                             : g_net.mode == NC_NET_LAN ? &nc_net_lan
                             : g_net.mode == NC_NET_KAI ? &nc_net_kai
                                                        : NULL;
    if (!be) {
        nc_log("hakuX-native-net", "network off: the NIC has no link");
        return;
    }
    g_net.be = be;
    g_net.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_net.wake_fd < 0 || !ring_init(&g_net.tx, TX_SLOTS) || !ring_init(&g_net.rx, RX_SLOTS) || !be->init()) {
        nc_log("hakuX-native-net", "%s: couldn't start: the NIC has no link", be->name);
        nc_net_stop();
        return;
    }
    pcap_open();
    __atomic_store_n(&g_net.quit, false, __ATOMIC_RELEASE);
    if (pthread_create(&g_net.thread, NULL, net_thread, NULL) != 0) {
        nc_log("hakuX-native-net", "%s: no thread: the NIC has no link", be->name);
        nc_net_stop();
        return;
    }
    g_net.started = true;
    __atomic_store_n(&g_net.running, true, __ATOMIC_RELEASE);
    nc_log("hakuX-native-net", "%s: started", be->name);
}

void nc_net_stop(void)
{
    __atomic_store_n(&g_net.running, false, __ATOMIC_RELEASE);
    __atomic_store_n(&g_net.quit, true, __ATOMIC_RELEASE);
    if (g_net.started) {
        const uint64_t one = 1;
        (void)!write(g_net.wake_fd, &one, sizeof(one));
        pthread_join(g_net.thread, NULL);
        g_net.started = false;
    }
    if (g_net.be && g_net.be->close) {
        g_net.be->close();
    }
    g_net.be = NULL;
    if (g_net.wake_fd >= 0) {
        close(g_net.wake_fd);
        g_net.wake_fd = -1;
    }
    pthread_mutex_lock(&g_net.lock);
    if (g_net.pcap) {
        fclose(g_net.pcap);
        g_net.pcap = NULL;
    }
    free(g_net.tx.slot);
    free(g_net.rx.slot);
    g_net.tx = (Ring){0};
    g_net.rx = (Ring){0};
    memset(g_net.mac, 0, sizeof(g_net.mac));
    g_net.sent = g_net.received = 0;
    g_net.twin = 0;
    pthread_mutex_unlock(&g_net.lock);
}
