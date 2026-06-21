# Socket-ChatRoom

计算机网络课程设计 - 局域网多线程群聊程序

这是一个用 C 语言写的局域网聊天室作业。服务端用了单线程的 `select()` 来处理并发，客户端用多线程来收发消息。支持在 Windows、Linux 和 macOS 上编译运行。

## 功能说明

- 支持所有人一起聊天，或者指定人发私聊（`/msg 昵称 消息`）
- 支持查看当前在线的人（`/who`）和服务器运行情况（`/stats`）
- 支持自己改名字（`/nick 新昵称`）
- 客户端按 Ctrl+C 可以安全退出，不会让服务端报错
- 有简单的心跳包机制，能自动清理掉线的人

## 怎么编译运行

项目里写了跨平台的 Makefile，直接编译就行。

### Linux / macOS 环境
```bash
make
./server
./client 127.0.0.1 8888
```

### Windows 环境
如果有 MinGW 的话，直接在命令行运行 `make` 就能生成 exe 文件：
```bash
make
server.exe
client.exe 127.0.0.1 8888
```

## 文件说明

- `server.c`: 服务端代码
- `client.c`: 客户端代码
- `Makefile`: 编译脚本
- `设计说明.md`: 课设的报告文档，里面有协议细节和流程图。
