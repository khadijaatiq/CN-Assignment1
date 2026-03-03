/*
 * proxy.c - A concurrent HTTP/1.0 Proxy Server
 * CS3001 - Computer Networks, Assignment #1
 * FAST-NUCES Karachi, Spring 2026
 *
 * Features:
 *   - Handles concurrent client requests using fork()
 *   - Supports HTTP/1.0 GET method only
 *   - Returns 501 for non-GET methods
 *   - Returns 400 for malformed requests
 *   - Parses absolute URIs (host, port, path)
 *   - Configurable port via command line
 *   - Max 100 concurrent child processes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

/* ─── Constants ─────────────────────────────────────────────── */
#define BUFFER_SIZE      65536   /* 64 KB read/write buffer      */
#define MAX_HEADER_SIZE  8192    /* Max incoming header size      */
#define MAX_PROCESSES    100     /* Max concurrent child procs    */
#define DEFAULT_HTTP_PORT 80     /* Default HTTP port             */

/* ─── Parsed request structure ──────────────────────────────── */
typedef struct {
    char method[16];
    char host[256];
    int  port;
    char path[4096];
    char version[16];
    char headers[MAX_HEADER_SIZE]; /* raw headers after request line */
} HTTPRequest;

/* ─── Global child-process counter ──────────────────────────── */
volatile int child_count = 0;

/* ══════════════════════════════════════════════════════════════
 *  Signal Handlers
 * ══════════════════════════════════════════════════════════════ */

/* Reap zombie children and decrement counter */
void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        child_count--;
}

/* ══════════════════════════════════════════════════════════════
 *  Error Response Helpers
 * ══════════════════════════════════════════════════════════════ */

void send_error(int fd, int code, const char *reason, const char *body) {
    char response[1024];
    snprintf(response, sizeof(response),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html><body><h1>%d %s</h1><p>%s</p></body></html>\r\n",
        code, reason, code, reason, body);
    send(fd, response, strlen(response), 0);
}

void send_400(int fd) {
    send_error(fd, 400, "Bad Request",
               "Your browser sent a request the proxy could not understand.");
}

void send_501(int fd) {
    send_error(fd, 501, "Not Implemented",
               "The proxy only supports the GET method.");
}

/* ══════════════════════════════════════════════════════════════
 *  URL / Header Parser
 * ══════════════════════════════════════════════════════════════ */

/*
 * parse_request()
 *
 * Reads a raw HTTP request from the client socket and fills in
 * an HTTPRequest struct.
 *
 * Returns:
 *   1  – success
 *   0  – bad request (400)
 *  -1  – not implemented (501)
 */
int parse_request(int client_fd, HTTPRequest *req) {
    char raw[MAX_HEADER_SIZE];
    memset(raw, 0, sizeof(raw));

    /* ── Read data until we see the blank line (\r\n\r\n) ── */
    int total = 0;
    char tmp[1];
    while (total < (int)sizeof(raw) - 1) {
        int n = recv(client_fd, tmp, 1, 0);
        if (n <= 0) break;
        raw[total++] = tmp[0];
        /* Detect end of headers */
        if (total >= 4 &&
            raw[total-4] == '\r' && raw[total-3] == '\n' &&
            raw[total-2] == '\r' && raw[total-1] == '\n')
            break;
    }
    raw[total] = '\0';

    if (total == 0) return 0;   /* empty request */

    /* ── Extract the first (request) line ── */
    char req_line[512];
    char *eol = strstr(raw, "\r\n");
    if (!eol) return 0;

    int req_line_len = (int)(eol - raw);
    if (req_line_len >= (int)sizeof(req_line)) return 0;
    strncpy(req_line, raw, req_line_len);
    req_line[req_line_len] = '\0';

    /* ── Parse method, URL, version from request line ── */
    char url[4096];
    memset(req->method,  0, sizeof(req->method));
    memset(url,          0, sizeof(url));
    memset(req->version, 0, sizeof(req->version));

    if (sscanf(req_line, "%15s %4095s %15s",
               req->method, url, req->version) != 3)
        return 0;

    /* ── Validate HTTP version ── */
    if (strncmp(req->version, "HTTP/", 5) != 0) return 0;

    /* ── Check method ── */
    if (strcasecmp(req->method, "GET") != 0) return -1; /* 501 */

    /* ── Parse the absolute URI: http://host[:port]/path ── */
    if (strncasecmp(url, "http://", 7) != 0) return 0;   /* 400 */

    char *host_start = url + 7;

    /* Find where host ends (either '/' or end of string) */
    char *path_start = strchr(host_start, '/');
    char host_port[256];
    memset(host_port, 0, sizeof(host_port));

    if (path_start) {
        int hp_len = (int)(path_start - host_start);
        if (hp_len >= (int)sizeof(host_port)) return 0;
        strncpy(host_port, host_start, hp_len);
        strncpy(req->path, path_start, sizeof(req->path) - 1);
    } else {
        strncpy(host_port, host_start, sizeof(host_port) - 1);
        strcpy(req->path, "/");
    }

    if (strlen(req->path) == 0) strcpy(req->path, "/");

    /* ── Split host and port ── */
    char *colon = strchr(host_port, ':');
    if (colon) {
        int host_len = (int)(colon - host_port);
        if (host_len >= (int)sizeof(req->host)) return 0;
        strncpy(req->host, host_port, host_len);
        req->host[host_len] = '\0';
        req->port = atoi(colon + 1);
        if (req->port <= 0 || req->port > 65535) return 0;
    } else {
        strncpy(req->host, host_port, sizeof(req->host) - 1);
        req->port = DEFAULT_HTTP_PORT;
    }

    if (strlen(req->host) == 0) return 0;

    /* ── Store remaining headers (after request line) ── */
    char *headers_start = eol + 2;  /* skip \r\n */
    strncpy(req->headers, headers_start, sizeof(req->headers) - 1);

    return 1; /* success */
}

/* ══════════════════════════════════════════════════════════════
 *  Connect to Remote Server
 * ══════════════════════════════════════════════════════════════ */

int connect_to_server(const char *host, int port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;    /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        fprintf(stderr, "[proxy] getaddrinfo failed for host: %s\n", host);
        return -1;
    }

    int sockfd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) continue;
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) break; /* success */
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);
    return sockfd;  /* -1 on failure */
}

/* ══════════════════════════════════════════════════════════════
 *  Handle a Single Client (runs in child process)
 * ══════════════════════════════════════════════════════════════ */

void handle_client(int client_fd, struct sockaddr_in *client_addr) {
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr->sin_addr, client_ip, sizeof(client_ip));
    printf("[proxy] New connection from %s:%d\n",
           client_ip, ntohs(client_addr->sin_port));

    /* ── Parse the HTTP request ── */
    HTTPRequest req;
    memset(&req, 0, sizeof(req));
    int parse_result = parse_request(client_fd, &req);

    if (parse_result == 0) {
        fprintf(stderr, "[proxy] 400 Bad Request from %s\n", client_ip);
        send_400(client_fd);
        close(client_fd);
        return;
    }
    if (parse_result == -1) {
        fprintf(stderr, "[proxy] 501 Not Implemented from %s (method: %s)\n",
                client_ip, req.method);
        send_501(client_fd);
        close(client_fd);
        return;
    }

    printf("[proxy] %s http://%s:%d%s\n",
           req.method, req.host, req.port, req.path);

    /* ── Connect to the remote server ── */
    int server_fd = connect_to_server(req.host, req.port);
    if (server_fd < 0) {
        fprintf(stderr, "[proxy] Could not connect to %s:%d\n",
                req.host, req.port);
        send_error(client_fd, 502, "Bad Gateway",
                   "The proxy could not connect to the remote server.");
        close(client_fd);
        return;
    }

    /* ── Build and send forward request to the real server ── */
    char forward[MAX_HEADER_SIZE + 512];
    snprintf(forward, sizeof(forward),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             req.path, req.host);

    if (send(server_fd, forward, strlen(forward), 0) < 0) {
        fprintf(stderr, "[proxy] send to server failed\n");
        send_error(client_fd, 502, "Bad Gateway", "Failed to forward request.");
        close(server_fd);
        close(client_fd);
        return;
    }

    /* ── Relay response from server → client ── */
    char buffer[BUFFER_SIZE];
    ssize_t bytes;
    long total_bytes = 0;

    while ((bytes = recv(server_fd, buffer, sizeof(buffer), 0)) > 0) {
        ssize_t sent = 0;
        while (sent < bytes) {
            ssize_t s = send(client_fd, buffer + sent, bytes - sent, 0);
            if (s < 0) {
                fprintf(stderr, "[proxy] send to client failed\n");
                goto done;
            }
            sent += s;
        }
        total_bytes += bytes;
    }

done:
    printf("[proxy] Done – transferred %ld bytes for %s%s\n",
           total_bytes, req.host, req.path);

    close(server_fd);
    close(client_fd);
}

/* ══════════════════════════════════════════════════════════════
 *  main()
 * ══════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    /* ── Set up SIGCHLD handler to reap zombies ── */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    /* ── Create listening socket ── */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* Allow reuse of port immediately after restart */
    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    /* ── Bind to the specified port on all interfaces ── */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, MAX_PROCESSES) < 0) {
        perror("listen");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    printf("╔══════════════════════════════════════════╗\n");
    printf("║   HTTP Proxy Server – CS3001 Assign #1   ║\n");
    printf("║   FAST-NUCES Karachi  –  Spring 2026     ║\n");
    printf("╚══════════════════════════════════════════╝\n");
    printf("[proxy] Listening on port %d ...\n\n", port);

    /* ── Main accept loop ── */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(listen_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;   /* interrupted by signal – retry */
            perror("accept");
            continue;
        }

        /* Enforce maximum concurrent process limit */
        if (child_count >= MAX_PROCESSES) {
            fprintf(stderr,
                    "[proxy] Max processes (%d) reached – dropping connection\n",
                    MAX_PROCESSES);
            send_error(client_fd, 503, "Service Unavailable",
                       "Proxy is at capacity. Please try again later.");
            close(client_fd);
            continue;
        }

        /* Fork a child to handle the request */
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            /* ── Child process ── */
            close(listen_fd);           /* child doesn't need the listen socket */
            handle_client(client_fd, &client_addr);
            exit(EXIT_SUCCESS);
        }

        /* ── Parent process ── */
        child_count++;
        close(client_fd);           /* parent doesn't handle this client */
    }

    close(listen_fd);
    return 0;
}
