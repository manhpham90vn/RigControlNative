/*
 * decoder.c — bọc FFmpeg libavcodec giải mã H.264 (Phase 2), tối ưu low-delay.
 *
 * Server gửi các packet đã tách khung (CONFIG SPS/PPS rồi các frame). H.264 decoder đọc SPS/PPS
 * in-band nên ta feed thẳng mọi packet (kể cả config) vào avcodec_send_packet, không B-frame nên
 * gần như 1 packet vào → 1 frame ra (độ trễ thấp).
 */
#include "rc_internal.h"

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct rc_decoder {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    AVFrame *sw_frame;             /* đích hwdownload khi decode hw */
    AVBufferRef *hw_ctx;           /* device context hw; NULL = software */
    enum AVPixelFormat hw_pix_fmt; /* pix fmt hw tương ứng (VAAPI/CUDA); NONE nếu sw */
    int hw_failed;                 /* hwaccel init lỗi → đã rơi hẳn về software */
    int emitted_unsupported;       /* để không spam log format lạ */
};

/* Các backend hw decode thử theo thứ tự: CUDA/NVDEC (NVIDIA) rồi VAAPI (Intel/AMD),
 * cuối cùng fallback software nếu không backend nào mở được. */
static const struct {
    enum AVHWDeviceType type;
    enum AVPixelFormat pix_fmt;
} HW_KINDS[] = {
    {AV_HWDEVICE_TYPE_CUDA, AV_PIX_FMT_CUDA},
    {AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI},
};

static AVBufferRef *try_hw_device(enum AVHWDeviceType type) {
    AVBufferRef *ref = NULL;
    if (av_hwdevice_ctx_create(&ref, type, NULL, NULL, 0) == 0) return ref;
    if (type == AV_HWDEVICE_TYPE_VAAPI) {
        /* Máy nhiều GPU: node mặc định có thể là GPU không có VAAPI (vd NVIDIA) → thử
         * lần lượt từng render node để tìm iGPU/GPU có driver VAAPI. */
        for (int i = 128; i < 132; i++) {
            char node[32];
            snprintf(node, sizeof node, "/dev/dri/renderD%d", i);
            if (av_hwdevice_ctx_create(&ref, type, node, NULL, 0) == 0) return ref;
        }
    }
    return NULL;
}

/* get_format callback: chọn pix fmt hw của backend đã mở, ngược lại format sw đầu tiên.
 * Khi hwaccel init lỗi FFmpeg loại fmt hw khỏi danh sách rồi gọi lại — lúc đó phải trả
 * format software; trả fmts[0] (thường là fmt hw khác: vdpau/vulkan/cuda) chỉ sinh thêm
 * một vòng lỗi "does not match the type of the provided device context" mỗi lần đổi
 * kích thước. Ghi nhớ thất bại để các lần gọi sau đi thẳng vào software. */
static enum AVPixelFormat pick_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts) {
    rc_decoder *d = ctx->opaque;
    if (!d->hw_failed) {
        for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
            if (*p == d->hw_pix_fmt) return *p;
        d->hw_failed = 1;
        fprintf(stderr, "[core] hw decode không dùng được với stream này → software\n");
    }
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return fmts[0];
}

static enum AVCodecID av_codec_id(rc_codec codec) {
    switch (codec) {
    case RC_CODEC_H265:
        return AV_CODEC_ID_HEVC;
    case RC_CODEC_AV1:
        return AV_CODEC_ID_AV1;
    case RC_CODEC_H264:
    default:
        return AV_CODEC_ID_H264;
    }
}

/* env RC_HWDEC=off/0/no/sw → ép software decode (escape hatch khi driver hw lỗi). */
static int hwdec_env_disabled(void) {
    const char *e = getenv("RC_HWDEC");
    if (!e || !*e) return 0;
    return strcmp(e, "off") == 0 || strcmp(e, "0") == 0 || strcmp(e, "no") == 0 ||
           strcmp(e, "sw") == 0;
}

rc_decoder *rc_decoder_create(rc_codec codec, int allow_hw) {
    rc_decoder *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    const AVCodec *dec = avcodec_find_decoder(av_codec_id(codec));
    if (!dec) goto fail;

    d->ctx = avcodec_alloc_context3(dec);
    if (!d->ctx) goto fail;

    /* Low-delay: xuất frame ngay, không chờ reorder; 1 thread để tránh trễ frame-threading. */
    d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    d->ctx->thread_count = 1;

    /* Thử hw decode lần lượt từng backend (trừ khi bị tắt); không backend nào mở được →
     * im lặng dùng software. */
    d->hw_pix_fmt = AV_PIX_FMT_NONE;
    if (hwdec_env_disabled()) allow_hw = 0;
    for (size_t k = 0; allow_hw && k < sizeof HW_KINDS / sizeof HW_KINDS[0]; k++) {
        d->hw_ctx = try_hw_device(HW_KINDS[k].type);
        if (d->hw_ctx) {
            d->hw_pix_fmt = HW_KINDS[k].pix_fmt;
            d->ctx->hw_device_ctx = av_buffer_ref(d->hw_ctx);
            d->ctx->opaque = d;
            d->ctx->get_format = pick_hw_format;
            /* Encoder Android (nhất là Qualcomm) mặc định H.264 Baseline (66) — VAAPI không
             * có mapping cho Baseline thuần, chỉ Constrained Baseline. Hai profile chỉ khác
             * FMO/ASO mà không encoder thực tế nào dùng → cho phép mismatch để decode bằng
             * profile gần nhất thay vì rơi về software. */
            d->ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
            break;
        }
    }

    if (avcodec_open2(d->ctx, dec, NULL) < 0) goto fail;

    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->sw_frame = av_frame_alloc();
    if (!d->pkt || !d->frame || !d->sw_frame) goto fail;

    return d;

fail:
    rc_decoder_destroy(d);
    return NULL;
}

int rc_decoder_is_hw(const rc_decoder *d) {
    return d && d->hw_ctx != NULL && !d->hw_failed;
}

const char *rc_decoder_hw_name(const rc_decoder *d) {
    if (!d || !d->hw_ctx || d->hw_failed) return NULL;
    return av_hwdevice_get_type_name(((AVHWDeviceContext *)d->hw_ctx->data)->type);
}

void rc_decoder_destroy(rc_decoder *d) {
    if (!d) return;
    if (d->sw_frame) av_frame_free(&d->sw_frame);
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt) av_packet_free(&d->pkt);
    if (d->ctx) avcodec_free_context(&d->ctx);
    if (d->hw_ctx) av_buffer_unref(&d->hw_ctx);
    free(d);
}

static rc_pixfmt map_pixfmt(int fmt, int *ok) {
    *ok = 1;
    switch (fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return RC_PIX_I420;
    case AV_PIX_FMT_NV12:
        return RC_PIX_NV12;
    default:
        *ok = 0;
        return RC_PIX_I420;
    }
}

rc_status rc_decoder_feed(rc_decoder *d, const uint8_t *data, size_t len, int is_config,
                          int64_t pts_us, rc_frame_cb cb, void *user) {
    (void)is_config; /* H.264 đọc SPS/PPS in-band; feed thẳng mọi packet */
    if (!d || !data || len == 0) return RC_ERR_INVALID_ARG;

    /* Mốc đo trễ pipeline desktop (rc_frame.recv_ms): decode đồng bộ không B-frame nên frame
     * ra thuộc đúng packet vừa vào — lấy giờ một lần cho cả batch. */
    struct timespec ts_in;
    clock_gettime(CLOCK_MONOTONIC, &ts_in);
    int64_t recv_ms = (int64_t)ts_in.tv_sec * 1000 + ts_in.tv_nsec / 1000000;

    d->pkt->data = (uint8_t *)data;
    d->pkt->size = (int)len;
    d->pkt->pts = pts_us;
    d->pkt->dts = pts_us;

    int r = avcodec_send_packet(d->ctx, d->pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) {
        char errbuf[64];
        av_strerror(r, errbuf, sizeof errbuf);
        fprintf(stderr, "[core] send_packet lỗi (len=%zu cfg=%d): %s\n", len, is_config, errbuf);
        return RC_ERR_DECODE;
    }

    for (;;) {
        r = avcodec_receive_frame(d->ctx, d->frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return RC_ERR_DECODE;

        /* Frame hw (VAAPI/CUDA) nằm trên GPU → download về CPU (NV12). Buffer đích giữ lại
         * giữa các frame (chỉ cấp lại khi đổi kích thước) để khỏi alloc/free vài MB mỗi frame.
         * Zero-copy: phase sau. */
        AVFrame *out = d->frame;
        if (d->hw_ctx && d->frame->format == d->hw_pix_fmt) {
            if (!d->sw_frame->buf[0] || d->sw_frame->width != d->frame->width ||
                d->sw_frame->height != d->frame->height) {
                av_frame_unref(d->sw_frame);
                if (av_hwframe_transfer_data(d->sw_frame, d->frame, 0) < 0) {
                    av_frame_unref(d->frame);
                    return RC_ERR_DECODE;
                }
            } else if (av_hwframe_transfer_data(d->sw_frame, d->frame, 0) < 0) {
                /* Transfer vào buffer tái sử dụng lỗi → thử lại với buffer mới. */
                av_frame_unref(d->sw_frame);
                if (av_hwframe_transfer_data(d->sw_frame, d->frame, 0) < 0) {
                    av_frame_unref(d->frame);
                    return RC_ERR_DECODE;
                }
            }
            av_frame_copy_props(d->sw_frame, d->frame); /* giữ pts/color info */
            out = d->sw_frame;
        }

        int ok = 0;
        rc_pixfmt pf = map_pixfmt(out->format, &ok);
        if (!ok) {
            if (!d->emitted_unsupported) d->emitted_unsupported = 1;
            av_frame_unref(d->frame);
            return RC_ERR_DECODE;
        }

        if (cb) {
            rc_frame f = {0};
            f.width = out->width;
            f.height = out->height;
            f.format = pf;
            f.full_range =
                out->color_range == AVCOL_RANGE_JPEG || out->format == AV_PIX_FMT_YUVJ420P;
            f.bt709 = out->colorspace == AVCOL_SPC_BT709;
            for (int i = 0; i < 4; i++) {
                f.data[i] = out->data[i];
                f.linesize[i] = out->linesize[i];
            }
            int64_t pts = out->pts;
            if (pts == AV_NOPTS_VALUE) pts = out->best_effort_timestamp;
            f.pts_us = (pts == AV_NOPTS_VALUE) ? 0 : pts;
            f.recv_ms = recv_ms;
            cb(&f, user);
        }
        av_frame_unref(d->frame);
    }
    return RC_OK;
}
