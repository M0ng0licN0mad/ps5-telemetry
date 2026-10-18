#include "telemetry.h"
#include "dashboard.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef struct { char unknown[45]; char message[3075]; } notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *msg) {
    notify_request_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.message, sizeof(r.message), "%s", msg);
    sceKernelSendNotificationRequest(0, &r, sizeof(r), 0);
}

static void send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n > 0) {
        ssize_t w = write(fd, c, n);
        if (w <= 0) return;
        c += w; n -= (size_t)w;
    }
}

static void reply(int fd, int code, const char *type, const void *body, size_t len) {
    char h[256];
    int n = snprintf(h, sizeof(h),
        "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n", code, type, len);
    send_all(fd, h, (size_t)n);
    send_all(fd, body, len);
}

static void emit_line(void *ctx, const char *s) {
    int fd = *(int *)ctx;
    send_all(fd, s, strlen(s));
    send_all(fd, "\n", 1);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    tm_start();

    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(HTTP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 8) != 0) {
        notify("Telemetry: cannot bind port 9090 (already running?)");
        return 1;
    }
    notify("Telemetry running. Open http://<PS5-IP>:9090 in a browser");

    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        struct timeval tv = {2, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char req[1024];
        ssize_t n = read(c, req, sizeof(req) - 1);
        if (n > 0) {
            req[n] = 0;
            if (!strncmp(req, "GET /stats", 10)) {
                char body[512];
                size_t len = tm_snapshot_json(body, sizeof(body));
                reply(c, 200, "application/json", body, len);
            } else if (!strncmp(req, "GET /procs", 10)) {
                char pbuf[4096];
                size_t len = tm_procs_json(pbuf, sizeof(pbuf));
                reply(c, 200, "application/json", pbuf, len);
            } else if (!strncmp(req, "GET /sysctls", 12)) {
                char q[48] = "";
                const char *eol = strpbrk(req, "\r\n");
                const char *p = strstr(req, "q=");
                if (p && (!eol || p < eol)) {
                    size_t i = 0;
                    p += 2;
                    while (*p && *p != ' ' && *p != '&' && *p != '\r' && i < sizeof(q) - 1) q[i++] = *p++;
                    q[i] = 0;
                }
                const char *h = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n"
                                "Access-Control-Allow-Origin: *\r\n\r\n";
                send_all(c, h, strlen(h));
                tm_sysctl_walk(q, emit_line, &c);
            } else if (!strncmp(req, "GET /diag", 9)) {
                char dbuf[4096];
                size_t len = tm_diag_json(dbuf, sizeof(dbuf));
                reply(c, 200, "application/json", dbuf, len);
            } else if (!strncmp(req, "GET / ", 6) || !strncmp(req, "GET /?", 6)) {
                reply(c, 200, "text/html; charset=utf-8", DASHBOARD_HTML, DASHBOARD_LEN);
            } else {
                reply(c, 404, "text/plain", "not found", 9);
            }
        }
        close(c);
    }
}
