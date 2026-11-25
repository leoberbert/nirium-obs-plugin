/*
    Copyright (C) 2014 by Hugh Bailey <obs.jim@gmail.com>
    Copyright (C) 2021 DEV47APPS, github.com/dev47apps
    Copyright (C) 2026 Leonardo Berbert (reworked for the Niriun plugin: Opus, standalone decoder)

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
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <obs.h>

extern "C" {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4204)
#endif
#include <libavcodec/avcodec.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

enum class ColorMatrix {
    AUTO,   // what the stream says; Rec.709 when it says nothing
    REC601,
    REC709,
};

/** Wraps one libavcodec decoder: H.264/HEVC video (optionally hardware accelerated) or raw Opus audio. */
class FFmpegDecoder {
public:
    FFmpegDecoder() = default;
    ~FFmpegDecoder();
    FFmpegDecoder(const FFmpegDecoder &) = delete;
    FFmpegDecoder &operator=(const FFmpegDecoder &) = delete;

    bool init_video(enum AVCodecID id, bool use_hw);
    /** 48 kHz mono, one Opus packet per call to decode_audio (the phone sends 20 ms packets). */
    bool init_opus();

    void set_color_matrix(ColorMatrix m) { color_matrix_ = m; }
    bool hardware() const { return hw_; }

    /**
     * `data` needs AV_INPUT_BUFFER_PADDING_SIZE zero bytes after `size`. A packet with only parameter sets
     * (SPS/PPS/VPS) is valid and produces no picture. @return false when the decoder is broken for good and
     * the connection should be restarted; a damaged packet is skipped and returns true.
     * When `*got_output` is set, `out` points into decoder memory valid until the next call.
     */
    bool decode_video(const uint8_t *data, size_t size, int64_t pts, struct obs_source_frame2 *out, bool *got_output);
    bool decode_audio(const uint8_t *data, size_t size, struct obs_source_audio *out, bool *got_output);

private:
    bool open(const AVCodec *codec);
    bool describe_picture(const AVFrame *f, struct obs_source_frame2 *out);

    const AVCodec *codec_ = nullptr;
    AVCodecContext *ctx_ = nullptr;
    AVPacket *packet_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVFrame *frame_hw_ = nullptr;
    AVBufferRef *hw_ctx_ = nullptr;
    enum AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;
    bool hw_ = false;
    ColorMatrix color_matrix_ = ColorMatrix::AUTO;
    unsigned damaged_ = 0;
    unsigned audio_errors_ = 0;

    // What the OBS frame was last configured for; reconfigured when the stream changes.
    int cfg_pix_fmt_ = -1;
    int cfg_range_ = -1;
    int cfg_cs_ = -1;
    int cfg_trc_ = -1;
};
