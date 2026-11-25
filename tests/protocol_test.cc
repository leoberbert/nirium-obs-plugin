// Unit tests for src/niriun_protocol.h (no OBS needed). Run with ctest or directly.
#include "niriun_protocol.h"

#include <stdio.h>
#include <stdlib.h>

#include <random>

using namespace niriun;

static int failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

static std::vector<uint8_t> make_packet(MsgType type, const std::vector<uint8_t> &body)
{
    std::vector<uint8_t> p(4 + body.size());
    wr32(p.data(), make_header(type, p.size()));
    if (!body.empty())
        memcpy(p.data() + 4, body.data(), body.size());
    return p;
}

// Mirrors PacketBuilder.hello in the Android app.
static std::vector<uint8_t> make_hello(const char *name, uint8_t fill)
{
    std::vector<uint8_t> p(kHelloSize, 0);
    wr32(p.data(), make_header(kMsgHello, kHelloSize));
    for (int i = 0; i < 16; i++)
        p[4 + i] = (uint8_t)(fill + i);
    p[20] = 2;
    p[22] = 2;
    memcpy(p.data() + 24, name, strlen(name));
    return p;
}

static void test_header()
{
    // Header written by the Android app for a 280-byte hello is 0x01000118.
    uint8_t b[4];
    wr32(b, make_header(kMsgHello, kHelloSize));
    CHECK(rd32(b) == 0x01000118u);
    CHECK(b[0] == 0x18 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x01);
}

static void test_hello()
{
    auto p = make_hello("Samsung SM-S938B", 0xA0);
    Hello h;
    CHECK(parse_hello(p.data(), p.size(), &h));
    CHECK(h.kind == 2 && h.version == 2);
    CHECK(h.name == "Samsung SM-S938B");
    CHECK(h.uuid_hex() == "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF");

    // Wrong type and short packets are rejected, a name filling the whole field is not over-read.
    auto bad = p;
    bad[3] = kMsgSizes;
    CHECK(!parse_hello(bad.data(), bad.size(), &h));
    CHECK(!parse_hello(p.data(), kHelloSize - 1, &h));
    std::vector<uint8_t> full = p;
    memset(full.data() + 24, 'x', kNameField);
    CHECK(parse_hello(full.data(), full.size(), &h));
    CHECK(h.name.size() == kNameField);
}

static void test_sizes_and_cameras()
{
    std::vector<uint8_t> body(8 + 2 * 12);
    wr32(body.data(), 1);      // flags: microphone
    wr32(body.data() + 4, 2);  // n
    wr32(body.data() + 8, 640);
    wr32(body.data() + 12, 480);
    wrf32(body.data() + 16, 30.f);
    wr32(body.data() + 20, 1920);
    wr32(body.data() + 24, 1080);
    wrf32(body.data() + 28, 60.f);
    auto p = make_packet(kMsgSizes, body);
    Sizes s;
    CHECK(parse_sizes(p.data(), p.size(), &s));
    CHECK(s.microphone());
    CHECK(s.modes.size() == 2);
    CHECK(s.modes[0].width == 640 && s.modes[0].height == 480 && s.modes[0].fps == 30.f);
    CHECK(s.modes[1].width == 1920 && s.modes[1].fps == 60.f);

    // Announced count larger than the data must fail instead of reading past the packet.
    wr32(p.data() + 8, 3);
    CHECK(!parse_sizes(p.data(), p.size(), &s));

    std::vector<uint8_t> cb(8 + 2 * kNameField, 0);
    wr32(cb.data(), 2);      // n
    wr32(cb.data() + 4, 1);  // selected
    memcpy(cb.data() + 8, "Back", 4);
    memcpy(cb.data() + 8 + kNameField, "Front", 5);
    auto c = make_packet(kMsgCameras, cb);
    Cameras cams;
    CHECK(parse_cameras(c.data(), c.size(), &cams));
    CHECK(cams.selected == 1);
    CHECK(cams.names.size() == 2 && cams.names[0] == "Back" && cams.names[1] == "Front");
    CHECK(!parse_cameras(c.data(), c.size() - 1, &cams));
}

static void test_media_packets()
{
    std::vector<uint8_t> body(4 + 5);
    wrf32(body.data(), 1.5f);
    for (int i = 0; i < 5; i++)
        body[4 + i] = (uint8_t)(0x10 + i);
    auto p = make_packet(kMsgVideo, body);
    VideoView v;
    CHECK(parse_video(p.data(), p.size(), &v));
    CHECK(v.seconds == 1.5f);
    CHECK(v.size == 5 && v.payload[0] == 0x10);
    CHECK(rd32(p.data() + 4) == v.ts_bits);

    // Header + timestamp only is not a video frame.
    auto empty = make_packet(kMsgVideo, std::vector<uint8_t>(4));
    CHECK(!parse_video(empty.data(), empty.size(), &v));

    std::vector<uint8_t> ab(4 + 3, 0xF8);
    auto a = make_packet(kMsgAudio, ab);
    const uint8_t *payload = nullptr;
    size_t n = 0;
    CHECK(parse_audio(a.data(), a.size(), &payload, &n));
    CHECK(n == 3 && payload[0] == 0xF8);
}

static void test_builders()
{
    uint8_t b[kTallySize];

    size_t n = build_start(b, true, kCodecHevc, 1920, 1080, 60.f);
    CHECK(n == 24 && rd32(b) == (24u | 0x0B000000u));
    CHECK(rd32(b + 4) == 1 && rd32(b + 8) == 2 && rd32(b + 12) == 1920 && rd32(b + 16) == 1080);
    CHECK(rdf32(b + 20) == 60.f);
    build_start(b, false, kCodecH264, 640, 480, 30.f);
    CHECK(rd32(b + 4) == 0 && rd32(b + 8) == 1);

    // The ACK must echo the exact f32 bits of the video packet.
    n = build_ack(b, 0x3FC00000u, 12, 0);
    CHECK(n == 16 && rd32(b) == (16u | 0x09000000u));
    CHECK(rd32(b + 4) == 0x3FC00000u && rdf32(b + 4) == 1.5f && rd32(b + 8) == 12);

    n = build_control(b, kCtlZoom, 50);
    CHECK(n == 12 && rd32(b) == (12u | 0x10000000u) && rd32(b + 4) == 4 && rd32(b + 8) == 50);

    n = build_noop(b);
    CHECK(n == 4 && rd32(b) == (4u | 0x01000000u));

    n = build_tally(b, kTallyProgram, "Camera 1");
    CHECK(n == 264 && rd32(b) == (264u | 0x30000000u));
    CHECK(rd32(b + 4) == 2);
    CHECK(read_name(b + 8, kNameField) == "Camera 1");

    n = build_tally(b, kTallyOff, nullptr);
    CHECK(rd32(b + 4) == 0 && b[8] == 0);

    // A name longer than the field is cut at 255 bytes without splitting a UTF-8 sequence.
    std::string longname(254, 'a');
    longname += "\xC3\xA9";  // 'é' would straddle byte 255
    build_tally(b, kTallyPreview, longname.c_str());
    std::string got = read_name(b + 8, kNameField);
    CHECK(got.size() == 254);
    CHECK(b[8 + 255] == 0);
}

// Feeds `stream` to a Framer in chunks of `chunk` bytes (0 = random sizes) and collects every packet.
static std::vector<std::vector<uint8_t>> frame_all(const std::vector<uint8_t> &stream, size_t chunk, bool *error)
{
    Framer f;
    std::vector<std::vector<uint8_t>> out;
    std::mt19937 rng(7);
    size_t pos = 0;
    *error = false;
    while (pos < stream.size()) {
        size_t need;
        uint8_t *dst = f.want(&need);
        size_t n = chunk ? chunk : 1 + rng() % 9000;
        if (n > need)
            n = need;
        if (n > stream.size() - pos)
            n = stream.size() - pos;
        memcpy(dst, stream.data() + pos, n);
        pos += n;
        int r = f.commit(n);
        if (r < 0) {
            *error = true;
            return out;
        }
        if (r == 1) {
            size_t len = f.size();
            std::vector<uint8_t> pkt = f.take();
            // Padding after the packet must be zero for FFmpeg.
            for (size_t i = 0; i < kPadding; i++)
                CHECK(pkt[len + i] == 0);
            pkt.resize(len);
            out.push_back(std::move(pkt));
        }
    }
    return out;
}

static void test_framer()
{
    std::vector<std::vector<uint8_t>> packets;
    packets.push_back(make_hello("A", 1));
    packets.push_back(make_packet(kMsgHello, {}));  // header-only no-op, length 4
    std::vector<uint8_t> big(300000);
    for (size_t i = 0; i < big.size(); i++)
        big[i] = (uint8_t)(i * 31);
    packets.push_back(make_packet(kMsgVideo, big));
    packets.push_back(make_packet(kMsgAudio, std::vector<uint8_t>(4 + 100, 0xAA)));
    packets.push_back(make_packet(kMsgHello, {}));

    std::vector<uint8_t> stream;
    for (auto &p : packets)
        stream.insert(stream.end(), p.begin(), p.end());

    for (size_t chunk : {(size_t)1, (size_t)3, (size_t)7, (size_t)1500, (size_t)0}) {
        bool error;
        auto got = frame_all(stream, chunk, &error);
        CHECK(!error);
        CHECK(got.size() == packets.size());
        for (size_t i = 0; i < got.size() && i < packets.size(); i++)
            CHECK(got[i] == packets[i]);
    }

    // Length below the header size and above the limit are protocol errors.
    bool error;
    std::vector<uint8_t> bad(4);
    wr32(bad.data(), 0x08000003u);
    frame_all(bad, 1, &error);
    CHECK(error);
    wr32(bad.data(), 0x08FFFFFFu);
    frame_all(bad, 4, &error);
    CHECK(error);
}

int main()
{
    test_header();
    test_hello();
    test_sizes_and_cameras();
    test_media_packets();
    test_builders();
    test_framer();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("protocol tests passed\n");
    return 0;
}
