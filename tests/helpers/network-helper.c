#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * Local network services used by isolated CUP integration tests. Libevent owns
 * the portable HTTP parsing, event loop, listeners and buffered socket I/O;
 * this file contains only CUP-specific fixture behavior.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/listener.h>
#include <event2/util.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#define REQUEST_LIMIT 8192U

enum {
    CUP_TEST_HTTP_OK = 200,
    CUP_TEST_HTTP_BAD_REQUEST = 400,
    CUP_TEST_HTTP_FORBIDDEN = 403,
    CUP_TEST_HTTP_NOT_FOUND = 404,
    CUP_TEST_HTTP_INTERNAL_ERROR = 500
};

typedef struct {
    const char *root;
    const char *ready_file;
    unsigned short port;
    long delay_ms;
} HttpOptions;

typedef struct ProxyState ProxyState;

typedef struct {
    ProxyState *proxy;
    struct bufferevent *client;
    struct bufferevent *upstream;
    char host[256];
    unsigned short port;
    int established;
    int closing;
} Tunnel;

struct ProxyState {
    struct event_base *base;
    FILE *log_file;
};

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

static int parse_port(const char *text, unsigned short *port) {
    long value;

    if (!parse_long(text, 1, 65535, &value)) {
        return 0;
    }
    *port = (unsigned short)value;
    return 1;
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

#if !defined(_WIN32)
static void stop_event_loop(evutil_socket_t signal_number, short events, void *context) {
    struct event_base *base = context;

    (void)signal_number;
    (void)events;
    event_base_loopexit(base, NULL);
}

static struct event *install_stop_signal(struct event_base *base, int signal_number) {
    struct event *event = evsignal_new(base, signal_number, stop_event_loop, base);

    if (event == NULL || event_add(event, NULL) != 0) {
        if (event != NULL) {
            event_free(event);
        }
        return NULL;
    }
    return event;
}
#endif

/* Static HTTP fixture server. */
static int parse_http_options(int argc, char **argv, HttpOptions *options) {
    int index;

    memset(options, 0, sizeof(*options));
    for (index = 0; index < argc; ++index) {
        if (strcmp(argv[index], "--root") == 0 && index + 1 < argc) {
            options->root = argv[++index];
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            if (!parse_port(argv[++index], &options->port)) {
                return 0;
            }
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

static int safe_request_path(const char *path) {
    const unsigned char *cursor = (const unsigned char *)path;

    if (path == NULL || path[0] != '/' || strstr(path, "..") != NULL ||
        strchr(path, '\\') != NULL) {
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

static void send_text_reply(struct evhttp_request *request,
                            int status,
                            const char *reason,
                            const char *text) {
    struct evbuffer *body = evbuffer_new();

    if (body == NULL) {
        evhttp_send_error(request, CUP_TEST_HTTP_INTERNAL_ERROR, "Internal Server Error");
        return;
    }
    if (evbuffer_add(body, text, strlen(text)) != 0) {
        evbuffer_free(body);
        evhttp_send_error(request, CUP_TEST_HTTP_INTERNAL_ERROR, "Internal Server Error");
        return;
    }
    (void)evhttp_add_header(evhttp_request_get_output_headers(request),
                            "Content-Type",
                            "text/plain");
    evhttp_send_reply(request, status, reason, body);
    evbuffer_free(body);
}

static int read_file_body(FILE *file, struct evbuffer *body) {
    for (;;) {
        unsigned char buffer[16384];
        size_t count = fread(buffer, 1, sizeof(buffer), file);

        if (count > 0 && evbuffer_add(body, buffer, count) != 0) {
            return 0;
        }
        if (count < sizeof(buffer)) {
            return !ferror(file);
        }
    }
}

static void serve_http_request(struct evhttp_request *request, void *context) {
    const HttpOptions *options = context;
    const char *path = evhttp_request_get_uri(request);
    char file_path[4096];
    struct evbuffer *body;
    FILE *file;
    int length;

    if (options->delay_ms > 0) {
        sleep_milliseconds(options->delay_ms);
    }
    if (evhttp_request_get_command(request) != EVHTTP_REQ_GET) {
        send_text_reply(request, CUP_TEST_HTTP_BAD_REQUEST, "Bad Request", "bad request\n");
        return;
    }
    if (!safe_request_path(path)) {
        send_text_reply(request, CUP_TEST_HTTP_FORBIDDEN, "Forbidden", "forbidden\n");
        return;
    }
    length = snprintf(file_path, sizeof(file_path), "%s%s", options->root, path);
    if (length < 0 || (size_t)length >= sizeof(file_path)) {
        send_text_reply(request, CUP_TEST_HTTP_BAD_REQUEST, "Bad Request", "path too long\n");
        return;
    }

    file = fopen(file_path, "rb");
    if (file == NULL) {
        send_text_reply(request, CUP_TEST_HTTP_NOT_FOUND, "Not Found", "not found\n");
        return;
    }
    body = evbuffer_new();
    if (body == NULL || !read_file_body(file, body)) {
        evbuffer_free(body);
        (void)fclose(file);
        send_text_reply(request, CUP_TEST_HTTP_INTERNAL_ERROR, "Internal Server Error", "read error\n");
        return;
    }
    if (fclose(file) != 0) {
        evbuffer_free(body);
        send_text_reply(request, CUP_TEST_HTTP_INTERNAL_ERROR, "Internal Server Error", "read error\n");
        return;
    }
    (void)evhttp_add_header(evhttp_request_get_output_headers(request),
                            "Content-Type",
                            "application/octet-stream");
    evhttp_send_reply(request, CUP_TEST_HTTP_OK, "OK", body);
    evbuffer_free(body);
}

static int run_http_server(int argc, char **argv) {
    HttpOptions options;
    struct event_base *base = NULL;
    struct evhttp *http = NULL;
#if !defined(_WIN32)
    struct event *interrupt_event = NULL;
    struct event *terminate_event = NULL;
#endif
    int status = 1;

    if (!parse_http_options(argc, argv, &options)) {
        fprintf(stderr,
                "usage: network-helper http-server --root DIR --port PORT "
                "[--ready-file PATH] [--delay-ms N]\n");
        return 2;
    }
    base = event_base_new();
    http = base == NULL ? NULL : evhttp_new(base);
    if (http == NULL) {
        fprintf(stderr, "failed to initialize HTTP server\n");
        goto cleanup;
    }
    evhttp_set_gencb(http, serve_http_request, &options);
    if (evhttp_bind_socket(http, "127.0.0.1", options.port) != 0) {
        fprintf(stderr, "bind/listen failed on port %u\n", (unsigned)options.port);
        goto cleanup;
    }
#if !defined(_WIN32)
    interrupt_event = install_stop_signal(base, SIGINT);
    terminate_event = install_stop_signal(base, SIGTERM);
    if (interrupt_event == NULL || terminate_event == NULL) {
        fprintf(stderr, "failed to install signal handlers\n");
        goto cleanup;
    }
#endif
    if (!write_ready_file(options.ready_file)) {
        fprintf(stderr, "failed to create ready file\n");
        goto cleanup;
    }
    status = event_base_dispatch(base) < 0 ? 1 : 0;

cleanup:
#if !defined(_WIN32)
    if (interrupt_event != NULL) {
        event_free(interrupt_event);
    }
    if (terminate_event != NULL) {
        event_free(terminate_event);
    }
#endif
    if (http != NULL) {
        evhttp_free(http);
    }
    if (base != NULL) {
        event_base_free(base);
    }
    return status;
}

/* HTTP CONNECT proxy fixture. */
static void tunnel_free(Tunnel *tunnel) {
    if (tunnel == NULL || tunnel->closing) {
        return;
    }
    tunnel->closing = 1;
    if (tunnel->client != NULL) {
        bufferevent_setcb(tunnel->client, NULL, NULL, NULL, NULL);
        bufferevent_free(tunnel->client);
    }
    if (tunnel->upstream != NULL) {
        bufferevent_setcb(tunnel->upstream, NULL, NULL, NULL, NULL);
        bufferevent_free(tunnel->upstream);
    }
    free(tunnel);
}

static void close_after_write(struct bufferevent *client, void *context) {
    Tunnel *tunnel = context;

    if (evbuffer_get_length(bufferevent_get_output(client)) == 0) {
        tunnel_free(tunnel);
    }
}

static void close_failure_event(struct bufferevent *client,
                                short events,
                                void *context) {
    (void)client;
    if ((events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) != 0) {
        tunnel_free(context);
    }
}

static void tunnel_fail(Tunnel *tunnel, const char *response) {
    if (tunnel->upstream != NULL) {
        bufferevent_free(tunnel->upstream);
        tunnel->upstream = NULL;
    }
    bufferevent_disable(tunnel->client, EV_READ);
    bufferevent_setcb(
        tunnel->client, NULL, close_after_write, close_failure_event, tunnel);
    if (bufferevent_write(tunnel->client, response, strlen(response)) != 0) {
        tunnel_free(tunnel);
        return;
    }
    bufferevent_enable(tunnel->client, EV_WRITE);
}

static void relay_read(struct bufferevent *source, void *context) {
    Tunnel *tunnel = context;
    struct bufferevent *destination =
        source == tunnel->client ? tunnel->upstream : tunnel->client;

    if (destination == NULL ||
        evbuffer_add_buffer(bufferevent_get_output(destination),
                            bufferevent_get_input(source)) != 0) {
        tunnel_free(tunnel);
    }
}

static void tunnel_event(struct bufferevent *event, short events, void *context) {
    static const char connected[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
    static const char bad_gateway[] = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
    Tunnel *tunnel = context;

    if (event == tunnel->upstream && (events & BEV_EVENT_CONNECTED) != 0) {
        if (fprintf(tunnel->proxy->log_file,
                    "CONNECT %s:%u\n",
                    tunnel->host,
                    (unsigned)tunnel->port) < 0 ||
            fflush(tunnel->proxy->log_file) != 0 ||
            bufferevent_write(tunnel->client, connected, sizeof(connected) - 1) != 0) {
            tunnel_fail(tunnel, bad_gateway);
            return;
        }
        tunnel->established = 1;
        bufferevent_setcb(tunnel->client, relay_read, NULL, tunnel_event, tunnel);
        bufferevent_setcb(tunnel->upstream, relay_read, NULL, tunnel_event, tunnel);
        if (evbuffer_add_buffer(bufferevent_get_output(tunnel->upstream),
                                bufferevent_get_input(tunnel->client)) != 0 ||
            bufferevent_enable(tunnel->client, EV_READ | EV_WRITE) != 0 ||
            bufferevent_enable(tunnel->upstream, EV_READ | EV_WRITE) != 0) {
            tunnel_free(tunnel);
        }
        return;
    }
    if ((events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) == 0) {
        return;
    }
    if (event == tunnel->upstream && !tunnel->established) {
        tunnel_fail(tunnel, bad_gateway);
    } else {
        tunnel_free(tunnel);
    }
}

static const unsigned char *find_header_end(const unsigned char *buffer,
                                             size_t length) {
    size_t index;

    if (buffer == NULL || length < 4) {
        return NULL;
    }
    for (index = 0; index + 4 <= length; ++index) {
        if (memcmp(buffer + index, "\r\n\r\n", 4) == 0) {
            return buffer + index + 4;
        }
    }
    return NULL;
}

static void read_connect_request(struct bufferevent *client, void *context) {
    static const char bad_request[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
    static const char bad_gateway[] = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
    Tunnel *tunnel = context;
    struct evbuffer *input = bufferevent_get_input(client);
    const unsigned char *buffer;
    const unsigned char *header_end;
    size_t input_size;
    char host[256];
    char port_text[16];
    char version[32];
    char *request;
    size_t header_size;
    unsigned short port;

    input_size = evbuffer_get_length(input);
    if (input_size > REQUEST_LIMIT) {
        tunnel_fail(tunnel, bad_request);
        return;
    }
    buffer = evbuffer_pullup(input, -1);
    if (buffer == NULL) {
        tunnel_fail(tunnel, bad_request);
        return;
    }
    header_end = find_header_end(buffer, input_size);
    if (header_end == NULL) {
        return;
    }
    header_size = (size_t)(header_end - buffer);
    request = malloc(header_size + 1);
    if (request == NULL || evbuffer_remove(input, request, header_size) != (int)header_size) {
        free(request);
        tunnel_fail(tunnel, bad_request);
        return;
    }
    request[header_size] = '\0';
    if (sscanf(request,
               "CONNECT %255[^:]:%15[0-9] HTTP/%31s",
               host,
               port_text,
               version) != 3 ||
        !parse_port(port_text, &port)) {
        free(request);
        tunnel_fail(tunnel, bad_request);
        return;
    }
    free(request);
    if (snprintf(tunnel->host, sizeof(tunnel->host), "%s", host) < 0 ||
        strlen(host) >= sizeof(tunnel->host)) {
        tunnel_fail(tunnel, bad_request);
        return;
    }
    tunnel->port = port;

    tunnel->upstream = bufferevent_socket_new(
        tunnel->proxy->base, -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    if (tunnel->upstream == NULL) {
        tunnel_fail(tunnel, bad_gateway);
        return;
    }
    bufferevent_setcb(tunnel->upstream, NULL, NULL, tunnel_event, tunnel);
    bufferevent_disable(client, EV_READ);
    if (bufferevent_socket_connect_hostname(
            tunnel->upstream, NULL, AF_INET, tunnel->host, tunnel->port) != 0) {
        tunnel_fail(tunnel, bad_gateway);
        return;
    }
    bufferevent_enable(tunnel->upstream, EV_READ | EV_WRITE);
}

static void accept_proxy_client(struct evconnlistener *listener,
                                evutil_socket_t socket,
                                struct sockaddr *address,
                                int address_length,
                                void *context) {
    ProxyState *proxy = context;
    Tunnel *tunnel = calloc(1, sizeof(*tunnel));

    (void)listener;
    (void)address;
    (void)address_length;
    if (tunnel == NULL) {
        evutil_closesocket(socket);
        return;
    }
    tunnel->proxy = proxy;
    tunnel->client = bufferevent_socket_new(
        proxy->base, socket, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    if (tunnel->client == NULL) {
        evutil_closesocket(socket);
        free(tunnel);
        return;
    }
    bufferevent_setcb(tunnel->client, read_connect_request, NULL, tunnel_event, tunnel);
    bufferevent_enable(tunnel->client, EV_READ | EV_WRITE);
}

static void proxy_listener_error(struct evconnlistener *listener, void *context) {
    ProxyState *proxy = context;

    (void)listener;
    event_base_loopbreak(proxy->base);
}

static int run_connect_proxy(int argc, char **argv) {
    struct sockaddr_in address;
    ProxyState proxy;
    struct evconnlistener *listener = NULL;
#if !defined(_WIN32)
    struct event *interrupt_event = NULL;
    struct event *terminate_event = NULL;
#endif
    unsigned short port;
    int status = 1;

    if (argc != 2 || !parse_port(argv[0], &port)) {
        fprintf(stderr, "usage: network-helper connect-proxy <port> <log-file>\n");
        return 2;
    }
    memset(&proxy, 0, sizeof(proxy));
    proxy.log_file = fopen(argv[1], "a");
    proxy.base = event_base_new();
    if (proxy.log_file == NULL || proxy.base == NULL) {
        fprintf(stderr, "failed to initialize CONNECT proxy\n");
        goto cleanup;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (evutil_inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        goto cleanup;
    }
    listener = evconnlistener_new_bind(proxy.base,
                                       accept_proxy_client,
                                       &proxy,
                                       LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
                                       16,
                                       (struct sockaddr *)&address,
                                       sizeof(address));
    if (listener == NULL) {
        fprintf(stderr, "bind/listen failed on port %u\n", (unsigned)port);
        goto cleanup;
    }
    evconnlistener_set_error_cb(listener, proxy_listener_error);
#if !defined(_WIN32)
    interrupt_event = install_stop_signal(proxy.base, SIGINT);
    terminate_event = install_stop_signal(proxy.base, SIGTERM);
    if (interrupt_event == NULL || terminate_event == NULL) {
        fprintf(stderr, "failed to install signal handlers\n");
        goto cleanup;
    }
#endif
    status = event_base_dispatch(proxy.base) < 0 ? 1 : 0;

cleanup:
#if !defined(_WIN32)
    if (interrupt_event != NULL) {
        event_free(interrupt_event);
    }
    if (terminate_event != NULL) {
        event_free(terminate_event);
    }
#endif
    if (listener != NULL) {
        evconnlistener_free(listener);
    }
    if (proxy.base != NULL) {
        event_base_free(proxy.base);
    }
    if (proxy.log_file != NULL && fclose(proxy.log_file) != 0) {
        status = 1;
    }
    return status;
}

int main(int argc, char **argv) {
    int status;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s http-server ... | connect-proxy ...\n",
                argv[0]);
        return 2;
    }
    if (!network_start()) {
        fprintf(stderr, "network initialization failed\n");
        return 1;
    }
    if (strcmp(argv[1], "http-server") == 0) {
        status = run_http_server(argc - 2, argv + 2);
    } else if (strcmp(argv[1], "connect-proxy") == 0) {
        status = run_connect_proxy(argc - 2, argv + 2);
    } else {
        fprintf(stderr, "unknown network-helper mode: %s\n", argv[1]);
        status = 2;
    }
    network_stop();
    return status;
}
