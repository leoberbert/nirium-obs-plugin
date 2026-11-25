// Copyright (C) 2026 Leonardo Berbert
#pragma once

#include <stdint.h>

#include <vector>

struct Ipv4Iface {
    uint32_t addr;  // host byte order
    uint32_t mask;  // host byte order
};

/** Up, non-loopback, non-link-local IPv4 interface addresses. */
std::vector<Ipv4Iface> list_ipv4_interfaces();
