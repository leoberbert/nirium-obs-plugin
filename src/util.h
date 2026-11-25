// Copyright (C) 2026 Leonardo Berbert
#pragma once

#include <stdint.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace niriun {

inline uint64_t now_ms()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** Manual-reset event: set() wakes every waiter and stays set until reset(). */
class Signal {
public:
    void set()
    {
        {
            std::lock_guard<std::mutex> g(m_);
            set_ = true;
        }
        cv_.notify_all();
    }

    void reset()
    {
        std::lock_guard<std::mutex> g(m_);
        set_ = false;
    }

    bool is_set()
    {
        std::lock_guard<std::mutex> g(m_);
        return set_;
    }

    /** @return true if the signal is set (before or during the wait), false on timeout. */
    bool wait_ms(unsigned ms)
    {
        std::unique_lock<std::mutex> l(m_);
        return cv_.wait_for(l, std::chrono::milliseconds(ms), [&] { return set_; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool set_ = false;
};

}  // namespace niriun
