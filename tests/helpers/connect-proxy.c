#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static int write_all(int fd, const char *buffer, size_t length) {
    while (length > 0) {
        ssize_t written = send(fd, buffer, length, 0);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0)
            return -1;
        buffer += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int connect_upstream(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, port, &hints, &addresses) != 0)
        return -1;
    for (address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static int relay(int first, int second) {
    char buffer[16384];
    while (!stop_requested) {
        fd_set reads;
        int maximum = first > second ? first : second;
        int ready;
        FD_ZERO(&reads);
        FD_SET(first, &reads);
        FD_SET(second, &reads);
        ready = select(maximum + 1, &reads, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (FD_ISSET(first, &reads)) {
            ssize_t count = recv(first, buffer, sizeof(buffer), 0);
            if (count <= 0)
                return 0;
            if (write_all(second, buffer, (size_t)count) != 0)
                return -1;
        }
        if (FD_ISSET(second, &reads)) {
            ssize_t count = recv(second, buffer, sizeof(buffer), 0);
            if (count <= 0)
                return 0;
            if (write_all(first, buffer, (size_t)count) != 0)
                return -1;
        }
    }
    return 0;
}

static int handle_client(int client, FILE *log_file) {
    char request[8192];
    size_t used = 0;
    char host[256];
    char port[16];
    int upstream;
    static const char response[] = "HTTP/1.1 200 Connection Established\r\n\r\n";

    while (used + 1 < sizeof(request)) {
        ssize_t count = recv(client, request + used, sizeof(request) - used - 1, 0);
        if (count <= 0)
            return -1;
        used += (size_t)count;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL)
            break;
    }
    if (sscanf(request, "CONNECT %255[^:]:%15[0-9] HTTP/%*s", host, port) != 2) {
        static const char bad[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
        (void)write_all(client, bad, sizeof(bad) - 1);
        return -1;
    }
    upstream = connect_upstream(host, port);
    if (upstream < 0) {
        static const char bad_gateway[] = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        (void)write_all(client, bad_gateway, sizeof(bad_gateway) - 1);
        return -1;
    }
    fprintf(log_file, "CONNECT %s:%s\n", host, port);
    fflush(log_file);
    if (write_all(client, response, sizeof(response) - 1) != 0) {
        close(upstream);
        return -1;
    }
    (void)relay(client, upstream);
    close(upstream);
    return 0;
}

int main(int argc, char **argv) {
    struct sockaddr_in address;
    int listener;
    int enable = 1;
    long port_long;
    char *end = NULL;
    FILE *log_file;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> <log-file>\n", argv[0]);
        return 2;
    }
    port_long = strtol(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || port_long < 1 || port_long > 65535)
        return 2;
    log_file = fopen(argv[2], "a");
    if (log_file == NULL)
        return 1;
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port_long);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 8) != 0)
        return 1;
    while (!stop_requested) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        (void)handle_client(client, log_file);
        close(client);
    }
    close(listener);
    fclose(log_file);
    return 0;
}
