/*
 * client.c - 局域网群聊客户端
 */

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
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <pthread.h>
    typedef int socket_t;
    #define INVALID_SOCK    (-1)
    #define CLOSE_SOCKET(s) close(s)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>

#define DEFAULT_PORT  8888
#define BUFFER_SIZE   2048
#define NICK_SIZE     32

socket_t g_sock    = INVALID_SOCK;
volatile sig_atomic_t g_running = 1;

void sigint_handler(int signum) {
    (void)signum;
    if (g_sock != INVALID_SOCK) {
        // 发送退出命令
#ifndef _WIN32
        const char msg[] = "/quit\n";
        write(g_sock, msg, sizeof(msg) - 1);
        const char exit_msg[] = "\n退出聊天室\n";
        write(STDOUT_FILENO, exit_msg, sizeof(exit_msg) - 1);
        _exit(0);
#else
        send(g_sock, "/quit\n", 6, 0);
        exit(0);
#endif
    }
}

#ifdef _WIN32
DWORD WINAPI recv_thread(LPVOID arg)
#else
void *recv_thread(void *arg)
#endif
{
    (void)arg;
    char buf[BUFFER_SIZE];

    while (g_running) {
        memset(buf, 0, sizeof(buf));
        int rlen = recv(g_sock, buf, sizeof(buf) - 1, 0);

        if (rlen <= 0) {
            if (g_running)
                printf("\n[系统] 与服务器的连接已断开\n");
            g_running = 0;
            break;
        }

        buf[rlen] = '\0';

        // 处理心跳包
        if (strcmp(buf, "[PING]\n") == 0 || strcmp(buf, "[PING]") == 0) {
            send(g_sock, "[PONG]\n", 7, 0);
            continue;
        }

        // 打印收到消息
        printf("\r\033[K%s", buf);
        if (buf[rlen - 1] != '\n') putchar('\n');
        if (g_running) printf("> ");
        fflush(stdout);
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int main(int argc, char *argv[])
{
    const char *server_ip   = "127.0.0.1";
    int         server_port = DEFAULT_PORT;

    if (argc >= 2) server_ip = argv[1];
    if (argc >= 3) server_port = atoi(argv[2]);

    signal(SIGINT, sigint_handler);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup 失败\n");
        return 1;
    }
#endif

    printf("--- 欢迎使用局域网群聊客户端 ---\n");
    printf("服务器地址: %s:%d\n\n", server_ip, server_port);

    char nick[NICK_SIZE] = {0};
    printf("请输入昵称: ");
    fflush(stdout);
    if (fgets(nick, sizeof(nick), stdin) == NULL) {
        printf("读取昵称失败\n");
        return 1;
    }
    nick[strcspn(nick, "\r\n")] = '\0';
    if (strlen(nick) == 0)
        snprintf(nick, sizeof(nick), "匿名用户");

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock == INVALID_SOCK) {
        perror("socket 创建失败");
        exit(1);
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons((uint16_t)server_port);

    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) <= 0) {
        printf("IP 地址格式不对: %s\n", server_ip);
        exit(1);
    }

    printf("正在连接服务器...\n");
    if (connect(g_sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("连接失败，请检查服务端是否启动");
        exit(1);
    }
    printf("连接成功！\n\n");

    if (send(g_sock, nick, (int)strlen(nick), 0) < 0) {
        perror("发送昵称失败");
        exit(1);
    }

#ifdef _WIN32
    HANDLE tid = CreateThread(NULL, 0, recv_thread, NULL, 0, NULL);
    if (!tid) {
        printf("创建接收线程失败\n");
        exit(1);
    }
    CloseHandle(tid);
#else
    pthread_t tid;
    if (pthread_create(&tid, NULL, recv_thread, NULL) != 0) {
        printf("创建接收线程失败\n");
        exit(1);
    }
    pthread_detach(tid);
#endif

    printf("输入消息后回车发送，/msg 昵称 内容 可以私聊，/quit 退出\n");
    printf("--------------------------------------------\n");

    char input[BUFFER_SIZE];
    while (g_running) {
        printf("> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) break;

        size_t len = strlen(input);
        if (len == BUFFER_SIZE - 1 && input[len - 1] != '\n') {
            printf("\r\033[K[系统] 输入内容过长\n");
            int c;
            while ((c = getchar()) != '\n' && c != EOF);
            input[len - 1] = '\n';
        }

        if (strncmp(input, "/quit", 5) == 0 && (input[5] == '\n' || input[5] == '\r' || input[5] == '\0')) {
            printf("退出聊天室\n");
            break;
        }

        if (strcmp(input, "\n") == 0 || !g_running) continue;

        if (send(g_sock, input, (int)strlen(input), 0) < 0) {
            printf("发送失败，可能已断线\n");
            break;
        }
    }

    g_running = 0;
    CLOSE_SOCKET(g_sock);

#ifdef _WIN32
    WSACleanup();
#endif
    printf("再见！\n");
    return 0;
}
