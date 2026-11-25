// Copyright (C) 2021 DEV47APPS, github.com/dev47apps
// Copyright (C) 2026 Leonardo Berbert (rewritten for the Niriun protocol)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#ifndef _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif
#else
#include <poll.h>
typedef int socket_t;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#endif

bool net_init(void);
void net_cleanup(void);
void net_close(socket_t sock);

/**
 * Connects to host:port, trying every resolved address. `bind_ip` (optional) is the local address to bind to
 * (OBS "Network > Bind to IP"). Gives up after `timeout_ms` per address, or as soon as `*cancel` becomes true.
 * The returned socket is blocking, with TCP_NODELAY set.
 */
socket_t net_connect(const char *host, const char *bind_ip, uint16_t port, int timeout_ms, const std::atomic<bool> *cancel);

/** Starts a non-blocking connect to an IPv4 address (host byte order). Used by the subnet probe. */
socket_t net_connect_begin_v4(uint32_t addr, uint16_t port);
/** After the socket polled writable: true when the non-blocking connect succeeded. */
bool net_connect_ok(socket_t sock);

bool set_nonblock(socket_t sock, bool nonblock);
int set_recv_buf_len(socket_t sock, int len);
int set_send_timeout(socket_t sock, int ms);

/** @return 1 readable (or closed/error: recv reports which), 0 timeout, -1 poll failure. */
int net_wait_readable(socket_t sock, int timeout_ms);
int net_poll(struct pollfd *fds, unsigned count, int timeout_ms);

/** recv(): bytes, 0 on orderly close, -1 on error. */
ssize_t net_recv(socket_t sock, void *buf, size_t len);
/** Sends everything or fails. */
bool net_send_all(socket_t sock, const void *buf, size_t len);

/** Remote address of a connected socket as text ("" on failure). */
std::string net_peer_ip(socket_t sock);
