// Copyright (C) 2026 Leonardo Berbert
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
#include "discovery.h"

#include <string.h>

#include <set>

#include "net.h"
#include "niriun_protocol.h"
#include "plugin.h"
#include "sys/ifaces.h"

#ifdef _WIN32
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static std::string ip_to_string(uint32_t host_order_addr)
{
    char buf[INET_ADDRSTRLEN] = {0};
    struct in_addr a;
    a.s_addr = htonl(host_order_addr);
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

Discovery &Discovery::instance()
{
    static Discovery d;
    return d;
}

void Discovery::note(const std::string &uuid, const std::string &name, const std::string &ip)
{
    if (uuid.empty() || ip.empty())
        return;
    std::lock_guard<std::mutex> g(lock_);
    DeviceInfo &d = devices_[uuid];
    d.uuid = uuid;
    d.ip = ip;
    if (!name.empty())
        d.name = name;
    d.last_seen_ms = niriun::now_ms();
}

std::vector<DeviceInfo> Discovery::devices()
{
    std::lock_guard<std::mutex> g(lock_);
    std::vector<DeviceInfo> out;
    for (const auto &kv : devices_)
        out.push_back(kv.second);
    return out;
}

bool Discovery::find(const std::string &uuid, DeviceInfo *out)
{
    std::lock_guard<std::mutex> g(lock_);
    auto it = devices_.find(uuid);
    if (it == devices_.end())
        return false;
    *out = it->second;
    return true;
}

void Discovery::start()
{
    std::lock_guard<std::mutex> g(control_);
    if (running_.exchange(true))
        return;
    stop_signal_.reset();
    thread_ = std::thread([this] { beacon_loop(); });
}

void Discovery::stop()
{
    std::lock_guard<std::mutex> g(control_);
    if (!running_.exchange(false))
        return;
    stop_signal_.set();
    if (thread_.joinable())
        thread_.join();
}

// ---- passive: multicast beacon ---------------------------------------------------------------------------

void Discovery::beacon_loop()
{
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        elog("beacon: socket() failed");
        return;
    }

    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
#if defined(SO_REUSEPORT)
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char *)&on, sizeof(on));
#endif

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(niriun::kBeaconPort);
    if (bind(sock, (const struct sockaddr *)&sa, sizeof(sa)) != 0) {
        elog("beacon: cannot bind UDP %u; discovery by multicast is off (subnet probe and manual IP still work)",
             (unsigned)niriun::kBeaconPort);
        net_close(sock);
        return;
    }

    struct in_addr group;
    inet_pton(AF_INET, niriun::kBeaconGroup, &group);

    std::set<uint32_t> joined;
    uint64_t last_join = 0;
    ilog("beacon: listening on UDP %u", (unsigned)niriun::kBeaconPort);

    while (running_.load()) {
        uint64_t now = niriun::now_ms();
        if (last_join == 0 || now - last_join > 10000) {
            last_join = now;
            for (const Ipv4Iface &ifc : list_ipv4_interfaces()) {
                if (joined.count(ifc.addr))
                    continue;
                struct ip_mreq mreq;
                mreq.imr_multiaddr = group;
                mreq.imr_interface.s_addr = htonl(ifc.addr);
                if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq)) == 0) {
                    joined.insert(ifc.addr);
                    dlog("beacon: joined group on %s", ip_to_string(ifc.addr).c_str());
                }
            }
        }

        int r = net_wait_readable(sock, 500);
        if (r < 0)
            break;
        if (r == 0)
            continue;

        uint8_t buf[512];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = (int)recvfrom(sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n <= 0)
            continue;

        niriun::Hello hello;
        if (niriun::parse_hello(buf, (size_t)n, &hello)) {
            note(hello.uuid_hex(), hello.name, ip_to_string(ntohl(from.sin_addr.s_addr)));
        }
    }

    net_close(sock);
    ilog("beacon: stopped");
}

// ---- active: TCP probe of the local subnets --------------------------------------------------------------

namespace {
struct Probe {
    socket_t sock;
    uint32_t addr;
    bool connected;
    size_t got;
    uint8_t buf[niriun::kHelloSize];
};
}  // namespace

void Discovery::probe_subnets()
{
    std::lock_guard<std::mutex> g(probe_lock_);

    std::set<uint32_t> targets;
    for (const Ipv4Iface &ifc : list_ipv4_interfaces()) {
        // At least the /24 around the interface; never more than that (a /16 would be 65k connects).
        // The interface's own address stays in: a phone-sim or an adb forward on this machine answers there.
        uint32_t mask = ifc.mask | 0xFFFFFF00u;
        uint32_t net = ifc.addr & mask;
        uint32_t hosts = ~mask;
        for (uint32_t h = 1; h < hosts; h++)
            targets.insert(net + h);
    }
    std::vector<Probe> probes;
    probes.reserve(targets.size());
    for (uint32_t addr : targets) {
        socket_t s = net_connect_begin_v4(addr, niriun::kProbePort);
        if (s == INVALID_SOCKET)
            continue;
        Probe p;
        p.sock = s;
        p.addr = addr;
        p.connected = false;
        p.got = 0;
        probes.push_back(p);
    }

    const uint64_t deadline = niriun::now_ms() + 900;
    size_t found = 0;
    while (niriun::now_ms() < deadline) {
        std::vector<struct pollfd> fds;
        std::vector<size_t> index;
        for (size_t i = 0; i < probes.size(); i++) {
            if (probes[i].sock == INVALID_SOCKET)
                continue;
            struct pollfd p;
            p.fd = probes[i].sock;
            p.events = probes[i].connected ? POLLIN : POLLOUT;
            p.revents = 0;
            fds.push_back(p);
            index.push_back(i);
        }
        if (fds.empty())
            break;

        int rc = net_poll(fds.data(), (unsigned)fds.size(), 50);
        if (rc < 0)
            break;
        if (rc == 0)
            continue;

        for (size_t k = 0; k < fds.size(); k++) {
            if (fds[k].revents == 0)
                continue;
            Probe &p = probes[index[k]];
            if (!p.connected) {
                if (net_connect_ok(p.sock)) {
                    p.connected = true;
                } else {
                    net_close(p.sock);
                    p.sock = INVALID_SOCKET;
                }
                continue;
            }

            ssize_t n = net_recv(p.sock, p.buf + p.got, sizeof(p.buf) - p.got);
            if (n > 0)
                p.got += (size_t)n;
            if (p.got >= sizeof(p.buf)) {
                niriun::Hello hello;
                if (niriun::parse_hello(p.buf, p.got, &hello)) {
                    note(hello.uuid_hex(), hello.name, ip_to_string(p.addr));
                    found++;
                }
                net_close(p.sock);
                p.sock = INVALID_SOCKET;
            } else if (n <= 0) {
                net_close(p.sock);
                p.sock = INVALID_SOCKET;
            }
        }
    }

    for (Probe &p : probes)
        net_close(p.sock);
    ilog("probe: %zu host(s) answered on TCP %u", found, (unsigned)niriun::kProbePort);
}
