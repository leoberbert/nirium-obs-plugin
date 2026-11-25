// Niriun wire protocol, PC side (client). Layout and evidence: docs/PROTOCOL.md of github.com/leoberbert/Niriun.
//
// Pure C++17, no OBS or FFmpeg dependency, so it can be unit tested on its own (tests/protocol_test.cc).
//
// Copyright (C) 2026 Leonardo Berbert
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

namespace niriun {

constexpr uint16_t kTcpPort = 4699;
constexpr uint16_t kProbePort = 4697;
constexpr uint16_t kBeaconPort = 4698;
constexpr const char *kBeaconGroup = "239.255.188.58";

constexpr size_t kHeaderSize = 4;
constexpr size_t kNameField = 256;
constexpr size_t kHelloSize = 0x118;
/** The phone sends at most 8 + 4 999 992 bytes in one packet. */
constexpr size_t kMaxPacket = 5u * 1000 * 1000 + 64;
/** Zero bytes kept after every received packet (FFmpeg needs AV_INPUT_BUFFER_PADDING_SIZE == 64). */
constexpr size_t kPadding = 64;

enum MsgType : uint8_t {
    kMsgHello = 0x01,
    kMsgAudio = 0x07,
    kMsgVideo = 0x08,
    kMsgAck = 0x09,
    kMsgSizes = 0x0A,
    kMsgStart = 0x0B,
    kMsgControl = 0x10,
    kMsgPeerInfo = 0x11,
    kMsgCameras = 0x18,
    kMsgCameraUsage = 0x1A,
    /** Niriun extension (PC -> phone): tally state of the OBS source. Not part of the Iriun protocol. */
    kMsgTally = 0x30,
    /** Niriun extension (phone -> PC): sent before a deliberate stop; what the source should show afterwards. */
    kMsgStopScreen = 0x31,
};

enum StopScreen : uint32_t {
    kStopClear = 0,    // transparent: the source simply has no picture
    kStopBlack = 1,
    kStopOffline = 2,  // a card that says the camera is offline
};

enum Codec : uint32_t {
    kCodecH264 = 1,
    kCodecHevc = 2,
};

enum ControlId : uint32_t {
    kCtlLockExposureFocus = 1,
    kCtlTorch = 2,
    kCtlMirror = 3,
    kCtlZoom = 4,
    kCtlNextCamera = 5,
    kCtlSelectCamera = 14,
};

enum TallyState : uint32_t {
    kTallyOff = 0,
    kTallyPreview = 1,
    kTallyProgram = 2,
};

inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

inline float rdf32(const uint8_t *p)
{
    uint32_t bits = rd32(p);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

inline void wrf32(uint8_t *p, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    wr32(p, bits);
}

inline uint32_t make_header(MsgType type, size_t total)
{
    return (uint32_t)(total & 0xFFFFFF) | ((uint32_t)type << 24);
}

/** Reads a NUL-padded name field of at most `max` bytes. */
inline std::string read_name(const uint8_t *p, size_t max)
{
    size_t n = 0;
    while (n < max && p[n] != 0)
        n++;
    return std::string((const char *)p, n);
}

// ---- phone -> PC ----------------------------------------------------------------------------------------

struct Hello {
    uint8_t uuid[16] = {0};
    uint16_t kind = 0;
    uint16_t version = 0;
    std::string name;

    /** 32 upper-case hex chars: the NSD service name and the id used in the device list. */
    std::string uuid_hex() const
    {
        static const char *digits = "0123456789ABCDEF";
        std::string s;
        s.reserve(32);
        for (uint8_t b : uuid) {
            s.push_back(digits[b >> 4]);
            s.push_back(digits[b & 15]);
        }
        return s;
    }
};

/** `pkt` is the whole packet including the header. Also used for the UDP beacon (identical 280 bytes). */
inline bool parse_hello(const uint8_t *pkt, size_t len, Hello *out)
{
    if (len < kHelloSize || pkt[3] != kMsgHello)
        return false;
    memcpy(out->uuid, pkt + 4, 16);
    out->kind = (uint16_t)(pkt[20] | (pkt[21] << 8));
    out->version = (uint16_t)(pkt[22] | (pkt[23] << 8));
    out->name = read_name(pkt + 24, kNameField);
    return true;
}

struct VideoMode {
    int32_t width;
    int32_t height;
    float fps;
};

struct Sizes {
    uint32_t flags = 0;
    std::vector<VideoMode> modes;
    bool microphone() const { return (flags & 1) != 0; }
};

inline bool parse_sizes(const uint8_t *pkt, size_t len, Sizes *out)
{
    if (len < 12 || pkt[3] != kMsgSizes)
        return false;
    uint32_t n = rd32(pkt + 8);
    if (n > 1024 || len < 12 + (size_t)n * 12)
        return false;
    out->flags = rd32(pkt + 4);
    out->modes.clear();
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *m = pkt + 12 + i * 12;
        out->modes.push_back({(int32_t)rd32(m), (int32_t)rd32(m + 4), rdf32(m + 8)});
    }
    return true;
}

struct Cameras {
    uint32_t selected = 0;
    std::vector<std::string> names;
};

inline bool parse_cameras(const uint8_t *pkt, size_t len, Cameras *out)
{
    if (len < 12 || pkt[3] != kMsgCameras)
        return false;
    uint32_t n = rd32(pkt + 4);
    if (n > 64 || len < 12 + (size_t)n * kNameField)
        return false;
    out->selected = rd32(pkt + 8);
    out->names.clear();
    for (uint32_t i = 0; i < n; i++)
        out->names.push_back(read_name(pkt + 12 + i * kNameField, kNameField));
    return true;
}

/** Video packet: f32 seconds since START at offset 4, Annex-B payload from offset 8. */
struct VideoView {
    uint32_t ts_bits;
    float seconds;
    const uint8_t *payload;
    size_t size;
};

inline bool parse_video(const uint8_t *pkt, size_t len, VideoView *out)
{
    if (len <= 8 || pkt[3] != kMsgVideo)
        return false;
    out->ts_bits = rd32(pkt + 4);
    out->seconds = rdf32(pkt + 4);
    out->payload = pkt + 8;
    out->size = len - 8;
    return true;
}

/** 0x31 from the phone: u32 mode at offset 4. Unknown modes are treated as "clear". */
inline bool parse_stop_screen(const uint8_t *pkt, size_t len, StopScreen *mode)
{
    if (len < 8 || pkt[3] != kMsgStopScreen)
        return false;
    uint32_t m = rd32(pkt + 4);
    *mode = m == kStopBlack ? kStopBlack : m == kStopOffline ? kStopOffline : kStopClear;
    return true;
}

/** Audio packet: raw Opus (48 kHz mono, 20 ms) from offset 8. */
inline bool parse_audio(const uint8_t *pkt, size_t len, const uint8_t **payload, size_t *size)
{
    if (len <= 8 || pkt[3] != kMsgAudio)
        return false;
    *payload = pkt + 8;
    *size = len - 8;
    return true;
}

// ---- PC -> phone ----------------------------------------------------------------------------------------

constexpr size_t kStartSize = 24;
constexpr size_t kAckSize = 16;
constexpr size_t kControlSize = 12;
constexpr size_t kTallySize = 8 + kNameField;
constexpr size_t kNoopSize = 4;

inline size_t build_start(uint8_t *out, bool audio, Codec codec, int width, int height, float fps)
{
    wr32(out, make_header(kMsgStart, kStartSize));
    wr32(out + 4, audio ? 1 : 0);
    wr32(out + 8, codec);
    wr32(out + 12, (uint32_t)width);
    wr32(out + 16, (uint32_t)height);
    wrf32(out + 20, fps);
    return kStartSize;
}

/** `ts_bits` is the raw f32 of the video packet being confirmed; the phone compares it as a float. */
inline size_t build_ack(uint8_t *out, uint32_t ts_bits, uint32_t decode_ms, uint32_t post_ms)
{
    wr32(out, make_header(kMsgAck, kAckSize));
    wr32(out + 4, ts_bits);
    wr32(out + 8, decode_ms);
    wr32(out + 12, post_ms);
    return kAckSize;
}

inline size_t build_control(uint8_t *out, ControlId id, int32_t raw)
{
    wr32(out, make_header(kMsgControl, kControlSize));
    wr32(out + 4, id);
    wr32(out + 8, (uint32_t)raw);
    return kControlSize;
}

/** `name` is the OBS source name, truncated to 255 bytes without splitting a UTF-8 sequence. */
inline size_t build_tally(uint8_t *out, TallyState state, const char *name)
{
    memset(out, 0, kTallySize);
    wr32(out, make_header(kMsgTally, kTallySize));
    wr32(out + 4, state);
    size_t n = name ? strlen(name) : 0;
    if (n > kNameField - 1) {
        n = kNameField - 1;
        while (n > 0 && ((uint8_t)name[n] & 0xC0) == 0x80)
            n--;
    }
    if (n)
        memcpy(out + 8, name, n);
    return kTallySize;
}

/** Header-only no-op (type 0x01, length 4): keeps the phone's 10 s idle timer from firing. */
inline size_t build_noop(uint8_t *out)
{
    wr32(out, make_header(kMsgHello, kNoopSize));
    return kNoopSize;
}

// ---- framing --------------------------------------------------------------------------------------------

/**
 * Incremental reader. The caller reads from the socket straight into `want()` and reports the byte count with
 * `commit()`, so a packet is never copied. A packet is complete when `commit()` returns 1; `take()` hands the
 * buffer (header included, `kPadding` zero bytes after the end) to the caller and installs `spare` as the next
 * receive buffer.
 */
class Framer {
public:
    /** Where the next bytes go and how many are still needed for the current header or packet. */
    uint8_t *want(size_t *n)
    {
        if (complete_)
            restart();
        ensure(need_ + kPadding);
        *n = need_ - filled_;
        return buf_.data() + filled_;
    }

    /** @return 1 packet complete, 0 need more, -1 invalid length (drop the connection). */
    int commit(size_t n)
    {
        filled_ += n;
        if (in_header_) {
            if (filled_ < kHeaderSize)
                return 0;
            size_t len = rd32(buf_.data()) & 0xFFFFFF;
            if (len < kHeaderSize || len > kMaxPacket)
                return -1;
            in_header_ = false;
            need_ = len;
        }
        if (filled_ < need_)
            return 0;
        complete_ = true;
        return 1;
    }

    const uint8_t *data() const { return buf_.data(); }
    /** Bytes of the completed packet, header included. */
    size_t size() const { return filled_; }
    MsgType type() const { return (MsgType)buf_[3]; }

    /** Detaches the completed packet (read `size()` first) and installs `spare` as the next receive buffer. */
    std::vector<uint8_t> take(std::vector<uint8_t> spare = std::vector<uint8_t>())
    {
        memset(buf_.data() + filled_, 0, kPadding);
        std::vector<uint8_t> out = std::move(buf_);
        buf_ = std::move(spare);
        restart();
        return out;
    }

private:
    void restart()
    {
        filled_ = 0;
        need_ = kHeaderSize;
        in_header_ = true;
        complete_ = false;
    }

    void ensure(size_t size)
    {
        if (buf_.size() < size)
            buf_.resize(size < 4096 ? 4096 : size);
    }

    std::vector<uint8_t> buf_;
    size_t filled_ = 0;
    size_t need_ = kHeaderSize;
    bool in_header_ = true;
    bool complete_ = false;
};

}  // namespace niriun
