// Copyright (C) 2026 Leonardo Berbert
#include "sys/ifaces.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

std::vector<Ipv4Iface> list_ipv4_interfaces()
{
    std::vector<Ipv4Iface> out;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; attempt++) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, NULL, (PIP_ADAPTER_ADDRESSES)buf.data(), &size);
    }
    if (rc != NO_ERROR)
        return out;

    for (PIP_ADAPTER_ADDRESSES aa = (PIP_ADAPTER_ADDRESSES)buf.data(); aa; aa = aa->Next) {
        if (aa->OperStatus != IfOperStatusUp || aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK || aa->IfType == IF_TYPE_TUNNEL)
            continue;
        for (PIP_ADAPTER_UNICAST_ADDRESS ua = aa->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            uint32_t addr = ntohl(((sockaddr_in *)ua->Address.lpSockaddr)->sin_addr.s_addr);
            if ((addr >> 24) == 127 || (addr >> 16) == 0xA9FE)
                continue;
            unsigned prefix = ua->OnLinkPrefixLength;
            if (prefix == 0 || prefix > 32)
                prefix = 24;
            uint32_t mask = prefix == 32 ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - prefix));
            out.push_back({addr, mask});
        }
    }
    return out;
}
