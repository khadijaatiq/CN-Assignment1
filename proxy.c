/*
 * proxy.c - Concurrent HTTP Proxy Server
 * CS3001 - Computer Networks, Assignment #1
 * FAST-NUCES Karachi, Spring 2026
 *
 * Accepts HTTP/1.0 and HTTP/1.1 from browsers,
 * forwards as HTTP/1.0 to origin servers.
 * Uses fork() for concurrency, max 100 processes.
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

/* ── Constants ──────────────────────────────────────────── */
#define BUFFER_SIZE       65536
#define MAX_HEADER_SIZE   16384   /* increased: browsers send big headers */
#define MAX_PROCESSES     100
#define DEFAULT_HTTP_PORT 80

/* ── Request struct ─────────────────────────────────────── */
typedef struct {
    char method[16];
    char host[256];
    int  port;
    char path[4096];
    char version[16];
} HTTPRequest;

/* ── Global child counter ───────────────────────────────── */
volatile int child_count = 0;

/* ════════════════════════════════════════════════════════
 *  SIGCHLD – reap zombies
 * ════════════════════════════════════════════════════════ */
void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        child_count--;
}

/* ════════════════════════════════════════════════════════
 *  Error senders
 * ════════════════════════════════════════════════════════ */
void send_error(int fd, int code, const char *reason, const char *body) {
    char resp[1024];
    snprintf(resp, sizeof(resp),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html><body><h1>%d %s</h1><p>%s</p></body></html>\r\n",
        code, reason, code, reason, body);
    send(fd, resp, strlen(resp), 0);
}

/* ════════════════════════════════════════════════════════
 *  Read full headers from socket (until \r\n\r\n)
 * ════════════════════════════════════════════════════════ */
int read_headers(int fd, char *buf, int bufsz) {
    int total = 0;
    char c;
    while (total < bufsz - 1) {
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) break;
        buf[total++] = c;
        if (total >= 4 &&
            buf[total-4]=='\r' && buf[total-3]=='\n' &&
            buf[total-2]=='\r' && buf[total-1]=='\n')
            break;
    }
    buf[total] = '\0';
    return total;
}

/* ════════════════════════════════════════════════════════
 *  Parse request line + extract host/port/path
 *
 *  Returns:  1 = OK (GET)
 *            0 = 400 Bad Request
 *           -1 = 501 Not Implemented
 * ════════════════════════════════════════════════════════ */
int parse_request(const char *raw, HTTPRequest *req) {
    /* --- pull out first line --- */
    char req_line[1024];
    const char *eol = strstr(raw, "\r\n");
    if (!eol) {
        /* try bare \n (some tools) */
        eol = strchr(raw, '\n');
        if (!eol) return 0;
    }
    int len = (int)(eol - raw);
    if (len >= (int)sizeof(req_line)) return 0;
    strncpy(req_line, raw, len);
    req_line[len] = '\0';

    /* --- parse: METHOD URL VERSION --- */
    char url[4096];
    memset(req->method,  0, sizeof(req->method));
    memset(url,          0, sizeof(url));
    memset(req->version, 0, sizeof(req->version));

    if (sscanf(req_line, "%15s %4095s %15s",
               req->method, url, req->version) != 3)
        return 0;

    /* version must start with HTTP/ */
    if (strncmp(req->version, "HTTP/", 5) != 0) return 0;

    /* CONNECT = used for HTTPS tunnelling → 501 */
    if (strcasecmp(req->method, "CONNECT") == 0) return -1;

    /* any method other than GET → 501 */
    if (strcasecmp(req->method, "GET") != 0) return -1;

    /* must be absolute URI: http://host... */
    if (strncasecmp(url, "http://", 7) != 0) return 0;

    char *host_start = url + 7;

    /* split host:port from path */
    char *path_start = strchr(host_start, '/');
    char host_port[256];
    memset(host_port, 0, sizeof(host_port));

    if (path_start) {
        int hp_len = (int)(path_start - host_start);
        if (hp_len >= (int)sizeof(host_port)) return 0;
        strncpy(host_port, host_start, hp_len);
        strncpy(req->path, path_start, sizeof(req->path)-1);
    } else {
        strncpy(host_port, host_start, sizeof(host_port)-1);
        strcpy(req->path, "/");
    }
    if (strlen(req->path) == 0) strcpy(req->path, "/");

    /* split host and port */
    char *colon = strchr(host_port, ':');
    if (colon) {
        int hl = (int)(colon - host_port);
        if (hl >= (int)sizeof(req->host)) return 0;
        strncpy(req->host, host_port, hl);
        req->host[hl] = '\0';
        req->port = atoi(colon + 1);
        if (req->port <= 0 || req->port > 65535) return 0;
    } else {
        strncpy(req->host, host_port, sizeof(req->host)-1);
        req->port = DEFAULT_HTTP_PORT;
    }

    if (strlen(req->host) == 0) return 0;
    return 1;
}

/* ════════════════════════════════════════════════════════
 *  Connect TCP to remote host:port
 * ════════════════════════════════════════════════════════ */
int connect_to_server(const char *host, int port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        fprintf(stderr, "[proxy] DNS lookup failed: %s\n", host);
        return -1;
    }

    int sockfd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) continue;
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);
    return sockfd;
}

/* ════════════════════════════════════════════════════════
 *  Handle one client (runs inside child process)
 * ════════════════════════════════════════════════════════ */
void handle_client(int cfd, struct sockaddr_in *caddr) {
    /* log client IP */
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &caddr->sin_addr, ip, sizeof(ip));

    /* read full request headers */
    char raw[MAX_HEADER_SIZE];
    int  rawlen = read_headers(cfd, raw, sizeof(raw));
    if (rawlen == 0) { close(cfd); return; }

    /* debug: print first line received */
    char first[256] = {0};
    const char *nl = strchr(raw, '\n');
    if (nl) {
        int fl = (int)(nl - raw);
        if (fl > 254) fl = 254;
        strncpy(first, raw, fl);
        /* strip \r */
        if (fl > 0 && first[fl-1]=='\r') first[fl-1]='\0';
    }
    printf("[proxy] From %s:%d  →  %s\n", ip, ntohs(caddr->sin_port), first);

    /* parse */
    HTTPRequest req;
    memset(&req, 0, sizeof(req));
    int result = parse_request(raw, &req);

    if (result == 0) {
        fprintf(stderr, "[proxy] 400 – bad request from %s\n", ip);
        send_error(cfd, 400, "Bad Request",
                   "Your browser sent a request the proxy could not understand.");
        close(cfd); return;
    }
    if (result == -1) {
        fprintf(stderr, "[proxy] 501 – method not supported: %s from %s\n",
                req.method, ip);
        send_error(cfd, 501, "Not Implemented",
                   "This proxy only supports the GET method. "
                   "HTTPS (CONNECT) is not supported.");
        close(cfd); return;
    }

    printf("[proxy] Forwarding GET http://%s:%d%s\n",
           req.host, req.port, req.path);

    /* connect to origin server */
    int sfd = connect_to_server(req.host, req.port);
    if (sfd < 0) {
        fprintf(stderr, "[proxy] Cannot reach %s:%d\n", req.host, req.port);
        send_error(cfd, 502, "Bad Gateway",
                   "Proxy could not connect to the remote server.");
        close(cfd); return;
    }

    /* forward clean HTTP/1.0 request to server */
    char fwd[MAX_HEADER_SIZE];
    snprintf(fwd, sizeof(fwd),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             req.path, req.host);

    if (send(sfd, fwd, strlen(fwd), 0) < 0) {
        send_error(cfd, 502, "Bad Gateway", "Failed to forward request.");
        close(sfd); close(cfd); return;
    }

    /* relay response bytes: server → client */
    char buf[BUFFER_SIZE];
    ssize_t n;
    long total = 0;
    while ((n = recv(sfd, buf, sizeof(buf), 0)) > 0) {
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t s = send(cfd, buf+sent, n-sent, 0);
            if (s < 0) goto done;
            sent += s;
        }
        total += n;
    }

done:
    printf("[proxy] Done  %s%s  (%ld bytes)\n", req.host, req.path, total);
    close(sfd);
    close(cfd);
}

/* ════════════════════════════════════════════════════════
 *  main
 * ════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        exit(1);
    }

    /* reap zombie children */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    /* listening socket */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port        = htons(port);

    if (bind(lfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        perror("bind"); close(lfd); exit(1);
    }
    if (listen(lfd, MAX_PROCESSES) < 0) {
        perror("listen"); close(lfd); exit(1);
    }

    printf("╔══════════════════════════════════════════╗\n");
    printf("║   HTTP Proxy Server  –  CS3001 Asgn #1   ║\n");
    printf("║   FAST-NUCES Karachi  |  Spring 2026     ║\n");
    printf("╚══════════════════════════════════════════╝\n");
    printf("[proxy] Listening on port %d\n\n", port);

    /* accept loop */
    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(lfd, (struct sockaddr*)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); continue;
        }

        if (child_count >= MAX_PROCESSES) {
            send_error(cfd, 503, "Service Unavailable",
                       "Proxy at capacity – try again later.");
            close(cfd); continue;
        }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); close(cfd); continue; }

        if (pid == 0) {
            /* child */
            close(lfd);
            handle_client(cfd, &caddr);
            exit(0);
        }

        /* parent */
        child_count++;
        close(cfd);
    }

    close(lfd);
    return 0;
}
