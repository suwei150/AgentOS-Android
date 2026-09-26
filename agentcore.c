#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/inotify.h>

#define VERSION "2.0.0"
#define BUFSIZE 8192

static volatile int running = 1;
static FILE *log_fp = NULL;

static void sig_handler(int s) { (void)s; running = 0; }

static void logp(const char *lvl, const char *fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(ts, 32, "%H:%M:%S", tm);
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "[%s][%s] ", ts, lvl);
    vfprintf(stdout, fmt, ap);
    putchar('\n');
    va_end(ap);
    if (log_fp) {
        fprintf(log_fp, "[%s][%s] ", ts, lvl);
        va_start(ap, fmt);
        vfprintf(log_fp, fmt, ap);
        fprintf(log_fp, "\n");
        va_end(ap);
    }
}

static int do_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { logp("ERR", "fopen %s: %s", path, strerror(errno)); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1); if (!buf) { fclose(f); return 1; }
    if (fread(buf, 1, sz, f) < (size_t)sz) { fclose(f); return 1; }
    buf[sz] = 0; fclose(f);
    printf("--- BEGIN %s (%ld bytes) ---\n%s\n--- END ---\n", path, sz, buf);
    free(buf); return 0;
}

static int do_write(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { logp("ERR", "fopen %s: %s", path, strerror(errno)); return 1; }
    fwrite(content, 1, strlen(content), f); fclose(f);
    logp("OK", "wrote %s", path); return 0;
}

static int do_delete(const char *path) {
    if (unlink(path)) { logp("ERR", "unlink %s: %s", path, strerror(errno)); return 1; }
    logp("OK", "deleted %s", path); return 0;
}

static int do_exec(const char *cmd) {
    logp("RUN", "%s", cmd);
    return system(cmd);
}

/* 纯socket HTTP GET，零依赖 */
static int do_get(const char *url) {
    const char *host_start, *path_start;
    char host[256], path[1024];
    
    if (strncmp(url, "https://", 8) == 0) { logp("ERR", "https not supported, use http"); return 1; }
    if (strncmp(url, "http://", 7) != 0) { logp("ERR", "invalid URL"); return 1; }
    
    host_start = url + 7;
    path_start = strstr(host_start, "/");
    if (!path_start) { strcpy(path, "/"); } else { strncpy(path, path_start, sizeof(path)-1); path[sizeof(path)-1]=0; }
    
    size_t hl = path_start ? (size_t)(path_start - host_start) : strlen(host_start);
    if (hl >= sizeof(host)) { logp("ERR", "host too long"); return 1; }
    strncpy(host, host_start, hl); host[hl] = 0;
    
    logp("GET", "%s%s", host, path);
    
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "80", &hints, &res) != 0) { logp("ERR", "DNS fail"); return 1; }
    
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { logp("ERR", "socket: %s", strerror(errno)); return 1; }
    
    struct timeval tv; tv.tv_sec = 10; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        logp("ERR", "connect: %s", strerror(errno)); close(fd); freeaddrinfo(res); return 1;
    }
    freeaddrinfo(res);
    
    char req[2048];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: AgentCore/%s\r\nConnection: close\r\n\r\n",
        path, host, VERSION);
    send(fd, req, strlen(req), 0);
    
    /* 读响应，跳过header */
    char buf[BUFSIZE]; char *all = NULL; size_t allsz = 0, hdr_end = 0;
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        if (!allsz) {
            all = realloc(all, n + 4096); allsz = n; all[n] = 0;
            hdr_end = (size_t)(strstr(all, "\r\n\r\n") - all) + 4;
        } else {
            all = realloc(all, allsz + n + 1);
            memcpy(all + allsz, buf, n); allsz += n; all[allsz] = 0;
        }
    }
    close(fd);
    
    if (!allsz || hdr_end >= allsz) { logp("ERR", "empty response"); free(all); return 1; }
    
    size_t body_len = allsz - hdr_end;
    printf("--- HTTP %zu bytes ---\n%s\n--- END ---\n", body_len, all + hdr_end);
    free(all); return 0;
}

static int do_watch(const char *path, int dur) {
    int wd = inotify_init();
    if (wd < 0) { logp("ERR", "inotify: %s", strerror(errno)); return 1; }
    int mask = IN_CREATE|IN_MODIFY|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO;
    if (inotify_add_watch(wd, path, mask) < 0) { logp("ERR", "watch %s: %s", path, strerror(errno)); close(wd); return 1; }
    
    logp("W", "watching %s for %d sec", path, dur);
    time_t start = time(NULL);
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    
    while (running && (time(NULL) - start) < dur) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(wd, &rfds);
        struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
        if (select(wd+1, &rfds, NULL, NULL, &tv) <= 0) continue;
        int len = read(wd, buf, sizeof(buf));
        char *p = buf;
        while (p < buf + len) {
            struct inotify_event *ev = (struct inotify_event *)p;
            const char *name = "?";
            if (ev->mask & IN_CREATE) name = "CREATE";
            else if (ev->mask & IN_MODIFY) name = "MODIFY";
            else if (ev->mask & IN_DELETE) name = "DELETE";
            logp("EVt", "%s %s", name, ev->len > 0 ? ev->name : "(dir)");
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    close(wd); return 0;
}

static void usage(void) {
    printf("AgentCore v%s\nCommands:\n", VERSION);
    printf("  read <file>      read file\n  write <file> <text>  write file\n");
    printf("  del <file>       delete file\n  exec <cmd>     run command\n");
    printf("  get <url>        HTTP GET\n  watch <path> [sec]  watch dir\n");
    printf("  log <file>       set log file\n  help           this help\n");
}

int main(int argc, char **argv) {
    signal(SIGINT, sig_handler); signal(SIGTERM, sig_handler);
    if (argc < 2) { usage(); return 0; }
    
    const char *c = argv[1];
    if (!strcmp(c, "help")) { usage(); return 0; }
    if (!strcmp(c, "log") && argc > 2) { log_fp = fopen(argv[2], "a"); return log_fp ? 0 : 1; }
    if (!strcmp(c, "read") && argc > 2) return do_read(argv[2]);
    if (!strcmp(c, "write") && argc > 3) return do_write(argv[2], argv[3]);
    if (!strcmp(c, "del") && argc > 2) return do_delete(argv[2]);
    if (!strcmp(c, "exec") && argc > 2) {
        char cmd[4096] = {0};
        for (int i = 2; i < argc; i++) { strcat(cmd, argv[i]); if (i < argc-1) strcat(cmd, " "); }
        return do_exec(cmd);
    }
    if (!strcmp(c, "get") && argc > 2) return do_get(argv[2]);
    if (!strcmp(c, "watch") && argc > 2) return do_watch(argv[2], argc > 3 ? atoi(argv[3]) : 60);
    
    logp("ERR", "unknown: %s", c); usage(); return 1;
}
