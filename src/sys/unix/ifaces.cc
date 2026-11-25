// Copyright (C) 2026 Leonardo Berbert
#include "sys/ifaces.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

std::vector<Ipv4Iface> list_ipv4_interfaces()
{
    std::vector<Ipv4Iface> out;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0)
        return out;

    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_netmask || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
        uint32_t addr = ntohl(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr);
        uint32_t mask = ntohl(((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr);
        if ((addr >> 16) == 0xA9FE)
            continue;
        out.push_back({addr, mask});
    }
    freeifaddrs(list);
    return out;
}
