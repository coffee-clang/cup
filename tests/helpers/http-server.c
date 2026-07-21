#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
/* Minimal cross-platform static HTTP server for isolated release tests. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET socket_handle;
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int socket_handle;
#define INVALID_SOCKET_HANDLE (-1)
#define close_socket close
#endif

static volatile sig_atomic_t running = 1;

static void stop_server(int signal_number) {
    (void)signal_number;
    running = 0;
}

static int send_all(socket_handle client, const char *data, size_t size) {
    while (size > 0) {
#if defined(_WIN32)
        int sent = send(client, data, size > 0x7fffffffU ? 0x7fffffff : (int)size, 0);
#else
        ssize_t sent = send(client, data, size, 0);
#endif
        if (sent <= 0)
            return 0;
        data += sent;
        size -= (size_t)sent;
    }
    return 1;
}

static void respond_text(socket_handle client, int status, const char *reason, const char *body) {
    char header[512];
    int length = snprintf(header,
                          sizeof(header),
                          "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Length: %zu\r\n"
                          "Content-Type: text/plain\r\n\r\n",
                          status,
                          reason,
                          strlen(body));
    if (length > 0 && (size_t)length < sizeof(header)) {
        send_all(client, header, (size_t)length);
        send_all(client, body, strlen(body));
    }
}

static int safe_request_path(const char *path) {
    const unsigned char *cursor = (const unsigned char *)path;
    if (path[0] != '/' || strstr(path, "..") != NULL || strchr(path, '\\') != NULL) {
        return 0;
    }
    while (*cursor != '\0') {
        if (*cursor < 0x20 || *cursor == '%' || *cursor == '?' || *cursor == '#')
            return 0;
        cursor++;
    }
    return 1;
}

static void sleep_milliseconds(long delay_ms) {
#if defined(_WIN32)
    Sleep((DWORD)delay_ms);
#else
    struct timespec duration;
    duration.tv_sec = delay_ms / 1000;
    duration.tv_nsec = (delay_ms % 1000) * 1000000L;
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
#endif
}

static void serve_client(socket_handle client, const char *root, long delay_ms) {
    char request[4096];
    char method[16];
    char url[2048];
    char version[32];
    char file_path[4096];
    char header[512];
    FILE *file;
    long size;
#if defined(_WIN32)
    int received;
#else
    ssize_t received;
#endif
    if (delay_ms > 0)
        sleep_milliseconds(delay_ms);
#if defined(_WIN32)
    received = recv(client, request, (int)sizeof(request) - 1, 0);
#else
    received = recv(client, request, sizeof(request) - 1, 0);
#endif
    if (received <= 0)
        return;
    request[received] = '\0';
    if (sscanf(request, "%15s %2047s %31s", method, url, version) != 3 ||
        strcmp(method, "GET") != 0 || strncmp(version, "HTTP/", 5) != 0) {
        respond_text(client, 400, "Bad Request", "bad request\n");
        return;
    }
    if (!safe_request_path(url)) {
        respond_text(client, 403, "Forbidden", "forbidden\n");
        return;
    }
    if (snprintf(
            file_path, sizeof(file_path), "%s/%s", root, url[1] != '\0' ? url + 1 : "index.html") >=
        (int)sizeof(file_path)) {
        respond_text(client, 414, "URI Too Long", "uri too long\n");
        return;
    }
    file = fopen(file_path, "rb");
    if (file == NULL) {
        respond_text(client, 404, "Not Found", "not found\n");
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        respond_text(client, 500, "Internal Server Error", "read error\n");
        return;
    }
    {
        int length = snprintf(header,
                              sizeof(header),
                              "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: %ld\r\n"
                              "Content-Type: application/octet-stream\r\n\r\n",
                              size);
        if (length <= 0 || (size_t)length >= sizeof(header) ||
            !send_all(client, header, (size_t)length)) {
            fclose(file);
            return;
        }
    }
    for (;;) {
        char buffer[16384];
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count > 0 && !send_all(client, buffer, count))
            break;
        if (count < sizeof(buffer))
            break;
    }
    fclose(file);
}

int main(int argc, char **argv) {
    const char *root = NULL;
    const char *ready_file = NULL;
    long port = 0;
    long delay_ms = 0;
    socket_handle listener = INVALID_SOCKET_HANDLE;
    struct sockaddr_in address;
    int option = 1;
    int index;
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif
    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--root") == 0 && index + 1 < argc)
            root = argv[++index];
        else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc)
            port = strtol(argv[++index], NULL, 10);
        else if (strcmp(argv[index], "--ready-file") == 0 && index + 1 < argc)
            ready_file = argv[++index];
        else if (strcmp(argv[index], "--delay-ms") == 0 && index + 1 < argc)
            delay_ms = strtol(argv[++index], NULL, 10);
        else {
            fprintf(stderr,
                    "usage: %s --root DIR --port PORT [--ready-file PATH] [--delay-ms N]\n",
                    argv[0]);
            return 2;
        }
    }
    if (root == NULL || port < 1 || port > 65535 || delay_ms < 0 || delay_ms > 600000) {
        fprintf(stderr, "invalid server arguments\n");
        return 2;
    }
    signal(SIGINT, stop_server);
    signal(SIGTERM, stop_server);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET_HANDLE) {
        fprintf(stderr, "socket failed\n");
        return 1;
    }
#if defined(_WIN32)
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&option, sizeof(option));
#else
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 16) != 0) {
        fprintf(stderr, "bind/listen failed on port %ld\n", port);
        close_socket(listener);
        return 1;
    }
    if (ready_file != NULL) {
        FILE *ready = fopen(ready_file, "wb");
        if (ready == NULL) {
            fprintf(stderr, "failed to create ready file\n");
            close_socket(listener);
            return 1;
        }
        fputs("ready\n", ready);
        fclose(ready);
    }
    while (running) {
        socket_handle client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET_HANDLE) {
            if (!running)
                break;
#if !defined(_WIN32)
            if (errno == EINTR)
                continue;
#endif
            break;
        }
        serve_client(client, root, delay_ms);
        close_socket(client);
    }
    close_socket(listener);
#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
}
