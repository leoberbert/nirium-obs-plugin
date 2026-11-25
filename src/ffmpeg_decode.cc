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
#include "ffmpeg_decode.h"

#include <string.h>

#include "plugin.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(59, 24, 100)
#error libavcodec 59.24.100 (FFmpeg 5.1, OBS 29.1) or newer is required
#endif

// https://trac.ffmpeg.org/wiki/HWAccelIntro
static const enum AVHWDeviceType hw_device_list[] = {
#ifdef _WIN32
	AV_HWDEVICE_TYPE_D3D11VA,
	AV_HWDEVICE_TYPE_CUDA,
	AV_HWDEVICE_TYPE_DXVA2,
#endif
#ifdef __APPLE__
	AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#endif
#ifdef __linux__
	AV_HWDEVICE_TYPE_CUDA,
	AV_HWDEVICE_TYPE_VAAPI,
	AV_HWDEVICE_TYPE_VDPAU,
#endif
	AV_HWDEVICE_TYPE_NONE,
};

static enum AVPixelFormat has_hw_type(const AVCodec *c, enum AVHWDeviceType type)
{
	for (int i = 0;; i++) {
		const AVCodecHWConfig *config = avcodec_get_hw_config(c, i);
		if (!config)
			break;
		if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == type)
			return config->pix_fmt;
	}
	return AV_PIX_FMT_NONE;
}

static const char *hw_pix_fmt_name(enum AVPixelFormat f)
{
	const char *n = av_get_pix_fmt_name(f);
	return n ? n : "none";
}

FFmpegDecoder::~FFmpegDecoder()
{
	if (frame_hw_)
		av_frame_free(&frame_hw_);
	if (frame_)
		av_frame_free(&frame_);
	if (packet_)
		av_packet_free(&packet_);
	if (ctx_)
		avcodec_free_context(&ctx_);
	if (hw_ctx_)
		av_buffer_unref(&hw_ctx_);
}

bool FFmpegDecoder::open(const AVCodec *codec)
{
	if (avcodec_open2(ctx_, codec, NULL) < 0)
		return false;

	frame_ = av_frame_alloc();
	packet_ = av_packet_alloc();
	if (hw_)
		frame_hw_ = av_frame_alloc();
	return frame_ && packet_ && (!hw_ || frame_hw_);
}

bool FFmpegDecoder::init_video(enum AVCodecID id, bool use_hw)
{
	codec_ = avcodec_find_decoder(id);
	if (!codec_) {
		elog("no decoder for codec id %d", (int)id);
		return false;
	}

	ctx_ = avcodec_alloc_context3(codec_);
	if (!ctx_)
		return false;

	// Both flags only take effect when set before avcodec_open2(). Without LOW_DELAY the H.264 decoder holds
	// frames back to reorder them, which the phone's encoder never needs (no B-frames).
	ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
	ctx_->thread_count = 1;

	if (use_hw) {
		for (const enum AVHWDeviceType *t = hw_device_list; *t != AV_HWDEVICE_TYPE_NONE; t++) {
			hw_pix_fmt_ = has_hw_type(codec_, *t);
			if (hw_pix_fmt_ == AV_PIX_FMT_NONE)
				continue;
			if (av_hwdevice_ctx_create(&hw_ctx_, *t, NULL, NULL, 0) == 0) {
				ctx_->hw_device_ctx = av_buffer_ref(hw_ctx_);
				hw_ = true;
				ilog("video: hardware decoding via %s (%s)", av_hwdevice_get_type_name(*t), hw_pix_fmt_name(hw_pix_fmt_));
				break;
			}
			hw_pix_fmt_ = AV_PIX_FMT_NONE;
		}
		if (!hw_)
			ilog("video: no hardware decoder available, using software");
	}

	if (!open(codec_)) {
		elog("could not open %s decoder", codec_->name);
		return false;
	}
	return true;
}

bool FFmpegDecoder::init_opus()
{
	codec_ = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!codec_) {
		elog("this FFmpeg build has no Opus decoder");
		return false;
	}

	ctx_ = avcodec_alloc_context3(codec_);
	if (!ctx_)
		return false;

	ctx_->sample_rate = 48000;
	av_channel_layout_default(&ctx_->ch_layout, 1);

	// The phone sends bare Opus packets; give the decoder the OpusHead it would otherwise find in an Ogg stream
	// (version 1, 1 channel, no pre-skip, 48000 Hz, 0 dB gain, channel mapping family 0).
	static const uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0};
	ctx_->extradata = (uint8_t *)av_mallocz(sizeof(head) + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!ctx_->extradata)
		return false;
	memcpy(ctx_->extradata, head, sizeof(head));
	ctx_->extradata_size = (int)sizeof(head);

	if (!open(codec_)) {
		elog("could not open the Opus decoder");
		return false;
	}
	return true;
}

static inline enum video_format convert_pixel_format(int f)
{
	switch (f) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUYV422:
		return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_UYVY422:
		return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_YUV422P10LE:
		return VIDEO_FORMAT_I210;
	}
	return VIDEO_FORMAT_NONE;
}

static enum video_colorspace convert_color_space(enum AVColorSpace s, enum AVColorTransferCharacteristic trc,
						 enum AVColorPrimaries color_primaries)
{
	switch (s) {
	case AVCOL_SPC_BT709:
		return (trc == AVCOL_TRC_IEC61966_2_1) ? VIDEO_CS_SRGB : VIDEO_CS_709;
	case AVCOL_SPC_FCC:
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_SMPTE240M:
		return VIDEO_CS_601;
	case AVCOL_SPC_BT2020_NCL:
		return (trc == AVCOL_TRC_ARIB_STD_B67) ? VIDEO_CS_2100_HLG : VIDEO_CS_2100_PQ;
	default:
		return (color_primaries == AVCOL_PRI_BT2020)
			       ? ((trc == AVCOL_TRC_ARIB_STD_B67) ? VIDEO_CS_2100_HLG : VIDEO_CS_2100_PQ)
			       : VIDEO_CS_DEFAULT;
	}
}

static inline enum video_trc convert_trc(enum AVColorTransferCharacteristic trc)
{
	switch (trc) {
	case AVCOL_TRC_BT709:
	case AVCOL_TRC_GAMMA22:
	case AVCOL_TRC_GAMMA28:
	case AVCOL_TRC_SMPTE170M:
	case AVCOL_TRC_SMPTE240M:
	case AVCOL_TRC_IEC61966_2_1:
		return VIDEO_TRC_SRGB;
	case AVCOL_TRC_SMPTE2084:
		return VIDEO_TRC_PQ;
	case AVCOL_TRC_ARIB_STD_B67:
		return VIDEO_TRC_HLG;
	default:
		return VIDEO_TRC_DEFAULT;
	}
}

static inline enum audio_format convert_sample_format(int f)
{
	switch (f) {
	case AV_SAMPLE_FMT_S16:
		return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S32:
		return AUDIO_FORMAT_32BIT;
	case AV_SAMPLE_FMT_U8:
		return AUDIO_FORMAT_U8BIT;
	case AV_SAMPLE_FMT_FLT:
		return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_U8P:
		return AUDIO_FORMAT_U8BIT_PLANAR;
	case AV_SAMPLE_FMT_S16P:
		return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S32P:
		return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_FLTP:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	default:;
	}
	return AUDIO_FORMAT_UNKNOWN;
}

static inline enum speaker_layout convert_speaker_layout(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

// Fills the format/colour fields of `out` for picture `f`, only when something changed since the last call.
bool FFmpegDecoder::describe_picture(const AVFrame *f, struct obs_source_frame2 *out)
{
	enum video_format format = convert_pixel_format(f->format);
	if (format == VIDEO_FORMAT_NONE) {
		elog("unsupported pixel format %s", hw_pix_fmt_name((enum AVPixelFormat)f->format));
		return false;
	}

	const enum video_range_type range = (f->color_range == AVCOL_RANGE_JPEG) ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;

	enum video_colorspace cs = convert_color_space(f->colorspace, f->color_trc, f->color_primaries);
	if (color_matrix_ == ColorMatrix::REC601)
		cs = VIDEO_CS_601;
	else if (color_matrix_ == ColorMatrix::REC709 && cs != VIDEO_CS_2100_HLG && cs != VIDEO_CS_2100_PQ)
		cs = VIDEO_CS_709;

	const enum video_trc trc = convert_trc(f->color_trc);

	if ((int)format != cfg_pix_fmt_ || (int)range != cfg_range_ || (int)cs != cfg_cs_ || (int)trc != cfg_trc_) {
		ilog("video: %s %dx%d, colorspace %s (stream says %s / %s), %s range", get_video_format_name(format),
		     f->width, f->height, get_video_colorspace_name(cs), av_color_space_name(f->colorspace),
		     av_color_primaries_name(f->color_primaries), range == VIDEO_RANGE_FULL ? "full" : "limited");

		out->format = format;
		out->range = range;
		out->trc = trc;
		video_format_get_parameters_for_format(cs, range, format, out->color_matrix, out->color_range_min,
						       out->color_range_max);
		cfg_pix_fmt_ = (int)format;
		cfg_range_ = (int)range;
		cfg_cs_ = (int)cs;
		cfg_trc_ = (int)trc;
	}
	return true;
}

bool FFmpegDecoder::decode_video(const uint8_t *data, size_t size, int64_t pts, struct obs_source_frame2 *out,
				 bool *got_output)
{
	*got_output = false;

	av_frame_unref(frame_);
	if (frame_hw_)
		av_frame_unref(frame_hw_);

	packet_->data = (uint8_t *)data;
	packet_->size = (int)size;
	packet_->pts = pts;

	int ret = avcodec_send_packet(ctx_, packet_);
	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		// Damaged or incomplete data (for instance a P-frame after a lost IDR). Skip it; the encoder sends a
		// new IDR every couple of seconds. Only give up when nothing decodes for a long time. The very first
		// rejection is silent: the H.264 decoder answers "no frame!" to the packet that carries only SPS/PPS.
		if (++damaged_ == 2 || damaged_ % 60 == 0)
			elog("video: packet rejected by the decoder (%d), %u in a row", ret, damaged_);
		return ret == AVERROR_INVALIDDATA && damaged_ < 300;
	}

	AVFrame *decoded = hw_ ? frame_hw_ : frame_;
	ret = avcodec_receive_frame(ctx_, decoded);
	if (ret == AVERROR(EAGAIN))
		return true;  // parameter sets only, or the decoder needs more data
	if (ret < 0) {
		if (++damaged_ == 2 || damaged_ % 60 == 0)
			elog("video: decode error %d, %u in a row", ret, damaged_);
		return ret == AVERROR_INVALIDDATA && damaged_ < 300;
	}
	damaged_ = 0;

	AVFrame *picture = decoded;
	if (hw_ && decoded->format == hw_pix_fmt_) {
		if (av_hwframe_transfer_data(frame_, decoded, 0) != 0 || av_frame_copy_props(frame_, decoded) != 0) {
			elog("video: copying the picture from the GPU failed");
			return false;
		}
		picture = frame_;
	}

	if (!describe_picture(picture, out))
		return false;

	for (int i = 0; i < MAX_AV_PLANES; i++) {
		out->data[i] = picture->data[i];
		out->linesize[i] = (uint32_t)picture->linesize[i];
	}
	out->width = (uint32_t)picture->width;
	out->height = (uint32_t)picture->height;
	out->flip = false;

	*got_output = true;
	return true;
}

bool FFmpegDecoder::decode_audio(const uint8_t *data, size_t size, struct obs_source_audio *out, bool *got_output)
{
	*got_output = false;

	av_frame_unref(frame_);
	packet_->data = (uint8_t *)data;
	packet_->size = (int)size;
	packet_->pts = AV_NOPTS_VALUE;

	int ret = avcodec_send_packet(ctx_, packet_);
	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		if (audio_errors_++ % 100 == 0)
			elog("audio: packet rejected by the decoder (%d), %u so far", ret, audio_errors_);
		return ret == AVERROR_INVALIDDATA;  // one bad 20 ms packet is not fatal
	}

	ret = avcodec_receive_frame(ctx_, frame_);
	if (ret == AVERROR(EAGAIN))
		return true;
	if (ret < 0) {
		if (audio_errors_++ % 100 == 0)
			elog("audio: decode error %d, %u so far", ret, audio_errors_);
		return ret == AVERROR_INVALIDDATA;
	}

	const enum audio_format format = convert_sample_format(frame_->format);
	if (format == AUDIO_FORMAT_UNKNOWN) {
		elog("audio: unsupported sample format %d", frame_->format);
		return false;
	}

	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		out->data[i] = frame_->data[i];
	out->samples_per_sec = (uint32_t)frame_->sample_rate;
	out->frames = (uint32_t)frame_->nb_samples;
	out->format = format;
	out->speakers = convert_speaker_layout(frame_->ch_layout.nb_channels);

	*got_output = true;
	return true;
}
