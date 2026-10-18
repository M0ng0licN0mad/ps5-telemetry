#include "telemetry.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <stddef.h>
#if defined(__has_include)
# if __has_include(<sys/user.h>)
#  include <sys/param.h>
#  include <sys/user.h>
#  define HAVE_KINFO 1
# endif
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define FT_N 120

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static double   g_cpu = -1, g_gpu = -1;
static double   ft_ring[FT_N];
static int      ft_head = 0, ft_count = 0;
static uint64_t last_flip_ns = 0;
static double   g_ram_total_mb = -1, g_ram_used_mb = -1;
static int      g_ncpu = 0, g_pagesz = 0;
static int      g_proc_pid = 0;
static char     g_proc_name[24] = "";

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

#ifdef HAVE_KINFO
#define MAXP 1024
typedef struct { int pid; char comm[24]; uint64_t runtime; int64_t rss; } pinfo_t;
static pthread_mutex_t pmu = PTHREAD_MUTEX_INITIALIZER;
static pinfo_t procs[MAXP];
static size_t nprocs = 0, raw_bytes = 0;
static unsigned char *rawbuf = NULL;
static size_t rawcap = 0;
static int last_stage = 0, last_errno = 0, first_len = 0;

/* Caller holds pmu. PS5 13.60 returns 1096-byte process records while the SDK
 * struct is 1088 bytes, so walk the table by each record's own declared length
 * and copy the common prefix (same approach TacHen uses). */
static int read_procs(void) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t need = 0, len;
    nprocs = 0; raw_bytes = 0; first_len = 0;
    if (sysctl(mib, 4, NULL, &need, NULL, 0) != 0) { last_stage = 1; last_errno = errno; return -1; }
    if (need < sizeof(int) || need > 16u * 1024 * 1024) { last_stage = 2; last_errno = EINVAL; return -1; }
    size_t cap = need + need / 4;
    if (cap > rawcap) {
        free(rawbuf);
        rawbuf = malloc(cap);
        rawcap = rawbuf ? cap : 0;
        if (!rawbuf) { last_stage = 3; last_errno = ENOMEM; return -1; }
    }
    len = rawcap;
    if (sysctl(mib, 4, rawbuf, &len, NULL, 0) != 0) { last_stage = 4; last_errno = errno; return -1; }
    raw_bytes = len;
    const size_t min_len = offsetof(struct kinfo_proc, ki_comm) + COMMLEN + 1;
    size_t off = 0;
    while (off + sizeof(int) <= len && nprocs < MAXP) {
        int reclen;
        memcpy(&reclen, rawbuf + off, sizeof(reclen));
        if (!first_len) first_len = reclen;
        if (reclen < (int)min_len || (size_t)reclen > len - off) {
            last_stage = 5; last_errno = EINVAL;
            return nprocs ? 0 : -1;
        }
        struct kinfo_proc info;
        memset(&info, 0, sizeof(info));
        memcpy(&info, rawbuf + off, (size_t)reclen < sizeof(info) ? (size_t)reclen : sizeof(info));
        if (info.ki_pid > 0) {
            pinfo_t *p = &procs[nprocs++];
            p->pid = info.ki_pid;
            p->runtime = (uint64_t)info.ki_runtime;
            p->rss = (int64_t)info.ki_rssize;
            size_t j = 0;
            for (; j < sizeof(p->comm) - 1 && j < COMMLEN && info.ki_comm[j]; j++) {
                char ch = info.ki_comm[j];
                p->comm[j] = (ch >= 32 && ch < 127 && ch != '"' && ch != '\\') ? ch : '?';
            }
            p->comm[j] = 0;
        }
        off += (size_t)reclen;
    }
    last_stage = 0; last_errno = 0;
    return 0;
}

/* The "game" is the process with the largest resident memory (kept sticky so it
 * does not flip). CPU % = its run-time delta / (wall time * logical CPUs), the same
 * definition TacHen uses. RAM = its resident pages. */
static int game_pid = 0, have_prev = 0;
static uint64_t prev_us = 0, prev_rt = 0;

static void sample_procs(double *cpu, double *rss_mb, int *pid, char *name) {
    *cpu = *rss_mb = -1;
    pthread_mutex_lock(&pmu);
    if (read_procs() == 0 && nprocs) {
        int best = 0, cur = -1;
        for (size_t i = 0; i < nprocs; i++) {
            if (procs[i].rss > procs[best].rss) best = (int)i;
            if (procs[i].pid == game_pid) cur = (int)i;
        }
        int pick = (cur >= 0 && procs[cur].rss * 2 >= procs[best].rss) ? cur : best;
        uint64_t t = now_ns() / 1000;
        if (procs[pick].pid != game_pid) { game_pid = procs[pick].pid; have_prev = 0; }
        if (have_prev && t > prev_us && procs[pick].runtime >= prev_rt && g_ncpu > 0) {
            double c = 100.0 * ((double)(procs[pick].runtime - prev_rt) / (double)(t - prev_us)) / g_ncpu;
            *cpu = c > 100 ? 100 : c;
        }
        prev_rt = procs[pick].runtime; prev_us = t; have_prev = 1;
        if (g_pagesz > 0) *rss_mb = (double)procs[pick].rss * g_pagesz / 1048576.0;
        *pid = procs[pick].pid;
        snprintf(name, 24, "%s", procs[pick].comm);
    }
    pthread_mutex_unlock(&pmu);
}

static size_t emit_top(char *buf, size_t o, size_t n, int by_rss, int k) {
    static unsigned char used[MAXP];
    memset(used, 0, sizeof(used));
    for (int r = 0; r < k && o + 200 < n; r++) {
        int best = -1;
        double bv = -1;
        for (size_t i = 0; i < nprocs; i++) {
            if (used[i]) continue;
            double v = by_rss ? (double)procs[i].rss : (double)procs[i].runtime;
            if (v > bv) { bv = v; best = (int)i; }
        }
        if (best < 0) break;
        used[best] = 1;
        o += (size_t)snprintf(buf + o, n - o,
            "%s{\"pid\":%d,\"comm\":\"%s\",\"runtime_us\":%llu,\"rss_pages\":%lld}",
            r ? "," : "", procs[best].pid, procs[best].comm,
            (unsigned long long)procs[best].runtime, (long long)procs[best].rss);
    }
    return o;
}
#endif

size_t tm_procs_json(char *buf, size_t n) {
#ifdef HAVE_KINFO
    pthread_mutex_lock(&pmu);
    int rc = read_procs();
    size_t o = (size_t)snprintf(buf, n,
        "{\"ok\":%s,\"stage\":%d,\"errno\":%d,\"bytes\":%zu,\"first_record_len\":%d,"
        "\"sdk_struct_len\":%zu,\"count\":%zu,\"ncpu\":%d,\"pagesize\":%d,\"top_rss\":[",
        rc == 0 ? "true" : "false", last_stage, last_errno, raw_bytes, first_len,
        sizeof(struct kinfo_proc), nprocs, g_ncpu, g_pagesz);
    o = emit_top(buf, o, n, 1, 6);
    o += (size_t)snprintf(buf + o, n - o, "],\"top_runtime\":[");
    o = emit_top(buf, o, n, 0, 8);
    o += (size_t)snprintf(buf + o, n - o, "]}");
    pthread_mutex_unlock(&pmu);
    return o;
#else
    return (size_t)snprintf(buf, n, "{\"ok\":false,\"reason\":\"no kinfo_proc in this SDK\"}");
#endif
}

/* Walk the kernel's sysctl name table. Names only: no values are read. */
void tm_sysctl_walk(const char *filter, void (*emit)(void *, const char *), void *ctx) {
    int nm[CTL_MAXNAME + 2], out[CTL_MAXNAME];
    size_t l1 = 3;
    nm[0] = 0; nm[1] = 2; nm[2] = 1;
    for (int guard = 0; guard < 20000; guard++) {
        size_t l2 = sizeof(out);
        if (sysctl(nm, (unsigned)l1, out, &l2, NULL, 0) != 0) break;
        l2 /= sizeof(int);
        if (l2 == 0 || l2 > CTL_MAXNAME) break;
        int nn[CTL_MAXNAME + 2];
        nn[0] = 0; nn[1] = 1;
        memcpy(nn + 2, out, l2 * sizeof(int));
        char name[256];
        size_t nl = sizeof(name);
        if (sysctl(nn, (unsigned)(l2 + 2), name, &nl, NULL, 0) == 0 &&
            (filter[0] == 0 || strstr(name, filter)))
            emit(ctx, name);
        memcpy(nm + 2, out, l2 * sizeof(int));
        l1 = 2 + l2;
    }
}

static void *sampler(void *arg) {
    (void)arg;
    uint64_t v;
    if (sysctl_u64("hw.ncpu", &v) == 0) g_ncpu = (int)v;
    if (sysctl_u64("hw.pagesize", &v) == 0) g_pagesz = (int)v;
    for (;;) {
        double c = -1, rss = -1, g = sample_gpu();
        int ppid = 0;
        char pname[24] = "";
#ifdef HAVE_KINFO
        sample_procs(&c, &rss, &ppid, pname);
#endif
        if (c < 0) c = sample_cpu();          /* kern.cp_time: absent on 13.60, kept as fallback */
        uint64_t t, u;
        sample_ram(&t, &u);                   /* vm.* counters: absent on 13.60, kept as fallback */
        double rt = t ? t / 1048576.0 : -1, ru = t ? u / 1048576.0 : (rss >= 0 ? rss : -1);
        pthread_mutex_lock(&mu);
        g_cpu = c; g_gpu = g; g_ram_total_mb = rt; g_ram_used_mb = ru;
        g_proc_pid = ppid; memcpy(g_proc_name, pname, sizeof(g_proc_name));
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
    int ppid = g_proc_pid;
    char pname[24];
    memcpy(pname, g_proc_name, sizeof(pname));
    double rtot = g_ram_total_mb, rused = g_ram_used_mb;
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
    o += put_num(buf + o, n - o, "ram_total_mb", rtot, 0);
    o += put_num(buf + o, n - o, "ram_used_mb", rused, 0);
    o += put_num(buf + o, n - o, "frametime_ms", last, 0);
    o += put_num(buf + o, n - o, "frametime_avg_ms", avg, 0);
    o += (size_t)snprintf(buf + o, n - o, "\"proc_pid\":%d,\"proc_name\":\"%s\"", ppid, pname);
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
