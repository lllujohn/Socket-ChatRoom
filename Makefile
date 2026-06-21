CC = gcc
CFLAGS = -std=c11 -Wall

ifeq ($(OS),Windows_NT)
    LDFLAGS_CLIENT = -lws2_32
    LDFLAGS_SERVER = -lws2_32
    EXE = .exe
else
    LDFLAGS_CLIENT = -pthread
    LDFLAGS_SERVER =
    EXE =
endif

all: client$(EXE) server$(EXE)

client$(EXE): client.c
	$(CC) $(CFLAGS) -o client$(EXE) client.c $(LDFLAGS_CLIENT)

server$(EXE): server.c
	$(CC) $(CFLAGS) -o server$(EXE) server.c $(LDFLAGS_SERVER)

clean:
	rm -f client client.exe server server.exe
