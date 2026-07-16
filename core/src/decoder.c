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
#include <libavutil/pixfmt.h>

#include <stdlib.h>

struct rc_decoder {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    AVFrame *sw_frame;             /* đích hwdownload khi decode hw */
    AVBufferRef *hw_ctx;           /* device context hw; NULL = software */
    enum AVPixelFormat hw_pix_fmt; /* pix fmt hw tương ứng (VAAPI/CUDA); NONE nếu sw */
    int emitted_unsupported;       /* để không spam log format lạ */
};

/* Các backend hw decode thử theo thứ tự: VAAPI (Intel/AMD) rồi CUDA/NVDEC (NVIDIA). */
static const struct {
    enum AVHWDeviceType type;
    enum AVPixelFormat pix_fmt;
} HW_KINDS[] = {
    {AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI},
    {AV_HWDEVICE_TYPE_CUDA, AV_PIX_FMT_CUDA},
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

/* get_format callback: chọn pix fmt hw của backend đã mở, ngược lại format sw đầu tiên. */
static enum AVPixelFormat pick_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts) {
    const rc_decoder *d = ctx->opaque;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == d->hw_pix_fmt) return *p;
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

rc_decoder *rc_decoder_create(rc_codec codec, int hw) {
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

    /* Thử từng backend hw nếu được yêu cầu; không có → im lặng dùng software. */
    d->hw_pix_fmt = AV_PIX_FMT_NONE;
    if (hw) {
        for (size_t k = 0; k < sizeof HW_KINDS / sizeof HW_KINDS[0]; k++) {
            d->hw_ctx = try_hw_device(HW_KINDS[k].type);
            if (d->hw_ctx) {
                d->hw_pix_fmt = HW_KINDS[k].pix_fmt;
                d->ctx->hw_device_ctx = av_buffer_ref(d->hw_ctx);
                d->ctx->opaque = d;
                d->ctx->get_format = pick_hw_format;
                break;
            }
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
    return d && d->hw_ctx != NULL;
}

const char *rc_decoder_hw_name(const rc_decoder *d) {
    if (!d || !d->hw_ctx) return NULL;
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

    d->pkt->data = (uint8_t *)data;
    d->pkt->size = (int)len;
    d->pkt->pts = pts_us;
    d->pkt->dts = pts_us;

    int r = avcodec_send_packet(d->ctx, d->pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) return RC_ERR_DECODE;

    for (;;) {
        r = avcodec_receive_frame(d->ctx, d->frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return RC_ERR_DECODE;

        /* Frame hw (VAAPI/CUDA) nằm trên GPU → download về CPU (NV12). Zero-copy: phase sau. */
        AVFrame *out = d->frame;
        if (d->hw_ctx && d->frame->format == d->hw_pix_fmt) {
            av_frame_unref(d->sw_frame);
            if (av_hwframe_transfer_data(d->sw_frame, d->frame, 0) < 0) {
                av_frame_unref(d->frame);
                return RC_ERR_DECODE;
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
            cb(&f, user);
        }
        av_frame_unref(d->frame);
    }
    return RC_OK;
}
