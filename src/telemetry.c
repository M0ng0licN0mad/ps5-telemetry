#include "telemetry.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define FT_N 120

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static double   g_cpu = -1, g_gpu = -1;
static uint64_t g_ram_total = 0, g_ram_used = 0;
static double   ft_ring[FT_N];
static int      ft_head = 0, ft_count = 0;
static uint64_t last_flip_ns = 0;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int sysctl_u64(const char *name, uint64_t *out) {
    uint64_t v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) return -1;
    *out = (len == 4) ? (uint64_t)(uint32_t)v : v;
    return 0;
}

/* ---- CPU: delta of kern.cp_time ticks (user,nice,sys,intr,idle) ---- */
static double sample_cpu(void) {
    static long prev[5];
    static int have_prev = 0;
    long cur[5];
    size_t len = sizeof(cur);
    if (sysctlbyname("kern.cp_time", cur, &len, NULL, 0) != 0) return -1;
    double r = -1;
    if (have_prev) {
        long total = 0;
        for (int i = 0; i < 5; i++) total += cur[i] - prev[i];
        if (total > 0) r = 100.0 * (1.0 - (double)(cur[4] - prev[4]) / (double)total);
    }
    memcpy(prev, cur, sizeof(prev));
    have_prev = 1;
    return r;
}

/* ---- RAM: FreeBSD VM counters. Values on PS5 may not match the game's
 *      direct/flexible memory pools; see README. ---- */
static void sample_ram(uint64_t *total, uint64_t *used) {
    uint64_t phys = 0, freec = 0, inact = 0, pgsz = 0;
    if (sysctl_u64("hw.physmem", &phys) || sysctl_u64("vm.stats.vm.v_page_size", &pgsz) ||
        sysctl_u64("vm.stats.vm.v_free_count", &freec)) { *total = *used = 0; return; }
    sysctl_u64("vm.stats.vm.v_inactive_count", &inact);
    uint64_t avail = (freec + inact) * pgsz;
    *total = phys;
    *used = phys > avail ? phys - avail : 0;
}

/* ---- GPU: TODO. No public API on PS5. Return percent busy, or -1 if unknown.
 *      See README "Roadmap" for the approach. ---- */
static double sample_gpu(void) { return -1; }

static void *sampler(void *arg) {
    (void)arg;
    for (;;) {
        double c = sample_cpu(), g = sample_gpu();
        uint64_t t, u;
        sample_ram(&t, &u);
        pthread_mutex_lock(&mu);
        g_cpu = c; g_gpu = g; g_ram_total = t; g_ram_used = u;
        pthread_mutex_unlock(&mu);
        usleep(500000);
    }
    return NULL;
}

void tm_frame_flip(void) {
    uint64_t t = now_ns();
    pthread_mutex_lock(&mu);
    if (last_flip_ns) {
        double ms = (double)(t - last_flip_ns) / 1e6;
        if (ms < 1000.0) {
            ft_ring[ft_head] = ms;
            ft_head = (ft_head + 1) % FT_N;
            if (ft_count < FT_N) ft_count++;
        }
    }
    last_flip_ns = t;
    pthread_mutex_unlock(&mu);
}

static void *flip_listener(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(FLIP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) return NULL;
    char b[16];
    for (;;) if (recv(s, b, sizeof(b), 0) >= 0) tm_frame_flip();
    return NULL;
}

void tm_start(void) {
    pthread_t t;
    pthread_create(&t, NULL, sampler, NULL);
    pthread_create(&t, NULL, flip_listener, NULL);
}

static int put_num(char *p, size_t n, const char *key, double v, int last) {
    if (v < 0) return snprintf(p, n, "\"%s\":null%s", key, last ? "" : ",");
    return snprintf(p, n, "\"%s\":%.2f%s", key, v, last ? "" : ",");
}

size_t tm_snapshot_json(char *buf, size_t n) {
    pthread_mutex_lock(&mu);
    double cpu = g_cpu, gpu = g_gpu, last = -1, avg = -1;
    uint64_t tot = g_ram_total, used = g_ram_used;
    int fresh = ft_count > 0 && (now_ns() - last_flip_ns) < 2000000000ULL;
    if (fresh) {
        double sum = 0;
        for (int i = 0; i < ft_count; i++) sum += ft_ring[i];
        avg = sum / ft_count;
        last = ft_ring[(ft_head + FT_N - 1) % FT_N];
    }
    pthread_mutex_unlock(&mu);

    size_t o = 0;
    o += snprintf(buf + o, n - o, "{");
    o += put_num(buf + o, n - o, "cpu", cpu, 0);
    o += put_num(buf + o, n - o, "gpu", gpu, 0);
    o += put_num(buf + o, n - o, "ram_total_mb", tot ? tot / 1048576.0 : -1, 0);
    o += put_num(buf + o, n - o, "ram_used_mb", tot ? used / 1048576.0 : -1, 0);
    o += put_num(buf + o, n - o, "frametime_ms", last, 0);
    o += put_num(buf + o, n - o, "frametime_avg_ms", avg, 1);
    o += snprintf(buf + o, n - o, "}");
    return o;
}

/* ---- Diagnostics: read-only probe of system counters. Refused names show
 *      ok:false with the errno, so we learn what 13.60 allows before
 *      building anything on top of it. ---- */
static const char *PROBES[] = {
    "hw.physmem", "hw.usermem", "hw.realmem", "hw.pagesize", "hw.ncpu", "hw.model",
    "vm.stats.vm.v_page_size", "vm.stats.vm.v_page_count", "vm.stats.vm.v_free_count",
    "vm.stats.vm.v_inactive_count", "vm.stats.vm.v_active_count", "vm.stats.vm.v_wire_count",
    "vm.kmem_size", "kern.cp_time", "kern.cp_times", "kern.smp.cpus",
    "dev.cpu.0.freq", "kern.boottime", "kern.osrelease", NULL
};

size_t tm_diag_json(char *buf, size_t n) {
    size_t o = (size_t)snprintf(buf, n, "{\"sysctl\":{");
    for (int i = 0; PROBES[i] && o + 200 < n; i++) {
        unsigned char raw[256];
        size_t len = sizeof(raw);
        memset(raw, 0, sizeof(raw));
        int rc = sysctlbyname(PROBES[i], raw, &len, NULL, 0);
        int e = errno;
        const char *sep = PROBES[i + 1] ? "," : "";
        if (rc == 0) {
            char hex[33] = {0};
            size_t k = len < 16 ? len : 16;
            for (size_t j = 0; j < k; j++) snprintf(hex + 2 * j, 3, "%02x", raw[j]);
            o += (size_t)snprintf(buf + o, n - o, "\"%s\":{\"ok\":true,\"len\":%zu,\"hex\":\"%s\"}%s",
                                  PROBES[i], len, hex, sep);
        } else {
            o += (size_t)snprintf(buf + o, n - o, "\"%s\":{\"ok\":false,\"errno\":%d}%s",
                                  PROBES[i], e, sep);
        }
    }
    o += (size_t)snprintf(buf + o, n - o, "}}");
    return o;
}
