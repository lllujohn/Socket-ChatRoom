#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET socket_t;
    #define INVALID_SOCK    INVALID_SOCKET
    #define CLOSE_SOCKET(s) closesocket(s)
    #define SOCK_ERR        SOCKET_ERROR
    #define SELECT_NFDS(m)  0
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    typedef int socket_t;
    #define INVALID_SOCK    (-1)
    #define CLOSE_SOCKET(s) close(s)
    #define SOCK_ERR        (-1)
    #define SELECT_NFDS(m)  ((m) + 1)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define SERVER_PORT  8888
#define MAX_CLIENTS  64
#define BUFFER_SIZE  2048
#define NICK_SIZE    32

#define C_SYS  "\033[33m" // 黄色
#define C_PRIV "\033[35m" // 紫色
#define C_CHAT "\033[36m" // 青色
#define C_RST  "\033[0m"

#define PING_INTERVAL_SEC 30
#define PING_TIMEOUT_SEC  90

typedef struct {
    socket_t           fd;
    struct sockaddr_in addr;
    char               nickname[NICK_SIZE];
    bool               active;
    time_t             last_active;
    time_t             last_ping;
} ClientSlot;

ClientSlot g_clients[MAX_CLIENTS];
int        g_client_count = 0;
socket_t   g_listen_fd    = INVALID_SOCK;
volatile sig_atomic_t g_running = 1;
FILE      *g_log_fp       = NULL;

// 统计信息
time_t     g_start_time;
uint64_t   g_total_msgs = 0;
uint64_t   g_total_connections = 0;

void disconnect_client(int idx);

void check_log_rotation() {
    if (!g_log_fp) return;
    long size = ftell(g_log_fp);
    if (size > 5 * 1024 * 1024) { // 5MB 轮转
        fclose(g_log_fp);
        char backup_name[64];
        snprintf(backup_name, sizeof(backup_name), "chat_%ld.log", (long)time(NULL));
        rename("chat.log", backup_name);
        g_log_fp = fopen("chat.log", "a");
    }
}

void log_event(const char *msg) {
    if (g_log_fp) {
        check_log_rotation();
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        fprintf(g_log_fp, "[%04d-%02d-%02d %02d:%02d:%02d] %s",
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                t->tm_hour, t->tm_min, t->tm_sec, msg);
        fflush(g_log_fp);
    }
}

int send_str(socket_t fd, const char *msg) {
    int total = (int)strlen(msg);
    int sent = 0;
    while (sent < total) {
        int ret = send(fd, msg + sent, total - sent, 0);
        if (ret <= 0) return -1;
        sent += ret;
    }
    return sent;
}

void broadcast(int exclude_idx, const char *raw_msg, const char *color) {
    char colored[BUFFER_SIZE * 2];
    snprintf(colored, sizeof(colored), "%s%s%s", color ? color : "", raw_msg, color ? C_RST : "");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && i != exclude_idx) {
            if (send_str(g_clients[i].fd, colored) < 0) {
                printf("[INFO] 向 %s 广播失败，强制断开\n", g_clients[i].nickname);
                CLOSE_SOCKET(g_clients[i].fd);
                g_clients[i].active = false;
                g_client_count--;
            }
        }
    }
}

// 检查昵称是否重名
bool is_nick_duplicate(const char *nick) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && strcmp(g_clients[i].nickname, nick) == 0) {
            return true;
        }
    }
    return false;
}

int handle_command(int sender_idx, const char *raw) {
    // 检查是否为心跳回复
    if (strcmp(raw, "[PONG]\n") == 0) {
        g_clients[sender_idx].last_active = time(NULL);
        return 1;
    }

    g_total_msgs++;

    // 私聊 /msg
    if (strncmp(raw, "/msg ", 5) == 0) {
        const char *space_pos = strchr(raw + 5, ' ');
        if (!space_pos) {
            char err[128];
            snprintf(err, sizeof(err), "%s[系统] 格式错误 (用法：/msg 昵称 消息)\n%s", C_SYS, C_RST);
            send_str(g_clients[sender_idx].fd, err);
            return 1;
        }

        int nick_len = space_pos - (raw + 5);
        int target_idx = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].active && (int)strlen(g_clients[i].nickname) == nick_len && strncmp(raw + 5, g_clients[i].nickname, nick_len) == 0) {
                target_idx = i;
                break;
            }
        }

        if (target_idx < 0) {
            char err[128];
            snprintf(err, sizeof(err), "%s[系统] 目标用户不存在\n%s", C_SYS, C_RST);
            send_str(g_clients[sender_idx].fd, err);
            return 1;
        }

        if (target_idx == sender_idx) {
            char err[128];
            snprintf(err, sizeof(err), "%s[系统] 不能给自己发私聊\n%s", C_SYS, C_RST);
            send_str(g_clients[sender_idx].fd, err);
            return 1;
        }

        const char *body = space_pos + 1;
        if (*body == '\0' || *body == '\n') {
            char err[128];
            snprintf(err, sizeof(err), "%s[系统] 消息内容不能为空\n%s", C_SYS, C_RST);
            send_str(g_clients[sender_idx].fd, err);
            return 1;
        }

        char raw_pm[BUFFER_SIZE + NICK_SIZE + 16];
        snprintf(raw_pm, sizeof(raw_pm), "[私聊] %s: %s", g_clients[sender_idx].nickname, body);
        char c_pm[BUFFER_SIZE * 2];
        snprintf(c_pm, sizeof(c_pm), "%s%s%s", C_PRIV, raw_pm, C_RST);
        send_str(g_clients[target_idx].fd, c_pm);

        char raw_echo[BUFFER_SIZE + NICK_SIZE * 2 + 24];
        snprintf(raw_echo, sizeof(raw_echo), "[私聊→%s] %s", g_clients[target_idx].nickname, body);
        char c_echo[BUFFER_SIZE * 2];
        snprintf(c_echo, sizeof(c_echo), "%s%s%s", C_PRIV, raw_echo, C_RST);
        send_str(g_clients[sender_idx].fd, c_echo);
        
        // 记录日志
        char log_str[BUFFER_SIZE * 2];
        snprintf(log_str, sizeof(log_str), "[私聊] %s -> %s: %s", g_clients[sender_idx].nickname, g_clients[target_idx].nickname, body);
        log_event(log_str);

        return 1;
    }
    
    // 改名命令 /nick
    if (strncmp(raw, "/nick ", 6) == 0) {
        char new_nick[NICK_SIZE] = {0};
        if (sscanf(raw + 6, "%31s", new_nick) == 1) {
            if (is_nick_duplicate(new_nick)) {
                char err[128];
                snprintf(err, sizeof(err), "%s[系统] 昵称 \"%s\" 已被占用\n%s", C_SYS, new_nick, C_RST);
                send_str(g_clients[sender_idx].fd, err);
            } else {
                char msg[128];
                snprintf(msg, sizeof(msg), "[系统] %s 改名为 %s\n", g_clients[sender_idx].nickname, new_nick);
                strncpy(g_clients[sender_idx].nickname, new_nick, NICK_SIZE - 1);
                log_event(msg);
                broadcast(-1, msg, C_SYS);
            }
        }
        return 1;
    }
    
    // 查看在线人员 /who
    if (strncmp(raw, "/who", 4) == 0 && (raw[4] == '\n' || raw[4] == '\r' || raw[4] == '\0' || raw[4] == ' ')) {
        char list[4096] = {0}; // 扩大缓冲区
        snprintf(list, sizeof(list), "%s[系统] 当前在线名单:\n", C_SYS);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].active) {
                char tmp[NICK_SIZE + 8];
                snprintf(tmp, sizeof(tmp), " - %s\n", g_clients[i].nickname);
                strncat(list, tmp, sizeof(list) - strlen(list) - 1);
            }
        }
        strncat(list, C_RST, sizeof(list) - strlen(list) - 1);
        send_str(g_clients[sender_idx].fd, list);
        return 1;
    }

    // 系统状态 /stats
    if (strncmp(raw, "/stats", 6) == 0 && (raw[6] == '\n' || raw[6] == '\r' || raw[6] == '\0' || raw[6] == ' ')) {
        char stats[512];
        time_t uptime = time(NULL) - g_start_time;
        snprintf(stats, sizeof(stats),
            "%s[系统状态]\n"
            "  运行时间: %ld 秒\n"
            "  当前在线: %d 人\n"
            "  累计连接: %llu 次\n"
            "  累计消息: %llu 条\n%s",
            C_SYS, (long)uptime, g_client_count,
            (unsigned long long)g_total_connections,
            (unsigned long long)g_total_msgs, C_RST);
        send_str(g_clients[sender_idx].fd, stats);
        return 1;
    }
    
    // 帮助命令 /help
    if (strncmp(raw, "/help", 5) == 0 && (raw[5] == '\n' || raw[5] == '\r' || raw[5] == '\0' || raw[5] == ' ')) {
        char help[512];
        snprintf(help, sizeof(help), 
            "%s[系统] 可用命令列表:\n"
            "  /help        显示此帮助\n"
            "  /who         查看当前在线的所有用户\n"
            "  /stats       查看服务器运行状态\n"
            "  /nick 新昵称 改名\n"
            "  /msg 昵称 消息内容  发送私聊\n"
            "  /quit        退出聊天室\n%s", C_SYS, C_RST);
        send_str(g_clients[sender_idx].fd, help);
        return 1;
    }

    return 0;
}

void disconnect_client(int idx) {
    if (!g_clients[idx].active) return;

    char msg[NICK_SIZE + 48];
    snprintf(msg, sizeof(msg), "[系统] %s 离开了聊天室，当前在线: %d 人\n",
             g_clients[idx].nickname, g_client_count - 1);
    printf("%s", msg);
    log_event(msg);

    CLOSE_SOCKET(g_clients[idx].fd);
    g_clients[idx].active = false;
    g_client_count--;
    
    if (g_running) {
        broadcast(-1, msg, C_SYS);
    }
}

void sigint_handler(int signum) {
    (void)signum;
    g_running = 0;
}

void graceful_shutdown(void) {
    printf("\n[INFO] 正在关闭服务器...\n");
    log_event("[INFO] 服务器正在关闭...\n");

    char bye[128];
    snprintf(bye, sizeof(bye), "%s[系统] 服务器正在关闭，连接即将断开...\n%s", C_SYS, C_RST);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            send_str(g_clients[i].fd, bye);
            CLOSE_SOCKET(g_clients[i].fd);
            g_clients[i].active = false;
        }
    }
    if (g_listen_fd != INVALID_SOCK)
        CLOSE_SOCKET(g_listen_fd);

    if (g_log_fp) {
        fclose(g_log_fp);
    }

#ifdef _WIN32
    WSACleanup();
#endif
    printf("[INFO] 服务端已安全退出\n");
    exit(0);
}

void set_tcp_options(socket_t fd) {
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&keepalive, sizeof(keepalive));

    // 禁用 Nagle 算法以降低延迟
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    
#ifndef _WIN32
    int idle = 10;
    int intvl = 3;
    int cnt = 3;
#ifdef __APPLE__
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#else
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

void check_heartbeats() {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            int idle_time = now - g_clients[i].last_active;
            if (idle_time > PING_TIMEOUT_SEC) {
                char msg[128];
                snprintf(msg, sizeof(msg), "[系统] %s 连接超时，已被强制断开\n", g_clients[i].nickname);
                printf("%s", msg);
                log_event(msg);
                disconnect_client(i);
            } else if (now - g_clients[i].last_ping >= PING_INTERVAL_SEC) {
                g_clients[i].last_ping = now;
                send_str(g_clients[i].fd, "[PING]\n");
            }
        }
    }
}

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup 失败\n");
        return 1;
    }
#endif

    g_start_time = time(NULL);
    g_log_fp = fopen("chat.log", "a");
    if (!g_log_fp) {
        perror("无法打开 chat.log");
    }

    signal(SIGINT, sigint_handler);
    memset(g_clients, 0, sizeof(g_clients));

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd == INVALID_SOCK) {
        perror("socket 创建失败");
        exit(1);
    }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family      = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port        = htons(SERVER_PORT);

    if (bind(g_listen_fd, (struct sockaddr *)&srv, sizeof(srv)) == SOCK_ERR) {
        perror("bind 失败");
        exit(1);
    }

    if (listen(g_listen_fd, 16) == SOCK_ERR) {
        perror("listen 失败");
        exit(1);
    }

    printf("--- 服务端已启动，监听端口 %d ---\n", SERVER_PORT);
    printf("按 Ctrl+C 可以安全关闭服务器\n\n");
    log_event("--- 服务端启动 ---\n");

    while (g_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(g_listen_fd, &read_fds);
        socket_t max_fd = g_listen_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].active) {
                FD_SET(g_clients[i].fd, &read_fds);
                if (g_clients[i].fd > max_fd)
                    max_fd = g_clients[i].fd;
            }
        }

        struct timeval tv = {1, 0};
        int ready = select(SELECT_NFDS(max_fd), &read_fds, NULL, NULL, &tv);

        if (!g_running) break;

        if (ready < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            perror("select 出错");
            continue;
        }

        // 检查心跳与超时
        check_heartbeats();

        if (ready == 0) continue;

        if (FD_ISSET(g_listen_fd, &read_fds)) {
            struct sockaddr_in cli_addr;
#ifdef _WIN32
            int addr_len = sizeof(cli_addr);
#else
            socklen_t addr_len = sizeof(cli_addr);
#endif
            socket_t cli_fd = accept(g_listen_fd, (struct sockaddr *)&cli_addr, &addr_len);
            if (cli_fd == INVALID_SOCK) {
                perror("accept 失败");
            } else if (g_client_count >= MAX_CLIENTS) {
                char err[128];
                snprintf(err, sizeof(err), "%s[系统] 服务器已满，请稍后重试\n%s", C_SYS, C_RST);
                send_str(cli_fd, err);
                CLOSE_SOCKET(cli_fd);
            } else {
                char nick[NICK_SIZE] = {0};
                int nlen = recv(cli_fd, nick, sizeof(nick) - 1, 0);
                if (nlen <= 0) {
                    CLOSE_SOCKET(cli_fd);
                } else {
                    nick[strcspn(nick, "\r\n")] = '\0';
                    if (strlen(nick) == 0)
                        snprintf(nick, sizeof(nick), "匿名用户");

                    if (is_nick_duplicate(nick)) {
                        char err[128];
                        snprintf(err, sizeof(err), "%s[系统] 昵称 \"%s\" 已被占用，连接断开\n%s", C_SYS, nick, C_RST);
                        send_str(cli_fd, err);
                        CLOSE_SOCKET(cli_fd);
                    } else {
                        set_tcp_options(cli_fd);

                        for (int i = 0; i < MAX_CLIENTS; i++) {
                            if (!g_clients[i].active) {
                                g_clients[i].fd     = cli_fd;
                                g_clients[i].addr   = cli_addr;
                                g_clients[i].active = true;
                                g_clients[i].last_active = time(NULL);
                                g_clients[i].last_ping = time(NULL);
                                g_client_count++;
                                g_total_connections++;
                                strncpy(g_clients[i].nickname, nick, NICK_SIZE - 1);

                                char ip[INET_ADDRSTRLEN];
                                inet_ntop(AF_INET, &cli_addr.sin_addr, ip, sizeof(ip));
                                printf("[新连接] 昵称: %s  地址: %s:%d  当前在线: %d\n",
                                       nick, ip, ntohs(cli_addr.sin_port), g_client_count);

                                char join_msg[NICK_SIZE + 48];
                                snprintf(join_msg, sizeof(join_msg),
                                         "[系统] %s 进入了聊天室，当前在线: %d 人\n",
                                         nick, g_client_count);
                                log_event(join_msg);
                                broadcast(-1, join_msg, C_SYS);
                                break;
                            }
                        }
                    }
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].active || !FD_ISSET(g_clients[i].fd, &read_fds))
                continue;

            char buf[BUFFER_SIZE] = {0};
            int rlen = recv(g_clients[i].fd, buf, sizeof(buf) - 1, 0);

            if (rlen <= 0) {
                disconnect_client(i);
                continue;
            }

            g_clients[i].last_active = time(NULL);

            buf[rlen] = '\0';
            buf[strcspn(buf, "\r\n")] = '\0';
            
            char log_msg[BUFFER_SIZE + NICK_SIZE + 4];
            snprintf(log_msg, sizeof(log_msg), "[%s] %s\n", g_clients[i].nickname, buf);
            
            char with_nl[BUFFER_SIZE + 2];
            snprintf(with_nl, sizeof(with_nl), "%s\n", buf);

            if (!handle_command(i, with_nl)) {
                printf("%s", log_msg);
                log_event(log_msg);
                broadcast(i, log_msg, C_CHAT);
            }
        }
    }

    graceful_shutdown();
    return 0;
}
