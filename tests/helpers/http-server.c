#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * Minimal static HTTP server for isolated release tests. Requests are limited to safe
 * root-relative GET paths, and an optional delay exercises timeout behavior without external
 * network access.
 */

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

typedef struct {
    const char *root;
    const char *ready_file;
    unsigned short port;
    long delay_ms;
} ServerOptions;

static volatile sig_atomic_t running = 1;

/* Process and socket lifecycle. */
static void stop_server(int signal_number) {
    (void)signal_number;
    running = 0;
}

static int network_start(void) {
#if defined(_WIN32)
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return 1;
#endif
}

static void network_stop(void) {
#if defined(_WIN32)
    WSACleanup();
#endif
}

static int parse_long(const char *text, long minimum, long maximum, long *value) {
    char *end = NULL;
    long parsed;

    if (text == NULL || value == NULL || *text == '\0') {
        return 0;
    }
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < minimum || parsed > maximum) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int parse_options(int argc, char **argv, ServerOptions *options) {
    int index;

    memset(options, 0, sizeof(*options));
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--root") == 0 && index + 1 < argc) {
            options->root = argv[++index];
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            long port;

            if (!parse_long(argv[++index], 1, 65535, &port)) {
                return 0;
            }
            options->port = (unsigned short)port;
        } else if (strcmp(argv[index], "--ready-file") == 0 && index + 1 < argc) {
            options->ready_file = argv[++index];
        } else if (strcmp(argv[index], "--delay-ms") == 0 && index + 1 < argc) {
            if (!parse_long(argv[++index], 0, 600000, &options->delay_ms)) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    return options->root != NULL && options->port != 0;
}

static socket_handle create_listener(unsigned short port) {
    struct sockaddr_in address;
    socket_handle listener;
    int option = 1;
    int option_result;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET_HANDLE) {
        return INVALID_SOCKET_HANDLE;
    }
#if defined(_WIN32)
    option_result =
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&option, sizeof(option));
#else
    option_result = setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
#endif
    if (option_result != 0) {
        close_socket(listener);
        return INVALID_SOCKET_HANDLE;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 16) != 0) {
        close_socket(listener);
        return INVALID_SOCKET_HANDLE;
    }
    return listener;
}

static int write_ready_file(const char *path) {
    FILE *file;
    int write_ok;
    int close_ok;

    if (path == NULL) {
        return 1;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    write_ok = fputs("ready\n", file) >= 0;
    close_ok = fclose(file) == 0;
    return write_ok && close_ok;
}

/* HTTP response helpers. */
static int send_all(socket_handle client, const char *data, size_t size) {
    while (size > 0) {
#if defined(_WIN32)
        int sent = send(client, data, size > 0x7fffffffU ? 0x7fffffff : (int)size, 0);
#else
        ssize_t sent = send(client, data, size, 0);
#endif
        if (sent <= 0) {
            return 0;
        }
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
        (void)send_all(client, header, (size_t)length);
        (void)send_all(client, body, strlen(body));
    }
}

static int safe_request_path(const char *path) {
    const unsigned char *cursor = (const unsigned char *)path;

    if (path[0] != '/' || strstr(path, "..") != NULL || strchr(path, '\\') != NULL) {
        return 0;
    }
    while (*cursor != '\0') {
        if (*cursor < 0x20 || *cursor == '%' || *cursor == '?' || *cursor == '#') {
            return 0;
        }
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

static int send_file(socket_handle client, FILE *file, long size) {
    char header[512];
    int length;

    length = snprintf(header,
                      sizeof(header),
                      "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: %ld\r\n"
                      "Content-Type: application/octet-stream\r\n\r\n",
                      size);
    if (length <= 0 || (size_t)length >= sizeof(header) ||
        !send_all(client, header, (size_t)length)) {
        return 0;
    }

    for (;;) {
        char buffer[16384];
        size_t count = fread(buffer, 1, sizeof(buffer), file);

        if (count > 0 && !send_all(client, buffer, count)) {
            return 0;
        }
        if (count < sizeof(buffer)) {
            return !ferror(file);
        }
    }
}

static void serve_client(socket_handle client, const ServerOptions *options) {
    char request[4096];
    char method[16];
    char url[2048];
    char version[32];
    char file_path[4096];
    FILE *file;
    long size;
#if defined(_WIN32)
    int received;
#else
    ssize_t received;
#endif

    if (options->delay_ms > 0) {
        sleep_milliseconds(options->delay_ms);
    }
#if defined(_WIN32)
    received = recv(client, request, (int)sizeof(request) - 1, 0);
#else
    received = recv(client, request, sizeof(request) - 1, 0);
#endif
    if (received <= 0) {
        return;
    }
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
    if (snprintf(file_path,
                 sizeof(file_path),
                 "%s/%s",
                 options->root,
                 url[1] != '\0' ? url + 1 : "index.html") >= (int)sizeof(file_path)) {
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
    (void)send_file(client, file, size);
    (void)fclose(file);
}

static int run_server(socket_handle listener, const ServerOptions *options) {
    int status = 0;

    while (running) {
        socket_handle client = accept(listener, NULL, NULL);

        if (client == INVALID_SOCKET_HANDLE) {
            if (!running) {
                break;
            }
#if !defined(_WIN32)
            if (errno == EINTR) {
                continue;
            }
#endif
            status = 1;
            break;
        }
        serve_client(client, options);
        close_socket(client);
    }
    return status;
}

/* Process entry point. */
int main(int argc, char **argv) {
    ServerOptions options;
    socket_handle listener;
    int status;

    if (!parse_options(argc, argv, &options)) {
        fprintf(stderr,
                "usage: %s --root DIR --port PORT [--ready-file PATH] [--delay-ms N]\n",
                argv[0]);
        return 2;
    }
    if (!network_start()) {
        fprintf(stderr, "network initialization failed\n");
        return 1;
    }
    if (signal(SIGINT, stop_server) == SIG_ERR || signal(SIGTERM, stop_server) == SIG_ERR) {
        network_stop();
        return 1;
    }

    listener = create_listener(options.port);
    if (listener == INVALID_SOCKET_HANDLE) {
        fprintf(stderr, "bind/listen failed on port %u\n", (unsigned)options.port);
        network_stop();
        return 1;
    }
    if (!write_ready_file(options.ready_file)) {
        fprintf(stderr, "failed to create ready file\n");
        close_socket(listener);
        network_stop();
        return 1;
    }

    status = run_server(listener, &options);
    close_socket(listener);
    network_stop();
    return status;
}
