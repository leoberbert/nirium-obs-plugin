// Copyright (C) 2026 Leonardo Berbert
#pragma once

#include <stdint.h>

#include <mutex>
#include <string>
#include <vector>

struct AdbDevice {
    std::string serial;
    std::string state;  // "device", "offline", "unauthorized"...
    std::string model;  // from `adb devices -l`, may be empty
    bool online() const { return state == "device"; }
};

/**
 * USB support through `adb forward`. adb is optional: it is looked up in PATH, the Android SDK
 * (ANDROID_HOME / ANDROID_SDK_ROOT / default SDK folders) and the plugin's data folder. Without it the USB
 * entries simply do not exist; Wi-Fi works without any helper.
 */
class Adb {
public:
    static Adb &instance();

    /** Runs `adb devices -l`. Returns an empty list when adb is not available. */
    std::vector<AdbDevice> devices();
    bool available();

    /** `adb -s <serial> forward tcp:0 tcp:<remote>`. @return the local port, 0 on failure. */
    int forward(const std::string &serial, uint16_t remote_port);
    void remove_forward(int local_port);

private:
    Adb() = default;
    void locate();
    bool run(const std::string &serial, const std::vector<std::string> &args, std::string *output);

    std::mutex lock_;
    bool located_ = false;
    std::string exe_;
};
