// Copyright (C) 2026 Leonardo Berbert
#pragma once

#include <stdint.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util.h"

struct DeviceInfo {
    std::string uuid;  // 32 upper-case hex chars, stable per phone
    std::string name;  // "Samsung SM-S938B"
    std::string ip;    // last address it was seen at
    uint64_t last_seen_ms = 0;
};

/**
 * Finds Niriun phones on the LAN without any helper installed on the computer:
 *  - passive: the phone multicasts its 280-byte hello to 239.255.188.58:4698 every 2 s while no computer is
 *    connected; a listener thread joins the group on every interface.
 *  - active: probe_subnets() connects to TCP 4697 on every host of the local subnets; the phone answers with
 *    the same hello. Outbound TCP only, so it works where multicast or inbound UDP is blocked by a firewall.
 * Sources also feed the table with the hello they receive after connecting (note()).
 */
class Discovery {
public:
    static Discovery &instance();

    /** Starts the beacon listener (idempotent). */
    void start();
    void stop();

    void note(const std::string &uuid, const std::string &name, const std::string &ip);
    std::vector<DeviceInfo> devices();
    bool find(const std::string &uuid, DeviceInfo *out);

    /** Blocks for about one second. Safe to call from any thread, one scan at a time. */
    void probe_subnets();

private:
    Discovery() = default;
    void beacon_loop();

    std::mutex lock_;
    std::map<std::string, DeviceInfo> devices_;

    std::mutex control_;  // serialises start/stop
    std::thread thread_;
    std::atomic<bool> running_{false};
    niriun::Signal stop_signal_;

    std::mutex probe_lock_;
};
