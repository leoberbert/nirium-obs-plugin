/*
Copyright (C) 2021 DEV47APPS, github.com/dev47apps
Copyright (C) 2026 Leonardo Berbert (rewritten for the Niriun protocol)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "plugin.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#define NET_ERRNO() WSAGetLastError()
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define NET_ERRNO() errno
#endif

bool net_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        elog("WSAStartup failed with error %d", rc);
        return false;
    }
#endif
    return true;
}

void net_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

void net_close(socket_t sock)
{
    if (sock == INVALID_SOCKET)
        return;
    shutdown(sock, 2 /* SD_BOTH == SHUT_RDWR */);
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

bool set_nonblock(socket_t sock, bool nonblock)
{
#ifdef _WIN32
    u_long nb = nonblock ? 1 : 0;
    return ioctlsocket(sock, FIONBIO, &nb) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
        return false;
    flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(sock, F_SETFL, flags) == 0;
#endif
}

int set_recv_buf_len(socket_t sock, int len)
{
    return setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&len, sizeof(len));
}

int set_send_timeout(socket_t sock, int ms)
{
#ifdef _WIN32
    DWORD timeout = (DWORD)ms;
#else
    struct timeval timeout;
    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (ms % 1000) * 1000;
#endif
    return setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
}

int net_poll(struct pollfd *fds, unsigned count, int timeout_ms)
{
#ifdef _WIN32
    return WSAPoll(fds, count, timeout_ms);
#else
    return poll(fds, count, timeout_ms);
#endif
}

int net_wait_readable(socket_t sock, int timeout_ms)
{
    struct pollfd p;
    p.fd = sock;
    p.events = POLLIN;
    p.revents = 0;
    int rc = net_poll(&p, 1, timeout_ms);
    if (rc < 0)
        return -1;
    return rc == 0 ? 0 : 1;
}

bool net_connect_ok(socket_t sock)
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0)
        return false;
    return err == 0;
}

static bool connect_in_progress()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

socket_t net_connect_begin_v4(uint32_t addr, uint16_t port)
{
    socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
        return INVALID_SOCKET;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(addr);
    sa.sin_port = htons(port);

    if (!set_nonblock(sock, true)) {
        net_close(sock);
        return INVALID_SOCKET;
    }

    if (connect(sock, (const struct sockaddr *)&sa, sizeof(sa)) != 0 && !connect_in_progress()) {
        net_close(sock);
        return INVALID_SOCKET;
    }
    return sock;
}

static socket_t connect_one(const struct addrinfo *ai, const struct sockaddr_storage *bind_addr, int bind_len,
                            int timeout_ms, const std::atomic<bool> *cancel)
{
    socket_t sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock == INVALID_SOCKET) {
        elog("socket(): error %d", NET_ERRNO());
        return INVALID_SOCKET;
    }

    if (bind_addr && bind_addr->ss_family == ai->ai_family) {
        if (bind(sock, (const struct sockaddr *)bind_addr, bind_len) != 0)
            elog("bind to local address failed: error %d", NET_ERRNO());
    }

    if (!set_nonblock(sock, true))
        goto fail;

    if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) != 0 && !connect_in_progress()) {
        dlog("connect(): error %d", NET_ERRNO());
        goto fail;
    }

    {
        // Wait in short slices so a cancel (source destroyed, settings changed) is honoured quickly.
        int waited = 0;
        for (;;) {
            struct pollfd p;
            p.fd = sock;
            p.events = POLLOUT;
            p.revents = 0;
            int rc = net_poll(&p, 1, 50);
            if (rc < 0)
                goto fail;
            if (rc > 0)
                break;
            waited += 50;
            if (waited >= timeout_ms || (cancel && cancel->load()))
                goto fail;
        }
    }

    if (!net_connect_ok(sock) || !set_nonblock(sock, false))
        goto fail;

    {
        int on = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
    }
    return sock;

fail:
    net_close(sock);
    return INVALID_SOCKET;
}

socket_t net_connect(const char *host, const char *bind_ip, uint16_t port, int timeout_ms,
                     const std::atomic<bool> *cancel)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo *addrs = NULL;
    if (getaddrinfo(host, port_str, &hints, &addrs) != 0 || !addrs) {
        elog("getaddrinfo(%s) failed: error %d", host, NET_ERRNO());
        return INVALID_SOCKET;
    }

    struct sockaddr_storage bind_addr;
    int bind_len = 0;
    memset(&bind_addr, 0, sizeof(bind_addr));
    if (bind_ip && bind_ip[0]) {
        struct addrinfo *b = NULL;
        if (getaddrinfo(bind_ip, NULL, &hints, &b) == 0 && b) {
            memcpy(&bind_addr, b->ai_addr, b->ai_addrlen);
            bind_len = (int)b->ai_addrlen;
        }
        if (b)
            freeaddrinfo(b);
    }

    socket_t sock = INVALID_SOCKET;
    for (const struct addrinfo *ai = addrs; ai; ai = ai->ai_next) {
        if (cancel && cancel->load())
            break;
        sock = connect_one(ai, bind_len ? &bind_addr : NULL, bind_len, timeout_ms, cancel);
        if (sock != INVALID_SOCKET)
            break;
    }

    freeaddrinfo(addrs);
    return sock;
}

ssize_t net_recv(socket_t sock, void *buf, size_t len)
{
    return recv(sock, (char *)buf, (int)len, 0);
}

bool net_send_all(socket_t sock, const void *buf, size_t len)
{
    const char *ptr = (const char *)buf;
    while (len > 0) {
#ifdef _WIN32
        int w = send(sock, ptr, (int)len, 0);
#else
        ssize_t w = send(sock, ptr, len, MSG_NOSIGNAL);
#endif
        if (w <= 0)
            return false;
        len -= (size_t)w;
        ptr += w;
    }
    return true;
}

std::string net_peer_ip(socket_t sock)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(sock, (struct sockaddr *)&ss, &len) != 0)
        return std::string();

    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, buf, sizeof(buf));
    } else if (ss.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}
