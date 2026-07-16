/*
 * audio.c — giải mã audio (FFmpeg libavcodec: Opus/AAC) + phát ra loa desktop.
 *
 * Backend phát dự kiến dùng miniaudio (single-header, cross-platform ALSA/Pulse/CoreAudio/
 * WASAPI) để mọi front-end dùng chung, không phải viết lại. miniaudio sẽ được vendor vào
 * third_party/ khi cài đặt audio ở phase tương ứng.
 *
 * Phase 0: khung + vòng đời rỗng; decode + playback hoàn thiện ở phase audio.
 */
#include "rc_internal.h"

#include <stdlib.h>

struct rc_audio {
    rc_audio_meta meta;
    /* TODO(audio): AVCodecContext *ctx; AVFrame *frame; AVPacket *pkt;
     *              ma_device device; ring buffer PCM low-latency. */
};

rc_audio *rc_audio_create(const rc_audio_meta *meta) {
    if (!meta || meta->codec_id == RC_ACODEC_ID_NONE) return NULL; /* audio tắt */
    rc_audio *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->meta = *meta;
    /* TODO(audio):
     *  - avcodec_find_decoder(AV_CODEC_ID_OPUS/AAC), avcodec_open2
     *  - ma_device_init (playback, sample_rate/channels theo meta), ma_device_start
     */
    return a;
}

void rc_audio_destroy(rc_audio *a) {
    if (!a) return;
    /* TODO(audio): ma_device_uninit, avcodec_free_context, av_frame/packet_free. */
    free(a);
}

rc_status rc_audio_feed(rc_audio *a, const uint8_t *data, size_t len, int is_config,
                        int64_t pts_us) {
    (void)a;
    (void)data;
    (void)len;
    (void)is_config;
    (void)pts_us;
    /* TODO(audio): send_packet -> receive_frame -> đẩy PCM vào ring buffer cho ma_device. */
    return RC_ERR_GENERIC;
}
