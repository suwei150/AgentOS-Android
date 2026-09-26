/*
 * AgentCore v0.3 — Android 原生 C 版执行引擎
 * 
 * 特点:
 * - 静态编译成 Android arm64 ELF (~50KB)
 * - Unix socket 通信 (非 HTTP)
 * - 零外部依赖 (仅 libc)
 * - 隐藏进程名
 * 
 * 编译:
 *   aarch64-linux-android-gcc -static -O2 -o agentcore agentcore.c
 * 
 * 运行:
 *   ./agentcore &
 * 
 * 通信:
 *   echo '{"type":"shell","cmd":"ls"}' | socat - UNIX-CONNECT:/data/local/tmp/agent.sock
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <libgen.h>
#include <sys/prctl.h>

#define SOCKET_PATH "/data/local/tmp/agent.sock"
#define PID_FILE "/data/local/tmp/agent.pid"
#define MAX_MSG 8192
#define MAX_CMD 4096
#define MAX_RESULT 16384

/* ===== 工具函数 ===== */

void log_msg(const char *msg) {
    FILE *f = fopen("/data/local/tmp/agent.log", "a");
    if (f) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        fprintf(f, "[%02d:%02d:%02d] %s\n", 
                tm->tm_hour, tm->tm_min, tm->tm_sec, msg);
        fclose(f);
    }
}

void write_pid() {
    FILE *f = fopen(PID_FILE, "w");
    if (f) {
        fprintf(f, "%d", getpid());
        fclose(f);
    }
}

/* ===== JSON 简易解析 ===== */

// 从 JSON 中提取字符串值: {"key":"value"} -> value
char *json_get_str(const char *json, const char *key) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return NULL;
    start += strlen(pattern);
    
    char *buf = malloc(4096);
    if (!buf) return NULL;
    
    int i = 0;
    while (*start && *start != "\"" && i < 4095) {
        if (*start == '\\' && *(start+1)) {
            buf[i++] = *(start+1);
            start += 2;
        } else {
            buf[i++] = *start++;
        }
    }
    buf[i] = '\0';
    return buf;
}

// 从 JSON 中提取整数值
int json_get_int(const char *json, const char *key, int default_val) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *start = strstr(json, pattern);
    if (!start) return default_val;
    start += strlen(pattern);
    return atoi(start);
}

/* ===== 原语实现 ===== */

void exec_shell(char *cmd, char *result, int *ok, int *status) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        *ok = 0;
        *status = -1;
        snprintf(result, MAX_RESULT, "popen failed: %s", strerror(errno));
        return;
    }
    
    int n = fread(result, 1, MAX_RESULT - 1, fp);
    result[n] = '\0';
    int ret = pclose(fp);
    *ok = (ret == 0);
    *status = ret;
}

void exec_file_read(char *path, char *result, int *ok, int *status) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        *ok = 0;
        *status = -1;
        snprintf(result, MAX_RESULT, "open failed: %s", strerror(errno));
        return;
    }
    
    int n = fread(result, 1, MAX_RESULT - 1, f);
    result[n] = '\0';
    fclose(f);
    *ok = 1;
    *status = 0;
}

void exec_file_write(char *path, char *content, char *result, int *ok, int *status) {
    FILE *f = fopen(path, "w");
    if (!f) {
        *ok = 0;
        *status = -1;
        snprintf(result, MAX_RESULT, "open failed: %s", strerror(errno));
        return;
    }
    
    int n = strlen(content);
    fwrite(content, 1, n, f);
    fclose(f);
    
    snprintf(result, MAX_RESULT, "written %d bytes", n);
    *ok = 1;
    *status = 0;
}

void exec_http_get(char *url, char *result, int *ok, int *status) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -sS --max-time 30 '%s' 2>&1", url);
    exec_shell(cmd, result, ok, status);
}

void exec_process_list(char *pattern, char *result, int *ok, int *status) {
    char cmd[512];
    if (pattern && *pattern) {
        snprintf(cmd, sizeof(cmd), "ps -A | grep '%s'", pattern);
    } else {
        snprintf(cmd, sizeof(cmd), "ps -A");
    }
    exec_shell(cmd, result, ok, status);
}

void exec_process_kill(char *pid_str, int force, char *result, int *ok, int *status) {
    int pid = atoi(pid_str);
    if (pid <= 0) {
        *ok = 0;
        *status = -1;
        snprintf(result, MAX_RESULT, "invalid pid: %s", pid_str);
        return;
    }
    
    int sig = force ? 9 : 15;
    int ret = kill(pid, sig);
    if (ret == 0) {
        snprintf(result, MAX_RESULT, "killed pid=%d sig=%d", pid, sig);
        *ok = 1;
        *status = 0;
    } else {
        snprintf(result, MAX_RESULT, "kill failed: %s", strerror(errno));
        *ok = 0;
        *status = -1;
    }
}

void exec_ui(char *action, char *arg, char *result, int *ok, int *status) {
    char cmd[512];
    if (strcmp(action, "tap") == 0) {
        snprintf(cmd, sizeof(cmd), "input tap %s", arg);
    } else if (strcmp(action, "swipe") == 0) {
        snprintf(cmd, sizeof(cmd), "input swipe %s", arg);
    } else if (strcmp(action, "text") == 0) {
        snprintf(cmd, sizeof(cmd), "input text %s", arg);
    } else if (strcmp(action, "key") == 0) {
        snprintf(cmd, sizeof(cmd), "input keyevent %s", arg);
    } else {
        snprintf(result, MAX_RESULT, "unknown action: %s", action);
        *ok = 0;
        *status = -1;
        return;
    }
    exec_shell(cmd, result, ok, status);
}

void exec_screenshot(char *path, char *result, int *ok, int *status) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "screencap -p %s 2>&1", path);
    exec_shell(cmd, result, ok, status);
}

void exec_device_info(char *result, int *ok, int *status) {
    char buf[MAX_RESULT];
    int offset = 0;
    
    // 获取设备信息
    char cmd[256];
    
    // model
    snprintf(cmd, sizeof(cmd), "getprop ro.product.model 2>/dev/null");
    exec_shell(cmd, buf, ok, status);
    offset += snprintf(result + offset, MAX_RESULT - offset, "model=%s\n", buf);
    
    // brand
    snprintf(cmd, sizeof(cmd), "getprop ro.product.brand 2>/dev/null");
    exec_shell(cmd, buf, ok, status);
    offset += snprintf(result + offset, MAX_RESULT - offset, "brand=%s\n", buf);
    
    // android version
    snprintf(cmd, sizeof(cmd), "getprop ro.build.version.release 2>/dev/null");
    exec_shell(cmd, buf, ok, status);
    offset += snprintf(result + offset, MAX_RESULT - offset, "android=%s\n", buf);
    
    // kernel
    snprintf(cmd, sizeof(cmd), "uname -r 2>/dev/null");
    exec_shell(cmd, buf, ok, status);
    offset += snprintf(result + offset, MAX_RESULT - offset, "kernel=%s\n", buf);
    
    // uid
    snprintf(result + offset, MAX_RESULT - offset, "uid=%d\n", getuid());
    
    *ok = 1;
    *status = 0;
}

/* ===== 请求分发 ===== */

void dispatch(char *req, char *resp) {
    char *type = json_get_str(req, "type");
    if (!type) {
        snprintf(resp, MAX_MSG, "{\"ok\":false,\"error\":\"missing type\"}");
        return;
    }
    
    char result[MAX_RESULT];
    int ok, status;
    
    if (strcmp(type, "ping") == 0) {
        snprintf(resp, MAX_MSG, "{\"ok\":true,\"result\":\"pong\"}");
    }
    else if (strcmp(type, "shell") == 0) {
        char *cmd = json_get_str(req, "cmd");
        if (!cmd) {
            snprintf(resp, MAX_MSG, "{\"ok\":false,\"error\":\"missing cmd\"}");
        } else {
            exec_shell(cmd, result, &ok, &status);
            snprintf(resp, MAX_MSG, "{\"ok\":%s,\"stdout\":\"%s\",\"status\":%d}",
                    ok ? "true" : "false", result, status);
        }
        free(cmd);
    }
    else if (strcmp(type, "file_read") == 0) {
        char *path = json_get_str(req, "path");
        if (!path) {
            snprintf(resp, MAX_MSG, "{\"ok\":false,\"error\":\"missing path\"}");
        } else {
            exec_file_read(path, result, &ok, &status);
            snprintf(resp, MAX_MSG, "{\"ok\":%s,\"content\":\"%s\"}",
                    ok ? "true" : "false", result);
        }
        free(path);
    }
    else if (strcmp(type, "file_write") == 0) {
        char *path = json_get_str(req, "path");
        char *content = json_get_str(req, "content");
        if (!path || !content) {
            snprintf(resp, MAX_MSG, "{\"ok\":false,\"error\":\"missing path or content\"}");
        } else {
            exec_file_write(path, content, result, &ok, &status);
            snprintf(resp, MAX_MSG, "{\"ok\":%s,\"result\":\"%s\"}",
                    ok ? "true" : "false", result);
        }
        free(path);
        free(content);
    }
    else if (strcmp(type, "http_get") == 0) {
        char *url = json_get_str(req, "url");
        if (!url) {
            snprintf(resp, MAX_MSG, "{\"ok\":false,\"error\":\"missing url\"}");
        } else {
            exec_http_get(url, result, &ok, &status);
            snprintf(resp, MAX_MSG, "{\"ok\":%s,\"body\":\"%s\"}",
                    ok ? "true" : "false", result);
        }
        free(url);
    }
    else if (strcmp(type, "process_list") == 0) {
        char *pattern = json_get_str(req, "pattern");
        if (!pattern) pattern = (char*)"";
        exec_process_list(pattern, result, &ok, &status);
        snprintf(resp, MAX_MSG, "{\"ok\":%s,\"processes\":\"%s\"}",
                ok ? "true" : "false", result);
        free(pattern);
    }
    else if (strcmp(type, "process_kill") == 0) {
        char *pid_str = json_get_str(req, "pid");
        int force = json_get_int(req, "force", 0);
        if (!pid_str) {
            snprintf(resp, MAX_MSG, "{\"ok\":false,\"error\":\"missing pid\"}");
        } else {
            exec_process_kill(pid_str, force, result, &ok, &status);
            snprintf(resp, MAX_MSG, "{\"ok\":%s,\"result\":\"%s\"}",
                    ok ? "true" : "false", result);
        }
        free(pid_str);
    }
    else if (strcmp(type, "ui") == 0) {
        char *action = json_get_str(req, "action");
        char *arg = json_get_str(req, "arg");
        if (!action) {
            snprintf(resp, MAX_MSG, "{\"ok\":false,\"error\":\"missing action\"}");
        } else {
            if (!arg) arg = (char*)"";
            exec_ui(action, arg, result, &ok, &status);
            snprintf(resp, MAX_MSG, "{\"ok\":%s,\"result\":\"%s\"}",
                    ok ? "true" : "false", result);
        }
        free(action);
        free(arg);
    }
    else if (strcmp(type, "screenshot") == 0) {
        char *path = json_get_str(req, "path");
        if (!path) path = (char*)"/data/local/tmp/screen.png";
        exec_screenshot(path, result, &ok, &status);
        snprintf(resp, MAX_MSG, "{\"ok\":%s,\"path\":\"%s\"}",
                ok ? "true" : "false", path);
        free(path);
    }
    else if (strcmp(type, "device_info") == 0) {
        exec_device_info(result, &ok, &status);
        snprintf(resp, MAX_MSG, "{\"ok\":%s,\"info\":\"%s\"}",
                ok ? "true" : "false", result);
    }
    else {
        snprintf(resp, MAX_MSG, "{\"ok\":false,\"error\":\"unknown type: %s\"}", type);
    }
    
    free(type);
}

/* ===== 信号处理 ===== */

void signal_handler(int sig) {
    log_msg("received signal, shutting down");
    unlink(SOCKET_PATH);
    unlink(PID_FILE);
    exit(0);
}

/* ===== 主程序 ===== */

int main() {
    // 隐藏进程名
    prctl(PR_SET_NAME, "input", 0, 0, 0);
    
    // 写 PID
    write_pid();
    
    // 注册信号处理
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    
    // 创建 Unix socket
    unlink(SOCKET_PATH);
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        log_msg("socket() failed");
        return 1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_msg("bind() failed");
        return 1;
    }
    
    if (listen(server_fd, 5) < 0) {
        log_msg("listen() failed");
        return 1;
    }
    
    log_msg("AgentCore v0.3 started");
    fprintf(stderr, "[AgentCore] pid=%d listening on %s\n", getpid(), SOCKET_PATH);
    fflush(stderr);
    
    // 主循环
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;
        
        // 读请求
        char req[MAX_MSG];
        int n = recv(client_fd, req, sizeof(req) - 1, 0);
        if (n <= 0) {
            close(client_fd);
            continue;
        }
        req[n] = '\0';
        
        log_msg(req);
        
        // 分发
        char resp[MAX_MSG];
        dispatch(req, resp);
        
        // 发响应
        send(client_fd, resp, strlen(resp), 0);
        close(client_fd);
    }
    
    return 0;
}