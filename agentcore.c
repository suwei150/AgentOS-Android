#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <curl/curl.h>

#define AGENTCORE_VERSION "2.0.0"
#define MAX_ARGS 16
#define BUFFER_SIZE 4096
#define WATCH_DEBOUNCE_MS 100

/* 全局状态 */
static volatile int running = 1;
static FILE *log_file = NULL;

/* 信号处理 */
static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/* 日志系统 */
static void log_msg(const char *level, const char *fmt, ...) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t sec = ts.tv_sec;
    struct tm tm;
    localtime_r(&sec, &tm);
    
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm);
    
    va_list args;
    va_start(args, fmt);
    
    if (log_file) {
        fprintf(log_file, "[%s] [%ld ms] [%s] ", timestamp, ts.tv_nsec / 1000000, level);
        vfprintf(log_file, fmt, args);
        fprintf(log_file, "\n");
        fflush(log_file);
    }
    
    printf("[%s] [%s] ", timestamp, level);
    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);
    
    va_end(args);
}

/* 文件操作 */
static int cmd_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        log_msg("ERROR", "无法打开文件: %s (%s)", path, strerror(errno));
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        log_msg("ERROR", "内存分配失败");
        return -1;
    }
    
    size_t n = fread(buf, 1, size, f);
    buf[n] = '\0';
    fclose(f);
    
    log_msg("INFO", "读取文件 %s: %ld 字节", path, n);
    printf("--- CONTENT START ---\n%s\n--- CONTENT END ---\n", buf);
    
    free(buf);
    return 0;
}

static int cmd_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        log_msg("ERROR", "无法写入文件: %s (%s)", path, strerror(errno));
        return -1;
    }
    
    size_t len = strlen(content);
    size_t n = fwrite(content, 1, len, f);
    fclose(f);
    
    log_msg("INFO", "写入文件 %s: %zu 字节", path, n);
    return 0;
}

static int cmd_delete_file(const char *path) {
    int ret = unlink(path);
    if (ret != 0) {
        log_msg("ERROR", "删除文件失败: %s (%s)", path, strerror(errno));
        return -1;
    }
    log_msg("INFO", "删除文件: %s", path);
    return 0;
}

/* 命令执行 */
static int cmd_exec(const char *cmdline) {
    log_msg("INFO", "执行命令: %s", cmdline);
    
    int ret = system(cmdline);
    if (ret == -1) {
        log_msg("ERROR", "命令执行失败");
        return -1;
    }
    
    if (WIFEXITED(ret)) {
        int status = WEXITSTATUS(ret);
        log_msg("INFO", "命令完成，退出码: %d", status);
        return status;
    }
    
    return -1;
}

/* HTTP 请求 */
struct curl_buffer {
    char *data;
    size_t size;
};

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct curl_buffer *buf = (struct curl_buffer *)userdata;
    size_t realsize = size * nmemb;
    
    char *tmp = realloc(buf->data, buf->size + realsize + 1);
    if (!tmp) return 0;
    
    buf->data = tmp;
    memcpy(&buf->data[buf->size], ptr, realsize);
    buf->size += realsize;
    buf->data[buf->size] = '\0';
    
    return realsize;
}

static int cmd_http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        log_msg("ERROR", "cURL 初始化失败");
        return -1;
    }
    
    struct curl_buffer buffer = {0};
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    
    log_msg("INFO", "GET %s", url);
    
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        log_msg("ERROR", "HTTP 请求失败: %s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return -1;
    }
    
    log_msg("INFO", "响应大小: %zu 字节", buffer.size);
    printf("--- HTTP RESPONSE START ---\n%s\n--- HTTP RESPONSE END ---\n", buffer.data);
    
    free(buffer.data);
    curl_easy_cleanup(curl);
    return 0;
}

/* 文件监控（使用 inotify） */
#ifdef __linux__
#include <sys/inotify.h>

static int cmd_watch(const char *path, int duration_sec) {
    int wd = inotify_init();
    if (wd < 0) {
        log_msg("ERROR", "inotify_init 失败: %s", strerror(errno));
        return -1;
    }
    
    int mask = IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
    int wd_event = inotify_add_watch(wd, path, mask);
    if (wd_event < 0) {
        log_msg("ERROR", "inotify_add_watch 失败: %s (%s)", path, strerror(errno));
        close(wd);
        return -1;
    }
    
    log_msg("INFO", "开始监控: %s (时长: %d 秒)", path, duration_sec);
    
    time_t start = time(NULL);
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    
    while (running && (time(NULL) - start) < duration_sec) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(wd, &rfds);
        
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(wd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        if (ret == 0) continue;
        
        int len = read(wd, buf, sizeof(buf));
        if (len <= 0) break;
        
        char *ptr = buf;
        while (ptr < buf + len) {
            struct inotify_event *event = (struct inotify_event *)ptr;
            
            const char *event_name = "UNKNOWN";
            if (event->mask & IN_CREATE) event_name = "CREATE";
            else if (event->mask & IN_MODIFY) event_name = "MODIFY";
            else if (event->mask & IN_DELETE) event_name = "DELETE";
            else if (event->mask & IN_MOVED_FROM) event_name = "MOVED_FROM";
            else if (event->mask & IN_MOVED_TO) event_name = "MOVED_TO";
            
            log_msg("WATCH", "%s: %s", event_name, event->name ? event->name : "(root)");
            
            ptr += sizeof(struct inotify_event) + event->len;
        }
    }
    
    inotify_rm_watch(wd, wd_event);
    close(wd);
    log_msg("INFO", "监控结束");
    return 0;
}
#else
static int cmd_watch(const char *path, int duration_sec) {
    (void)path;
    (void)duration_sec;
    log_msg("WARN", "文件监控仅支持 Linux");
    return -1;
}
#endif

/* 显示用法 */
static void usage(const char *prog) {
    printf("AgentCore v%s - 轻量级 AI Agent 运行时\n", AGENTCORE_VERSION);
    printf("用法: %s <command> [args...]\n\n", prog);
    printf("命令:\n");
    printf("  read <file>        读取文件内容\n");
    printf("  write <file> <text>  写入文件\n");
    printf("  delete <file>      删除文件\n");
    printf("  exec <command>     执行系统命令\n");
    printf("  get <url>          HTTP GET 请求\n");
    printf("  watch <path> [sec]  监控文件变化\n");
    printf("  log <file>         设置日志文件\n");
    printf("  help               显示帮助\n");
    printf("  exit               退出\n");
}

/* 主函数 */
int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    log_msg("INFO", "AgentCore v%s 启动", AGENTCORE_VERSION);
    log_msg("INFO", "PID: %d", getpid());
    
    if (argc < 2) {
        usage(argv[0]);
        return 0;
    }
    
    const char *cmd = argv[1];
    
    if (strcmp(cmd, "help") == 0) {
        usage(argv[0]);
        return 0;
    }
    
    if (strcmp(cmd, "exit") == 0) {
        log_msg("INFO", "退出");
        return 0;
    }
    
    if (strcmp(cmd, "log") == 0) {
        if (argc < 3) {
            log_msg("ERROR", "用法: log <file>");
            return 1;
        }
        log_file = fopen(argv[2], "a");
        if (!log_file) {
            log_msg("ERROR", "无法打开日志文件: %s", argv[2]);
            return 1;
        }
        log_msg("INFO", "日志文件: %s", argv[2]);
        return 0;
    }
    
    if (strcmp(cmd, "read") == 0) {
        if (argc < 3) {
            log_msg("ERROR", "用法: read <file>");
            return 1;
        }
        return cmd_read_file(argv[2]);
    }
    
    if (strcmp(cmd, "write") == 0) {
        if (argc < 4) {
            log_msg("ERROR", "用法: write <file> <text>");
            return 1;
        }
        return cmd_write_file(argv[2], argv[3]);
    }
    
    if (strcmp(cmd, "delete") == 0) {
        if (argc < 3) {
            log_msg("ERROR", "用法: delete <file>");
            return 1;
        }
        return cmd_delete_file(argv[2]);
    }
    
    if (strcmp(cmd, "exec") == 0) {
        if (argc < 3) {
            log_msg("ERROR", "用法: exec <command>");
            return 1;
        }
        char cmdline[BUFFER_SIZE] = {0};
        for (int i = 2; i < argc; i++) {
            strcat(cmdline, argv[i]);
            if (i < argc - 1) strcat(cmdline, " ");
        }
        return cmd_exec(cmdline);
    }
    
    if (strcmp(cmd, "get") == 0) {
        if (argc < 3) {
            log_msg("ERROR", "用法: get <url>");
            return 1;
        }
        return cmd_http_get(argv[2]);
    }
    
    if (strcmp(cmd, "watch") == 0) {
        if (argc < 3) {
            log_msg("ERROR", "用法: watch <path> [seconds]");
            return 1;
        }
        int duration = (argc >= 4) ? atoi(argv[3]) : 60;
        return cmd_watch(argv[2], duration);
    }
    
    log_msg("ERROR", "未知命令: %s", cmd);
    usage(argv[0]);
    return 1;
}
