/*
 * audio.c — giải mã audio Opus (FFmpeg libavcodec) + phát ra loa desktop.
 *
 * Backend phát (Ubuntu MVP): ALSA (libasound), chế độ blocking để tự pace theo tốc độ phát.
 * Trừu tượng hoá qua rc_audio nên phase sau có thể thay bằng miniaudio (cross-platform) mà không
 * đụng client.c.
 *
 * Luồng: audio_packet (Opus) → avcodec decode (FLTP) → swresample (S16 interleaved) →
 * snd_pcm_writei. Server encode Opus bằng MediaCodec; ta tự tổng hợp OpusHead từ audio_meta làm
 * extradata cho decoder (gói CONFIG của server bỏ qua).
 */
#include "rc_internal.h"

#include <alsa/asoundlib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Buffer ALSA ~60ms: đủ hấp thụ jitter Wi-Fi/scheduler nhưng không cộng trễ đáng kể cho
 * gaming; backlog phía mạng tự bị chặn bởi backpressure TCP (writei blocking pace server). */
#define ALSA_LATENCY_US 60000

struct rc_audio {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwrContext *swr;
    snd_pcm_t *pcm;
    int channels;
    int sample_rate;

    int16_t *out; /* buffer S16 interleaved tái sử dụng */
    int out_cap;  /* số mẫu (frame) đã cấp */
    long frames_played;
};

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Tổng hợp OpusHead (19 byte) làm extradata cho decoder Opus. */
static int set_opus_extradata(AVCodecContext *ctx, int channels, int sample_rate) {
    const int size = 19;
    ctx->extradata = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ctx->extradata) return -1;
    uint8_t *h = ctx->extradata;
    memcpy(h, "OpusHead", 8);
    h[8] = 1;                      /* version */
    h[9] = (uint8_t)channels;      /* channel count */
    put_le16(h + 10, 0);           /* pre-skip */
    put_le32(h + 12, sample_rate); /* input sample rate */
    put_le16(h + 16, 0);           /* output gain */
    h[18] = 0;                     /* channel mapping family 0 (mono/stereo) */
    ctx->extradata_size = size;
    return 0;
}

static snd_pcm_t *alsa_open(int channels, int rate) {
    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) return NULL;
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           (unsigned)channels, (unsigned)rate, 1 /*resample*/,
                           ALSA_LATENCY_US) < 0) {
        snd_pcm_close(pcm);
        return NULL;
    }
    return pcm;
}

rc_audio *rc_audio_create(const rc_audio_meta *meta) {
    if (!meta || meta->codec_id == RC_ACODEC_ID_NONE) return NULL; /* audio tắt */
    if (meta->codec_id != RC_ACODEC_ID_OPUS) {
        fprintf(stderr, "[core] audio: chỉ hỗ trợ Opus ở Phase 5 (codec=0x%08x)\n", meta->codec_id);
        return NULL;
    }

    rc_audio *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->channels = meta->channels ? meta->channels : 2;
    a->sample_rate = meta->sample_rate ? (int)meta->sample_rate : 48000;

    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!dec) goto fail;
    a->ctx = avcodec_alloc_context3(dec);
    if (!a->ctx) goto fail;
    a->ctx->sample_rate = a->sample_rate;
    av_channel_layout_default(&a->ctx->ch_layout, a->channels);
    if (set_opus_extradata(a->ctx, a->channels, a->sample_rate) < 0) goto fail;
    if (avcodec_open2(a->ctx, dec, NULL) < 0) goto fail;

    a->pkt = av_packet_alloc();
    a->frame = av_frame_alloc();
    if (!a->pkt || !a->frame) goto fail;

    a->pcm = alsa_open(a->channels, a->sample_rate);
    if (!a->pcm) {
        fprintf(stderr, "[core] audio: không mở được thiết bị ALSA — bỏ qua audio\n");
        goto fail;
    }

    fprintf(stderr, "[core] audio: phát Opus %dHz %dch qua ALSA\n", a->sample_rate, a->channels);
    return a;

fail:
    rc_audio_destroy(a);
    return NULL;
}

void rc_audio_destroy(rc_audio *a) {
    if (!a) return;
    if (getenv("RC_AUDIO_DEBUG") && a->frames_played)
        fprintf(stderr, "[core] audio: đã phát %ld mẫu (%.1fs)\n", a->frames_played,
                (double)a->frames_played / (a->sample_rate ? a->sample_rate : 48000));
    if (a->pcm) {
        snd_pcm_drain(a->pcm);
        snd_pcm_close(a->pcm);
    }
    if (a->swr) swr_free(&a->swr);
    if (a->frame) av_frame_free(&a->frame);
    if (a->pkt) av_packet_free(&a->pkt);
    if (a->ctx) avcodec_free_context(&a->ctx);
    free(a->out);
    free(a);
}

static int ensure_swr(rc_audio *a, const AVFrame *frame) {
    if (a->swr) return 0;
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, a->channels);
    int r = swr_alloc_set_opts2(&a->swr, &out_layout, AV_SAMPLE_FMT_S16, a->sample_rate,
                                &frame->ch_layout, (enum AVSampleFormat)frame->format,
                                frame->sample_rate, 0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (r < 0 || !a->swr) return -1;
    if (swr_init(a->swr) < 0) return -1;
    return 0;
}

static void alsa_write(rc_audio *a, const int16_t *buf, int frames) {
    while (frames > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(a->pcm, buf, (snd_pcm_uframes_t)frames);
        if (w < 0) {
            w = snd_pcm_recover(a->pcm, (int)w, 1 /*silent*/);
            if (w < 0) return; /* lỗi không phục hồi được: bỏ khối này */
            continue;
        }
        buf += w * a->channels;
        frames -= (int)w;
    }
}

rc_status rc_audio_feed(rc_audio *a, const uint8_t *data, size_t len, int is_config,
                        int64_t pts_us) {
    (void)pts_us;
    if (!a) return RC_ERR_INVALID_ARG;
    if (is_config) return RC_OK; /* dùng OpusHead tự tổng hợp, bỏ CONFIG của server */

    a->pkt->data = (uint8_t *)data;
    a->pkt->size = (int)len;
    if (avcodec_send_packet(a->ctx, a->pkt) < 0) return RC_ERR_DECODE;

    for (;;) {
        int r = avcodec_receive_frame(a->ctx, a->frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return RC_ERR_DECODE;

        if (ensure_swr(a, a->frame) < 0) {
            av_frame_unref(a->frame);
            return RC_ERR_DECODE;
        }

        int max_out = (int)swr_get_out_samples(a->swr, a->frame->nb_samples);
        if (max_out > a->out_cap) {
            int16_t *nb = realloc(a->out, (size_t)max_out * a->channels * sizeof(int16_t));
            if (!nb) {
                av_frame_unref(a->frame);
                return RC_ERR_NOMEM;
            }
            a->out = nb;
            a->out_cap = max_out;
        }

        uint8_t *outp[1] = {(uint8_t *)a->out};
        int got = swr_convert(a->swr, outp, max_out, (const uint8_t **)a->frame->data,
                              a->frame->nb_samples);
        if (got > 0) {
            alsa_write(a, a->out, got);
            a->frames_played += got;
        }
        av_frame_unref(a->frame);
    }
    return RC_OK;
}
