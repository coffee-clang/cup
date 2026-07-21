#define _POSIX_C_SOURCE 200809L

/*
 * Minimal HTTP CONNECT proxy used by the isolated Linux network suite. It accepts one tunnel at a
 * time, records the requested authority, and relays bytes until either endpoint closes.
 */

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

/* Signal and socket helpers. */
static void handle_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static int parse_port(const char *text, unsigned short *port) {
    char *end = NULL;
    long value;

    if (text == NULL || port == NULL || *text == '\0') {
        return 0;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 1 || value > 65535) {
        return 0;
    }
    *port = (unsigned short)value;
    return 1;
}

static int write_all(int fd, const char *buffer, size_t length) {
    while (length > 0) {
        ssize_t written = send(fd, buffer, length, 0);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        buffer += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int create_listener(unsigned short port) {
    struct sockaddr_in address;
    int listener;
    int enable = 1;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return -1;
    }
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0) {
        close(listener);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 8) != 0) {
        close(listener);
        return -1;
    }
    return listener;
}

static int connect_upstream(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, port, &hints, &addresses) != 0) {
        return -1;
    }

    for (address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

/* One CONNECT tunnel. */
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
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (FD_ISSET(first, &reads)) {
            ssize_t count = recv(first, buffer, sizeof(buffer), 0);

            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return 0;
            }
            if (write_all(second, buffer, (size_t)count) != 0) {
                return -1;
            }
        }

        if (FD_ISSET(second, &reads)) {
            ssize_t count = recv(second, buffer, sizeof(buffer), 0);

            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return 0;
            }
            if (write_all(first, buffer, (size_t)count) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int read_connect_request(int client, char *request, size_t request_size) {
    size_t used = 0;

    while (used + 1 < request_size) {
        ssize_t count = recv(client, request + used, request_size - used - 1, 0);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return 0;
        }
        used += (size_t)count;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL) {
            return 1;
        }
    }
    return 0;
}

static int handle_client(int client, FILE *log_file) {
    static const char connected[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
    static const char bad_request[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
    static const char bad_gateway[] = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
    char request[8192];
    char host[256];
    char port[16];
    int upstream;
    int result;

    if (!read_connect_request(client, request, sizeof(request)) ||
        sscanf(request, "CONNECT %255[^:]:%15[0-9] HTTP/%*s", host, port) != 2) {
        (void)write_all(client, bad_request, sizeof(bad_request) - 1);
        return -1;
    }

    upstream = connect_upstream(host, port);
    if (upstream < 0) {
        (void)write_all(client, bad_gateway, sizeof(bad_gateway) - 1);
        return -1;
    }

    if (fprintf(log_file, "CONNECT %s:%s\n", host, port) < 0 || fflush(log_file) != 0 ||
        write_all(client, connected, sizeof(connected) - 1) != 0) {
        close(upstream);
        return -1;
    }

    result = relay(client, upstream);
    close(upstream);
    return result;
}

static int run_proxy(int listener, FILE *log_file) {
    int status = 0;

    while (!stop_requested) {
        int client = accept(listener, NULL, NULL);

        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = 1;
            break;
        }
        (void)handle_client(client, log_file);
        close(client);
    }
    return status;
}

/* Process entry point. */
int main(int argc, char **argv) {
    unsigned short port;
    FILE *log_file;
    int listener;
    int status;

    if (argc != 3 || !parse_port(argv[1], &port)) {
        fprintf(stderr, "usage: %s <port> <log-file>\n", argv[0]);
        return 2;
    }

    log_file = fopen(argv[2], "a");
    if (log_file == NULL) {
        return 1;
    }
    if (signal(SIGTERM, handle_signal) == SIG_ERR || signal(SIGINT, handle_signal) == SIG_ERR ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fclose(log_file);
        return 1;
    }

    listener = create_listener(port);
    if (listener < 0) {
        fclose(log_file);
        return 1;
    }

    status = run_proxy(listener, log_file);
    close(listener);
    if (fclose(log_file) != 0) {
        status = 1;
    }
    return status;
}
