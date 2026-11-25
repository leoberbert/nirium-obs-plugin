/*
Copyright (C) 2022 DEV47APPS, github.com/dev47apps
Copyright (C) 2026 Leonardo Berbert (rewritten for the Niriun protocol: PC client, Opus, tally, camera controls)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "source.h"

#include <util/platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "adb.h"
#include "discovery.h"
#include "ffmpeg_decode.h"
#include "net.h"
#include "niriun_protocol.h"
#include "plugin.h"
#include "util.h"

// ---- settings keys ---------------------------------------------------------------------------------------

#define OPT_DEVICE_LIST "device_list"
#define OPT_WIFI_IP "wifi_ip"
#define OPT_APP_PORT "app_port"
#define OPT_RESOLUTION "resolution_str"
#define OPT_FPS "fps"
#define OPT_VIDEO_FORMAT "video_format"
#define OPT_IS_ACTIVATED "activated"
#define OPT_ENABLE_AUDIO "enable_audio"
#define OPT_HW_ACCEL "allow_hw_accel"
#define OPT_UNBUFFERED "unbuffered_output"
#define OPT_DEACTIVATE_WNS "deactivate_wns"
#define OPT_SEND_TALLY "send_tally"
#define OPT_COLOR_MATRIX "color_matrix"
#define OPT_ZOOM "zoom"
#define OPT_CAMERA "camera"
#define OPT_ACTIVE_DEV_ID "cur_dev_id"
#define OPT_ACTIVE_DEV_IP "cur_dev_ip"
#define OPT_ACTIVE_DEV_NAME "cur_dev_name"
#define OPT_ACTIVE_DEV_TYPE "cur_dev_type"

#define DEV_MANUAL "wifi_manual"
#define DEV_PREFIX_UUID "uuid:"
#define DEV_PREFIX_ADB "adb:"

#define LOC(key) obs_module_text(key)

namespace {

constexpr int kConnectTimeoutMs = 2000;
constexpr uint64_t kInboundTimeoutMs = 15000;
constexpr uint64_t kKeepAliveMs = 3000;
constexpr uint64_t kProbeCooldownMs = 8000;
constexpr size_t kMaxQueuedVideo = 16;
constexpr int kRetryDelayMs = 2000;

const char *const kResolutions[] = {"640x360", "640x480", "1280x720", "1920x1080", "2560x1440", "3840x2160"};
const int kFrameRates[] = {24, 30, 60};

enum class DeviceType : int { NONE = 0, WIFI_MANUAL = 1, DISCOVERED = 2, ADB = 3 };

struct Target {
    DeviceType type = DeviceType::NONE;
    std::string id;    // uuid (DISCOVERED), serial (ADB), unused (WIFI_MANUAL)
    std::string ip;    // WIFI_MANUAL address, or the last address a DISCOVERED phone was reached at
    std::string name;  // for the device list only
    int port = niriun::kTcpPort;
};

/** Everything the phone is told in START; a change forces a reconnect. */
struct StartParams {
    bool audio = false;
    bool hevc = false;
    bool hw = true;
    int width = 1920;
    int height = 1080;
    int fps = 30;

    bool operator==(const StartParams &o) const
    {
        return audio == o.audio && hevc == o.hevc && hw == o.hw && width == o.width && height == o.height &&
               fps == o.fps;
    }
    bool operator!=(const StartParams &o) const { return !(*this == o); }
};

/** What the properties dialog shows about the live connection. */
struct Info {
    bool connected = false;
    bool streaming = false;
    std::string phone;
    std::string stream;  // "1920x1080 · H.264 · GPU"
    std::string error;
    std::vector<std::string> cameras;
    uint32_t camera = 0;
    bool microphone = false;
};

struct VideoJob {
    std::vector<uint8_t> buf;  // whole packet; the Annex-B payload starts at offset 8
    size_t len = 0;
    uint32_t ts_bits = 0;
};

struct NiriunSource;

/** One TCP connection to the phone: the socket, the video decode thread and the decoders. */
struct Session {
    Session(NiriunSource *s, socket_t sk, const StartParams &p) : src(s), sock(sk), params(p)
    {
        memset(&vframe, 0, sizeof(vframe));
        memset(&aframe, 0, sizeof(aframe));
    }

    NiriunSource *src;
    socket_t sock;
    StartParams params;
    std::atomic<bool> alive{true};
    std::atomic<uint64_t> last_send_ms{0};
    std::mutex send_lock;

    std::mutex q_lock;
    std::condition_variable q_cv;
    std::deque<VideoJob> queue;
    std::vector<std::vector<uint8_t>> pool;
    std::thread decode_thread;

    FFmpegDecoder video;
    FFmpegDecoder audio;
    bool audio_ready = false;
    obs_source_frame2 vframe;
    obs_source_audio aframe;
    int shown_w = 0, shown_h = 0;
    niriun::StopScreen stop_mode = niriun::kStopClear;  // set by the phone just before a deliberate Stop

    // network-thread state
    bool got_hello = false;
    bool started = false;
    niriun::Sizes sizes;
    int last_tally = -1;
    uint64_t last_rx_ms = 0;

    bool send(const uint8_t *data, size_t len);
    std::vector<uint8_t> spare();
    void recycle(std::vector<uint8_t> &&buf);
    bool push_video(std::vector<uint8_t> &&buf, size_t len, uint32_t ts_bits);
    void start_decoding();
    void stop();
    void decode_loop();
};

struct NiriunSource {
    obs_source_t *source = nullptr;

    std::atomic<bool> activated{false};
    std::atomic<bool> showing{false};
    std::atomic<bool> deactivate_wns{false};
    std::atomic<bool> enable_audio{true};
    std::atomic<bool> use_hw{true};
    std::atomic<bool> send_tally{true};
    std::atomic<bool> hevc{false};
    std::atomic<int> width{1920}, height{1080}, fps{30};
    std::atomic<int> color_matrix{0};
    std::atomic<int> zoom{0};
    std::atomic<bool> on_program{false};
    std::atomic<bool> on_preview{false};

    std::mutex cfg_lock;
    Target target;
    std::string last_error;

    std::mutex info_lock;
    Info info;

    std::mutex cmd_lock;
    std::deque<std::pair<niriun::ControlId, int32_t>> cmds;

    std::atomic<bool> stopping{false};
    niriun::Signal wake;
    std::thread net_thread;

    int adb_port = 0;  // local end of `adb forward`, net thread only
    std::string adb_serial;
    uint64_t last_probe_ms = 0;

    StartParams params() const
    {
        StartParams p;
        p.audio = enable_audio.load();
        p.hevc = hevc.load();
        p.hw = use_hw.load();
        p.width = width.load();
        p.height = height.load();
        p.fps = fps.load();
        return p;
    }

    bool should_connect()
    {
        if (!activated.load() || (deactivate_wns.load() && !showing.load()))
            return false;
        std::lock_guard<std::mutex> g(cfg_lock);
        return target.type != DeviceType::NONE;
    }

    niriun::TallyState tally() const
    {
        if (on_program.load())
            return niriun::kTallyProgram;
        if (on_preview.load())
            return niriun::kTallyPreview;
        return niriun::kTallyOff;
    }

    void post_control(niriun::ControlId id, int32_t value)
    {
        {
            std::lock_guard<std::mutex> g(cmd_lock);
            cmds.emplace_back(id, value);
        }
        wake.set();
    }

    void set_error(const std::string &e)
    {
        std::lock_guard<std::mutex> g(cfg_lock);
        last_error = e;
    }

    void run();
    socket_t connect_target(Target &t);
    void run_session(socket_t sock, const StartParams &sp);
    void show_stop_frame(niriun::StopScreen mode, int w, int h);
    std::vector<uint8_t> stop_buf;
    bool stop_card_shown = false;  // network thread only
    bool handle_packet(Session &s, niriun::Framer &framer, const std::string &peer);
    void flush_outgoing(Session &s);
    void publish(const std::function<void(Info &)> &fn)
    {
        std::lock_guard<std::mutex> g(info_lock);
        fn(info);
    }
};

// ---- adb device cache ------------------------------------------------------------------------------------

std::mutex g_adb_lock;
std::vector<AdbDevice> g_adb_devices;
std::thread g_warmup;
std::once_flag g_started;

void refresh_adb_cache()
{
    std::vector<AdbDevice> devs = Adb::instance().devices();
    std::lock_guard<std::mutex> g(g_adb_lock);
    g_adb_devices = std::move(devs);
}

std::vector<AdbDevice> adb_cache()
{
    std::lock_guard<std::mutex> g(g_adb_lock);
    return g_adb_devices;
}

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parse_resolution(const char *s, int *w, int *h)
{
    int cx = 0, cy = 0;
    if (!s || sscanf(s, "%dx%d", &cx, &cy) != 2)
        return false;
    if (cx < 160 || cy < 120 || cx > 7680 || cy > 4320)
        return false;
    *w = cx;
    *h = cy;
    return true;
}

// ---- Session ---------------------------------------------------------------------------------------------

bool Session::send(const uint8_t *data, size_t len)
{
    if (!alive.load())
        return false;
    std::lock_guard<std::mutex> g(send_lock);
    if (!net_send_all(sock, data, len)) {
        alive = false;
        return false;
    }
    last_send_ms = niriun::now_ms();
    return true;
}

std::vector<uint8_t> Session::spare()
{
    std::lock_guard<std::mutex> g(q_lock);
    if (pool.empty())
        return std::vector<uint8_t>();
    std::vector<uint8_t> b = std::move(pool.back());
    pool.pop_back();
    return b;
}

void Session::recycle(std::vector<uint8_t> &&buf)
{
    std::lock_guard<std::mutex> g(q_lock);
    if (pool.size() < kMaxQueuedVideo)
        pool.push_back(std::move(buf));
}

bool Session::push_video(std::vector<uint8_t> &&buf, size_t len, uint32_t ts_bits)
{
    {
        std::lock_guard<std::mutex> g(q_lock);
        if (queue.size() < kMaxQueuedVideo) {
            VideoJob j;
            j.buf = std::move(buf);
            j.len = len;
            j.ts_bits = ts_bits;
            queue.push_back(std::move(j));
            q_cv.notify_one();
            return true;
        }
    }
    return false;
}

void Session::start_decoding()
{
    decode_thread = std::thread([this] { decode_loop(); });
}

void Session::stop()
{
    alive = false;
    q_cv.notify_all();
    if (decode_thread.joinable())
        decode_thread.join();
}

void Session::decode_loop()
{
    bool ready = false;
    uint8_t ack[niriun::kAckSize];

    while (alive.load()) {
        VideoJob job;
        {
            std::unique_lock<std::mutex> l(q_lock);
            q_cv.wait_for(l, std::chrono::milliseconds(100), [&] { return !queue.empty() || !alive.load(); });
            if (queue.empty())
                continue;
            job = std::move(queue.front());
            queue.pop_front();
        }

        const uint64_t t0 = os_gettime_ns();

        if (!ready) {
            if (!video.init_video(params.hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264, params.hw)) {
                elog("could not start the %s decoder", params.hevc ? "HEVC" : "H.264");
                src->publish([](Info &i) { i.error = "decoder"; });
                alive = false;
                break;
            }
            ready = true;
        }

        video.set_color_matrix((ColorMatrix)src->color_matrix.load());

        const uint8_t *payload = job.buf.data() + 8;
        const size_t payload_size = job.len - 8;
        float seconds;
        memcpy(&seconds, &job.ts_bits, sizeof(seconds));

        bool got = false;
        if (!video.decode_video(payload, payload_size, (int64_t)((double)seconds * 1e6), &vframe, &got)) {
            elog("video decoder failed, restarting the connection");
            alive = false;
            break;
        }

        if (got) {
            vframe.timestamp = (uint64_t)((double)seconds * 1e9);
            obs_source_output_video2(src->source, &vframe);

            if ((int)vframe.width != shown_w || (int)vframe.height != shown_h) {
                shown_w = (int)vframe.width;
                shown_h = (int)vframe.height;
                src->publish([&](Info &i) {
                    char buf[96];
                    snprintf(buf, sizeof(buf), "%dx%d \xC2\xB7 %s \xC2\xB7 %s", shown_w, shown_h,
                             params.hevc ? "H.265" : "H.264", video.hardware() ? "GPU" : "CPU");
                    i.stream = buf;
                });
            }
        }

        // Confirm every packet (parameter sets and damaged frames too): the phone stops feeding its encoder when
        // it is more than about two frames ahead of the last ACK.
        uint32_t ms = (uint32_t)((os_gettime_ns() - t0) / 1000000ULL);
        niriun::build_ack(ack, job.ts_bits, ms, 0);
        send(ack, sizeof(ack));

        recycle(std::move(job.buf));
    }
}

// ---- NiriunSource: network thread ------------------------------------------------------------------------

socket_t NiriunSource::connect_target(Target &t)
{
    std::string host;
    int port = t.port;

    switch (t.type) {
    case DeviceType::WIFI_MANUAL:
        host = t.ip;
        break;

    case DeviceType::DISCOVERED: {
        DeviceInfo dev;
        if (!Discovery::instance().find(t.id, &dev)) {
            const uint64_t now = niriun::now_ms();
            if (now - last_probe_ms > kProbeCooldownMs) {
                last_probe_ms = now;
                Discovery::instance().probe_subnets();
            }
        }
        if (Discovery::instance().find(t.id, &dev))
            host = dev.ip;
        else
            host = t.ip;  // last address it had
        if (host.empty())
            return INVALID_SOCKET;
        break;
    }

    case DeviceType::ADB: {
        if (adb_port == 0 || adb_serial != t.id) {
            if (adb_port)
                Adb::instance().remove_forward(adb_port);
            adb_port = Adb::instance().forward(t.id, (uint16_t)t.port);
            adb_serial = t.id;
            if (adb_port == 0)
                return INVALID_SOCKET;
        }
        host = "127.0.0.1";
        port = adb_port;
        break;
    }

    case DeviceType::NONE:
        return INVALID_SOCKET;
    }

    socket_t sock = net_connect(host.c_str(), niriun_bind_ip().c_str(), (uint16_t)port, kConnectTimeoutMs, &stopping);
    if (sock == INVALID_SOCKET && t.type == DeviceType::ADB) {
        // The forward may be stale (phone re-plugged, app restarted): make a new one next time.
        Adb::instance().remove_forward(adb_port);
        adb_port = 0;
    }
    return sock;
}

void NiriunSource::flush_outgoing(Session &s)
{
    if (!s.started)
        return;

    std::deque<std::pair<niriun::ControlId, int32_t>> pending;
    {
        std::lock_guard<std::mutex> g(cmd_lock);
        pending.swap(cmds);
    }
    uint8_t buf[niriun::kTallySize];
    for (const auto &c : pending) {
        niriun::build_control(buf, c.first, c.second);
        s.send(buf, niriun::kControlSize);
    }

    // Tally: what OBS says now, or "off" when the user turned the option off.
    const int want = send_tally.load() ? (int)tally() : (int)niriun::kTallyOff;
    if (want != s.last_tally) {
        niriun::build_tally(buf, (niriun::TallyState)want, obs_source_get_name(source));
        if (s.send(buf, niriun::kTallySize)) {
            s.last_tally = want;
            ilog("tally -> %s", want == niriun::kTallyProgram ? "program" : want == niriun::kTallyPreview ? "preview" : "off");
        }
    }

    if (niriun::now_ms() - s.last_send_ms.load() >= kKeepAliveMs) {
        niriun::build_noop(buf);
        s.send(buf, niriun::kNoopSize);
    }
}

bool NiriunSource::handle_packet(Session &s, niriun::Framer &framer, const std::string &peer)
{
    const uint8_t *data = framer.data();
    const size_t len = framer.size();

    switch (framer.type()) {
    case niriun::kMsgHello: {
        niriun::Hello h;
        if (niriun::parse_hello(data, len, &h)) {
            s.got_hello = true;
            ilog("phone: \"%s\" (%s)", h.name.c_str(), h.uuid_hex().c_str());
            Discovery::instance().note(h.uuid_hex(), h.name, peer);
            publish([&](Info &i) { i.phone = h.name; });
        }
        break;
    }

    case niriun::kMsgSizes: {
        if (!niriun::parse_sizes(data, len, &s.sizes))
            break;
        publish([&](Info &i) { i.microphone = s.sizes.microphone(); });
        if (s.started)
            break;

        uint8_t buf[niriun::kStartSize];
        niriun::build_start(buf, s.params.audio && s.sizes.microphone(),
                            s.params.hevc ? niriun::kCodecHevc : niriun::kCodecH264, s.params.width,
                            s.params.height, (float)s.params.fps);
        if (!s.send(buf, niriun::kStartSize))
            return false;
        s.started = true;
        ilog("START %dx%d @%d %s%s", s.params.width, s.params.height, s.params.fps, s.params.hevc ? "HEVC" : "H.264",
             (s.params.audio && s.sizes.microphone()) ? " + audio" : "");
        publish([](Info &i) { i.streaming = true; });

        // A fresh phone-side camera starts at zoom 0; bring back the zoom saved with this source.
        const int z = zoom.load();
        if (z > 0)
            post_control(niriun::kCtlZoom, z);
        break;
    }

    case niriun::kMsgCameras: {
        niriun::Cameras c;
        if (niriun::parse_cameras(data, len, &c))
            publish([&](Info &i) {
                i.cameras = c.names;
                i.camera = c.selected;
            });
        break;
    }

    case niriun::kMsgVideo: {
        niriun::VideoView v;
        if (!s.started || !niriun::parse_video(data, len, &v))
            break;
        const size_t n = framer.size();
        std::vector<uint8_t> buf = framer.take(s.spare());
        if (!s.push_video(std::move(buf), n, v.ts_bits)) {
            // Decoder too far behind: let the phone go on, the next IDR repairs the picture.
            uint8_t ack[niriun::kAckSize];
            niriun::build_ack(ack, v.ts_bits, 0, 0);
            s.send(ack, sizeof(ack));
            elog("video: dropped a packet, the decoder is behind");
        }
        break;
    }

    case niriun::kMsgAudio: {
        const uint8_t *payload;
        size_t size;
        if (!s.started || !enable_audio.load() || !niriun::parse_audio(data, len, &payload, &size))
            break;
        if (!s.audio_ready) {
            if (!s.audio.init_opus())
                break;
            s.audio_ready = true;
        }
        bool got = false;
        if (s.audio.decode_audio(payload, size, &s.aframe, &got) && got) {
            s.aframe.timestamp = os_gettime_ns();
            obs_source_output_audio(source, &s.aframe);
        }
        break;
    }

    case niriun::kMsgStopScreen: {
        niriun::StopScreen mode;
        if (niriun::parse_stop_screen(data, len, &mode))
            s.stop_mode = mode;
        break;
    }

    default:
        break;  // CAMERA_USAGE, control echoes and unknown types are ignored
    }
    return true;
}

namespace {

/** The "offline" card: a ring with a slash through it, centered. */
bool offline_card_pixel(int x, int y, int w, int h)
{
    const float unit = (float)(w < h ? w : h);
    const float radius = unit * 0.14f, half = unit * 0.012f;
    const float dx = (float)x - w * 0.5f, dy = (float)y - h * 0.5f;
    const float d = sqrtf(dx * dx + dy * dy);
    if (fabsf(d - radius) <= half)
        return true;
    return d < radius && fabsf(dx - dy) * 0.70710678f <= half;
}

}  // namespace

void NiriunSource::show_stop_frame(niriun::StopScreen mode, int w, int h)
{
    if (w < 2 || h < 2) {
        w = 1280;
        h = 720;
    }
    w &= ~1;
    h &= ~1;
    const int cw = w / 2, ch = h / 2;
    const bool card = mode == niriun::kStopOffline;
    stop_buf.assign((size_t)w * h + 2 * (size_t)cw * ch, 0);
    uint8_t *y = stop_buf.data();
    uint8_t *u = y + (size_t)w * h;
    uint8_t *v = u + (size_t)cw * ch;
    // Limited-range BT.709: black is 16/128/128; the card is dark purple with a light purple ring.
    memset(y, card ? 24 : 16, (size_t)w * h);
    memset(u, card ? 137 : 128, (size_t)cw * ch);
    memset(v, card ? 133 : 128, (size_t)cw * ch);
    if (card) {
        for (int row = 0; row < h; row++)
            for (int col = 0; col < w; col++)
                if (offline_card_pixel(col, row, w, h))
                    y[(size_t)row * w + col] = 153;
        for (int row = 0; row < ch; row++)
            for (int col = 0; col < cw; col++)
                if (offline_card_pixel(col * 2, row * 2, w, h)) {
                    u[(size_t)row * cw + col] = 173;
                    v[(size_t)row * cw + col] = 149;
                }
    }

    obs_source_frame2 f = {};
    f.width = (uint32_t)w;
    f.height = (uint32_t)h;
    f.format = VIDEO_FORMAT_I420;
    f.range = VIDEO_RANGE_PARTIAL;
    f.timestamp = os_gettime_ns();
    video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_I420, f.color_matrix,
                                           f.color_range_min, f.color_range_max);
    f.data[0] = y;
    f.data[1] = u;
    f.data[2] = v;
    f.linesize[0] = (uint32_t)w;
    f.linesize[1] = f.linesize[2] = (uint32_t)cw;
    obs_source_output_video2(source, &f);
    stop_card_shown = true;
    ilog("showing the %s screen after Stop", card ? "offline" : "black");
}

void NiriunSource::run_session(socket_t sock, const StartParams &sp)
{
    set_recv_buf_len(sock, 1 << 20);
    set_send_timeout(sock, 3000);

    const std::string peer = net_peer_ip(sock);
    Session s(this, sock, sp);
    s.last_rx_ms = niriun::now_ms();
    s.last_send_ms = niriun::now_ms();
    s.start_decoding();

    publish([](Info &i) {
        i = Info();
        i.connected = true;
    });
    set_error("");
    ilog("connected to %s", peer.c_str());

    niriun::Framer framer;
    while (!stopping.load() && s.alive.load() && should_connect() && params() == sp) {
        flush_outgoing(s);

        int w = net_wait_readable(sock, 50);
        if (w < 0)
            break;
        if (w == 0) {
            if (niriun::now_ms() - s.last_rx_ms > kInboundTimeoutMs) {
                elog("nothing received from the phone for %llus, reconnecting",
                     (unsigned long long)(kInboundTimeoutMs / 1000));
                break;
            }
            continue;
        }

        size_t need;
        uint8_t *dst = framer.want(&need);
        ssize_t n = net_recv(sock, dst, need);
        if (n <= 0) {
            ilog("phone closed the connection");
            break;
        }
        s.last_rx_ms = niriun::now_ms();

        int r = framer.commit((size_t)n);
        if (r < 0) {
            elog("invalid packet length from the phone");
            break;
        }
        if (r == 1 && !handle_packet(s, framer, peer))
            break;
    }

    s.stop();
    net_close(sock);

    // A deliberate Stop on the phone says what to leave on screen; a dropped connection clears the picture. A
    // connection that closes before the phone even said hello (adb forward with the app stopped) must not wipe
    // the card that is already showing.
    if (s.stop_mode != niriun::kStopClear && !stopping.load() && should_connect()) {
        show_stop_frame(s.stop_mode, s.shown_w, s.shown_h);
    } else if (s.got_hello || !stop_card_shown) {
        obs_source_output_video2(source, NULL);
        stop_card_shown = false;
    }
    obs_source_output_audio(source, NULL);
    publish([](Info &i) {
        Info keep;
        keep.phone = i.phone;
        keep.cameras = i.cameras;
        i = keep;
    });
    ilog("disconnected");
}

void NiriunSource::run()
{
    ilog("network thread started for \"%s\"", obs_source_get_name(source));

    while (!stopping.load()) {
        wake.reset();

        if (!should_connect()) {
            if (adb_port) {
                Adb::instance().remove_forward(adb_port);
                adb_port = 0;
            }
            if (stop_card_shown) {
                obs_source_output_video2(source, NULL);
                stop_card_shown = false;
            }
            wake.wait_ms(250);
            continue;
        }

        Target t;
        {
            std::lock_guard<std::mutex> g(cfg_lock);
            t = target;
        }

        const StartParams sp = params();
        socket_t sock = connect_target(t);
        if (sock == INVALID_SOCKET) {
            publish([](Info &i) { i.connected = false; });
            wake.wait_ms(kRetryDelayMs);
            continue;
        }

        run_session(sock, sp);

        if (!stopping.load())
            wake.wait_ms(500);
    }

    if (adb_port) {
        Adb::instance().remove_forward(adb_port);
        adb_port = 0;
    }
    ilog("network thread stopped");
}

// ---- settings --------------------------------------------------------------------------------------------

bool make_target(obs_data_t *settings, const std::string &id, Target *out, std::string *err)
{
    Target t;
    t.port = (int)obs_data_get_int(settings, OPT_APP_PORT);
    if (t.port <= 0 || t.port > 65535) {
        *err = LOC("BadPort");
        return false;
    }

    if (id == DEV_MANUAL) {
        t.type = DeviceType::WIFI_MANUAL;
        t.ip = trim(obs_data_get_string(settings, OPT_WIFI_IP));
        if (t.ip.empty()) {
            *err = LOC("NoWifiIP");
            return false;
        }
        t.name = t.ip;
    } else if (id.rfind(DEV_PREFIX_UUID, 0) == 0) {
        t.type = DeviceType::DISCOVERED;
        t.id = id.substr(strlen(DEV_PREFIX_UUID));
        DeviceInfo dev;
        if (Discovery::instance().find(t.id, &dev)) {
            t.ip = dev.ip;
            t.name = dev.name;
        } else {
            // Not seen this session: keep the address and name saved when it was last activated.
            const char *old_id = obs_data_get_string(settings, OPT_ACTIVE_DEV_ID);
            if (old_id && t.id == old_id) {
                t.ip = obs_data_get_string(settings, OPT_ACTIVE_DEV_IP);
                t.name = obs_data_get_string(settings, OPT_ACTIVE_DEV_NAME);
            }
        }
    } else if (id.rfind(DEV_PREFIX_ADB, 0) == 0) {
        t.type = DeviceType::ADB;
        t.id = id.substr(strlen(DEV_PREFIX_ADB));
        t.name = t.id;
        for (const AdbDevice &d : adb_cache())
            if (d.serial == t.id && !d.model.empty())
                t.name = d.model;
    } else {
        *err = LOC("NoDevice");
        return false;
    }

    *out = t;
    return true;
}

/** Copies the persisted state into the live source. Used by create and update. */
void load_settings(NiriunSource *s, obs_data_t *settings)
{
    s->enable_audio = obs_data_get_bool(settings, OPT_ENABLE_AUDIO);
    s->use_hw = obs_data_get_bool(settings, OPT_HW_ACCEL);
    s->send_tally = obs_data_get_bool(settings, OPT_SEND_TALLY);
    s->deactivate_wns = obs_data_get_bool(settings, OPT_DEACTIVATE_WNS);
    s->hevc = obs_data_get_int(settings, OPT_VIDEO_FORMAT) == 1;
    s->fps = std::max(1, std::min(120, (int)obs_data_get_int(settings, OPT_FPS)));
    s->color_matrix = (int)obs_data_get_int(settings, OPT_COLOR_MATRIX);
    s->zoom = std::max(0, std::min(100, (int)obs_data_get_int(settings, OPT_ZOOM)));

    int w, h;
    if (parse_resolution(obs_data_get_string(settings, OPT_RESOLUTION), &w, &h)) {
        s->width = w;
        s->height = h;
    }

    obs_source_set_async_unbuffered(s->source, obs_data_get_bool(settings, OPT_UNBUFFERED));
    obs_source_set_async_decoupled(s->source, true);  // only effective in unbuffered mode

    Target t;
    t.type = (DeviceType)obs_data_get_int(settings, OPT_ACTIVE_DEV_TYPE);
    t.id = obs_data_get_string(settings, OPT_ACTIVE_DEV_ID);
    t.ip = obs_data_get_string(settings, OPT_ACTIVE_DEV_IP);
    t.name = obs_data_get_string(settings, OPT_ACTIVE_DEV_NAME);
    t.port = (int)obs_data_get_int(settings, OPT_APP_PORT);
    if (t.port <= 0 || t.port > 65535)
        t.port = niriun::kTcpPort;
    const bool valid = t.type != DeviceType::NONE && (t.type == DeviceType::WIFI_MANUAL ? !t.ip.empty() : !t.id.empty());
    if (!valid)
        t.type = DeviceType::NONE;
    {
        std::lock_guard<std::mutex> g(s->cfg_lock);
        s->target = t;
    }
    s->activated = obs_data_get_bool(settings, OPT_IS_ACTIVATED) && valid;
    s->wake.set();
}

const char *niriun_name(void *)
{
    return LOC("NiriunSource");
}

void *niriun_create(obs_data_t *settings, obs_source_t *source)
{
    std::call_once(g_started, [] {
        Discovery::instance().start();
        g_warmup = std::thread([] {
            if (Adb::instance().available())
                refresh_adb_cache();
        });
    });

    NiriunSource *s = new NiriunSource();
    s->source = source;
    ilog("source \"%s\" created", obs_source_get_name(source));
    load_settings(s, settings);
    s->net_thread = std::thread([s] { s->run(); });
    return s;
}

void niriun_destroy(void *data)
{
    NiriunSource *s = (NiriunSource *)data;
    ilog("source \"%s\" destroyed", obs_source_get_name(s->source));
    s->stopping = true;
    s->wake.set();
    if (s->net_thread.joinable())
        s->net_thread.join();
    delete s;
}

void niriun_update(void *data, obs_data_t *settings)
{
    load_settings((NiriunSource *)data, settings);
}

void niriun_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, OPT_DEVICE_LIST, DEV_MANUAL);
    obs_data_set_default_int(settings, OPT_APP_PORT, niriun::kTcpPort);
    obs_data_set_default_string(settings, OPT_RESOLUTION, "1920x1080");
    obs_data_set_default_int(settings, OPT_FPS, 30);
    obs_data_set_default_int(settings, OPT_VIDEO_FORMAT, 0);
    obs_data_set_default_bool(settings, OPT_IS_ACTIVATED, false);
    obs_data_set_default_bool(settings, OPT_ENABLE_AUDIO, true);
    obs_data_set_default_bool(settings, OPT_HW_ACCEL, true);
    obs_data_set_default_bool(settings, OPT_UNBUFFERED, true);
    obs_data_set_default_bool(settings, OPT_DEACTIVATE_WNS, false);
    obs_data_set_default_bool(settings, OPT_SEND_TALLY, true);
    obs_data_set_default_int(settings, OPT_COLOR_MATRIX, 0);
    obs_data_set_default_int(settings, OPT_ZOOM, 0);
}

// ---- tally: OBS tells the source when it becomes visible / live -------------------------------------------

// show/hide: the source is visible somewhere (the preview in Studio Mode, the editing view, a projector).
// activate/deactivate: it is part of the program output. Program wins when both are true.
void niriun_show(void *data)
{
    NiriunSource *s = (NiriunSource *)data;
    s->on_preview = true;
    s->showing = true;
    s->wake.set();
}

void niriun_hide(void *data)
{
    NiriunSource *s = (NiriunSource *)data;
    s->on_preview = false;
    s->showing = false;
    s->wake.set();
}

void niriun_activate(void *data)
{
    NiriunSource *s = (NiriunSource *)data;
    s->on_program = true;
    s->wake.set();
}

void niriun_deactivate(void *data)
{
    NiriunSource *s = (NiriunSource *)data;
    s->on_program = false;
    s->wake.set();
}

// ---- properties ------------------------------------------------------------------------------------------

void apply_enabled(obs_properties_t *ppts, bool activated)
{
    for (const char *name : {OPT_DEVICE_LIST, "refresh", OPT_WIFI_IP, OPT_APP_PORT}) {
        obs_property_t *p = obs_properties_get(ppts, name);
        if (p)
            obs_property_set_enabled(p, !activated);
    }
}

bool refresh_clicked(obs_properties_t *, obs_property_t *, void *)
{
    ilog("refreshing the device list");
    Discovery::instance().probe_subnets();
    refresh_adb_cache();
    return true;  // rebuilds the list
}

bool activate_clicked(obs_properties_t *, obs_property_t *, void *data)
{
    NiriunSource *s = (NiriunSource *)data;
    if (!s)
        return false;

    obs_data_t *settings = obs_source_get_settings(s->source);

    if (obs_data_get_bool(settings, OPT_IS_ACTIVATED)) {
        obs_data_set_bool(settings, OPT_IS_ACTIVATED, false);
        s->activated = false;
        s->wake.set();
        ilog("deactivated");
        obs_data_release(settings);
        return true;
    }

    const std::string id = obs_data_get_string(settings, OPT_DEVICE_LIST);
    Target t;
    std::string err;
    if (!make_target(settings, id, &t, &err)) {
        elog("cannot activate: %s", err.c_str());
        s->set_error(err);
        obs_data_release(settings);
        return true;
    }

    obs_data_set_string(settings, OPT_ACTIVE_DEV_ID, t.id.c_str());
    obs_data_set_string(settings, OPT_ACTIVE_DEV_IP, t.ip.c_str());
    obs_data_set_string(settings, OPT_ACTIVE_DEV_NAME, t.name.c_str());
    obs_data_set_int(settings, OPT_ACTIVE_DEV_TYPE, (long long)t.type);
    obs_data_set_bool(settings, OPT_IS_ACTIVATED, true);
    obs_data_release(settings);

    s->set_error("");
    {
        std::lock_guard<std::mutex> g(s->cfg_lock);
        s->target = t;
    }
    s->activated = true;
    s->wake.set();
    ilog("activated: type=%d id=%s ip=%s port=%d", (int)t.type, t.id.c_str(), t.ip.c_str(), t.port);
    return true;
}

bool torch_clicked(obs_properties_t *, obs_property_t *, void *data)
{
    if (data)
        ((NiriunSource *)data)->post_control(niriun::kCtlTorch, 0);
    return false;
}

bool mirror_clicked(obs_properties_t *, obs_property_t *, void *data)
{
    if (data)
        ((NiriunSource *)data)->post_control(niriun::kCtlMirror, 0);
    return false;
}

bool lock_clicked(obs_properties_t *, obs_property_t *, void *data)
{
    if (data)
        ((NiriunSource *)data)->post_control(niriun::kCtlLockExposureFocus, 0);
    return false;
}

// OBS runs every modified-callback once when the dialog opens, so compare with what is already applied.
bool zoom_changed(void *data, obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
    NiriunSource *s = (NiriunSource *)data;
    if (!s)
        return false;
    int z = std::max(0, std::min(100, (int)obs_data_get_int(settings, OPT_ZOOM)));
    if (z != s->zoom.exchange(z))
        s->post_control(niriun::kCtlZoom, z);
    return false;
}

bool camera_changed(void *data, obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
    NiriunSource *s = (NiriunSource *)data;
    if (!s)
        return false;
    uint32_t wanted = (uint32_t)obs_data_get_int(settings, OPT_CAMERA);
    uint32_t current;
    size_t count;
    {
        std::lock_guard<std::mutex> g(s->info_lock);
        current = s->info.camera;
        count = s->info.cameras.size();
    }
    if (wanted != current && wanted < count) {
        s->post_control(niriun::kCtlSelectCamera, (int32_t)wanted);
        std::lock_guard<std::mutex> g(s->info_lock);
        s->info.camera = wanted;  // the phone confirms with a new camera list
    }
    return false;
}

std::string status_text(NiriunSource *s)
{
    Info info;
    std::string err;
    {
        std::lock_guard<std::mutex> g(s->info_lock);
        info = s->info;
    }
    {
        std::lock_guard<std::mutex> g(s->cfg_lock);
        err = s->last_error;
    }

    char buf[512];
    if (!err.empty()) {
        snprintf(buf, sizeof(buf), "%s", err.c_str());
    } else if (!s->activated.load()) {
        snprintf(buf, sizeof(buf), "%s", LOC("StatusIdle"));
    } else if (!info.connected) {
        snprintf(buf, sizeof(buf), "%s", LOC("StatusSearching"));
    } else {
        const char *tally = s->tally() == niriun::kTallyProgram ? LOC("TallyProgram")
                            : s->tally() == niriun::kTallyPreview ? LOC("TallyPreview")
                                                                  : LOC("TallyOff");
        snprintf(buf, sizeof(buf), "%s: %s%s%s \xE2\x80\xA2 %s", LOC("StatusConnected"), info.phone.c_str(),
                 info.stream.empty() ? "" : " \xE2\x80\xA2 ", info.stream.c_str(), tally);
    }
    return buf;
}

obs_properties_t *niriun_properties(void *data)
{
    NiriunSource *s = (NiriunSource *)data;
    obs_properties_t *ppts = obs_properties_create();

    bool activated = false;
    std::string selected;
    if (s) {
        obs_data_t *settings = obs_source_get_settings(s->source);
        activated = obs_data_get_bool(settings, OPT_IS_ACTIVATED);
        selected = obs_data_get_string(settings, OPT_DEVICE_LIST);
        obs_data_release(settings);
    }

    obs_property_t *p = obs_properties_add_text(ppts, "status", LOC("Status"), OBS_TEXT_INFO);
    obs_property_set_long_description(p, s ? status_text(s).c_str() : "");

    // ---- device ----
    p = obs_properties_add_list(ppts, OPT_DEVICE_LIST, LOC("Device"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    bool have_selected = false;
    {
        std::vector<DeviceInfo> found = Discovery::instance().devices();
        std::sort(found.begin(), found.end(), [](const DeviceInfo &a, const DeviceInfo &b) { return a.name < b.name; });
        for (const DeviceInfo &d : found) {
            std::string label = (d.name.empty() ? d.uuid.substr(0, 8) : d.name) + " [WiFi] (" + d.ip + ")";
            std::string value = DEV_PREFIX_UUID + d.uuid;
            obs_property_list_add_string(p, label.c_str(), value.c_str());
            have_selected |= value == selected;
        }
        for (const AdbDevice &d : adb_cache()) {
            std::string label = (d.model.empty() ? d.serial : d.model) + " [USB] (" + d.serial + ")";
            std::string value = DEV_PREFIX_ADB + d.serial;
            size_t idx = obs_property_list_add_string(p, label.c_str(), value.c_str());
            if (!d.online())
                obs_property_list_item_disable(p, idx, true);
            have_selected |= value == selected;
        }
    }
    if (s && !have_selected && selected.rfind(DEV_PREFIX_UUID, 0) == 0) {
        // Chosen earlier but silent now (beacons stop while a computer is connected): keep it selectable.
        obs_data_t *settings = obs_source_get_settings(s->source);
        std::string name = obs_data_get_string(settings, OPT_ACTIVE_DEV_NAME);
        std::string ip = obs_data_get_string(settings, OPT_ACTIVE_DEV_IP);
        obs_data_release(settings);
        std::string label = (name.empty() ? selected.substr(strlen(DEV_PREFIX_UUID), 8) : name) + " [WiFi] (" + ip + ")";
        obs_property_list_add_string(p, label.c_str(), selected.c_str());
    }
    obs_property_list_add_string(p, LOC("UseWiFi"), DEV_MANUAL);

    obs_properties_add_button2(ppts, "refresh", LOC("Refresh"), refresh_clicked, s);
    obs_properties_add_text(ppts, OPT_WIFI_IP, LOC("WifiIP"), OBS_TEXT_DEFAULT);
    obs_properties_add_int(ppts, OPT_APP_PORT, LOC("Port"), 1, 65535, 1);
    obs_properties_add_button2(ppts, "activate", activated ? LOC("Deactivate") : LOC("Activate"), activate_clicked, s);
    apply_enabled(ppts, activated);

    // ---- video ----
    p = obs_properties_add_list(ppts, OPT_RESOLUTION, LOC("Resolution"), OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    for (const char *r : kResolutions)
        obs_property_list_add_string(p, r, r);

    p = obs_properties_add_list(ppts, OPT_FPS, LOC("FrameRate"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (int f : kFrameRates) {
        char label[16];
        snprintf(label, sizeof(label), "%d", f);
        obs_property_list_add_int(p, label, f);
    }

    p = obs_properties_add_list(ppts, OPT_VIDEO_FORMAT, LOC("VideoFormat"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p, "AVC/H.264", 0);
    obs_property_list_add_int(p, "HEVC/H.265", 1);

    p = obs_properties_add_list(ppts, OPT_COLOR_MATRIX, LOC("ColorMatrix"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p, LOC("ColorAuto"), 0);
    obs_property_list_add_int(p, "Rec. 601", 1);
    obs_property_list_add_int(p, "Rec. 709", 2);

    // ---- behaviour ----
    obs_properties_add_bool(ppts, OPT_ENABLE_AUDIO, LOC("EnableAudio"));
    obs_properties_add_bool(ppts, OPT_SEND_TALLY, LOC("SendTally"));
    obs_properties_add_bool(ppts, OPT_DEACTIVATE_WNS, LOC("DeactivateWhenNotShowing"));
    p = obs_properties_add_bool(ppts, OPT_UNBUFFERED, LOC("LatencyToggle"));
    obs_property_set_long_description(p, LOC("LatencyToolTip"));
    obs_properties_add_bool(ppts, OPT_HW_ACCEL, LOC("AllowHWAccel"));

    // ---- camera controls (sent to the phone while connected) ----
    obs_properties_t *cam = obs_properties_create();
    p = obs_properties_add_list(cam, OPT_CAMERA, LOC("Camera"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    if (s) {
        Info info;
        {
            std::lock_guard<std::mutex> g(s->info_lock);
            info = s->info;
        }
        for (size_t i = 0; i < info.cameras.size(); i++)
            obs_property_list_add_int(p, info.cameras[i].c_str(), (long long)i);
        obs_data_t *settings = obs_source_get_settings(s->source);
        obs_data_set_int(settings, OPT_CAMERA, info.camera);
        obs_data_release(settings);
    }
    obs_property_set_modified_callback2(p, camera_changed, s);

    p = obs_properties_add_int_slider(cam, OPT_ZOOM, LOC("Zoom"), 0, 100, 1);
    obs_property_set_modified_callback2(p, zoom_changed, s);
    obs_properties_add_button2(cam, "torch", LOC("Torch"), torch_clicked, s);
    obs_properties_add_button2(cam, "mirror", LOC("Mirror"), mirror_clicked, s);
    obs_properties_add_button2(cam, "lock", LOC("LockFocus"), lock_clicked, s);
    obs_properties_add_group(ppts, "camera_group", LOC("CameraControls"), OBS_GROUP_NORMAL, cam);

    return ppts;
}

}  // namespace

void register_niriun_source(void)
{
    static struct obs_source_info info;
    memset(&info, 0, sizeof(info));
    info.id = "niriun_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC_VIDEO;
    info.icon_type = OBS_ICON_TYPE_CAMERA;
    info.get_name = niriun_name;
    info.create = niriun_create;
    info.destroy = niriun_destroy;
    info.update = niriun_update;
    info.get_defaults = niriun_defaults;
    info.get_properties = niriun_properties;
    info.show = niriun_show;
    info.hide = niriun_hide;
    info.activate = niriun_activate;
    info.deactivate = niriun_deactivate;
    obs_register_source(&info);
}

void niriun_source_shutdown(void)
{
    if (g_warmup.joinable())
        g_warmup.join();
    Discovery::instance().stop();
}
