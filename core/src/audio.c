/*
 * audio.c — giải mã audio Opus (FFmpeg libavcodec) + phát ra loa desktop.
 *
 * Backend phát: miniaudio (cross-platform — CoreAudio trên macOS, WASAPI trên Windows,
 * ALSA/PulseAudio trên Linux). Trước đây là ALSA thuần (Linux-only); miniaudio cho phép libcore
 * build và phát audio trên cả ba nền tảng mà không đụng client.c.
 *
 * Luồng: audio_packet (Opus) → avcodec decode (FLTP) → swresample (S16 interleaved) → ghi vào
 * ring buffer (ma_pcm_rb). Thiết bị miniaudio kéo (pull) từ ring trong data callback. Ghi vào
 * ring là BLOCKING khi đầy — chính cơ chế này pace theo tốc độ phát (giống snd_pcm_writei cũ) nên
 * backlog phía mạng tự bị chặn bởi backpressure TCP.
 *
 * Server encode Opus bằng MediaCodec; ta tự tổng hợp OpusHead từ audio_meta làm extradata cho
 * decoder (gói CONFIG của server bỏ qua).
 */
#include "rc_internal.h"

#include "miniaudio.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Dung lượng ring ~80ms: đủ hấp thụ jitter Wi-Fi/scheduler nhưng không cộng trễ đáng kể cho
 * gaming; khi ring đầy, ghi chặn lại → server bị pace qua backpressure TCP. */
#define RC_AUDIO_BUFFER_MS 80

struct rc_audio {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwrContext *swr;

    ma_device device;
    ma_pcm_rb rb;
    int device_ok; /* device đã init (cần uninit khi destroy) */
    int rb_ok;     /* ring buffer đã init */
    int started;   /* ma_device_start đã gọi */

    int channels;
    int sample_rate;

    int16_t *out; /* buffer S16 interleaved tái sử dụng cho swr_convert */
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

/* data callback của miniaudio: kéo S16 interleaved từ ring buffer ra thiết bị phát; thiếu dữ
 * liệu (underrun) thì phát im lặng. Chạy trên thread audio nội bộ của miniaudio. */
static void audio_data_cb(ma_device *dev, void *out, const void *in, ma_uint32 frame_count) {
    (void)in;
    rc_audio *a = (rc_audio *)dev->pUserData;
    ma_uint8 *dst = (ma_uint8 *)out;
    ma_uint32 bpf = (ma_uint32)a->channels * sizeof(int16_t);
    ma_uint32 remaining = frame_count;

    while (remaining > 0) {
        ma_uint32 n = remaining;
        void *pread = NULL;
        if (ma_pcm_rb_acquire_read(&a->rb, &n, &pread) != MA_SUCCESS || n == 0) break;
        memcpy(dst, pread, (size_t)n * bpf);
        ma_pcm_rb_commit_read(&a->rb, n);
        dst += (size_t)n * bpf;
        remaining -= n;
    }
    if (remaining > 0) memset(dst, 0, (size_t)remaining * bpf); /* underrun → im lặng */
}

/* Ghi S16 interleaved vào ring, CHẶN tới khi hết dữ liệu (ring đầy thì ngủ ngắn rồi thử lại —
 * chính cơ chế pace server). Trả về khi đã ghi hết hoặc device/ring lỗi. */
static void rb_write_blocking(rc_audio *a, const int16_t *buf, int frames) {
    ma_uint32 bpf = (ma_uint32)a->channels * sizeof(int16_t);
    while (frames > 0) {
        ma_uint32 n = (ma_uint32)frames;
        void *pw = NULL;
        if (ma_pcm_rb_acquire_write(&a->rb, &n, &pw) != MA_SUCCESS) return;
        if (n == 0) {
            /* Ring đầy: ngủ ~2ms cho data callback rút bớt rồi thử lại (pace theo phát). */
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 2 * 1000 * 1000};
            nanosleep(&ts, NULL);
            continue;
        }
        memcpy(pw, buf, (size_t)n * bpf);
        ma_pcm_rb_commit_write(&a->rb, n);
        buf += (size_t)n * a->channels;
        frames -= (int)n;
    }
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

    /* Ring buffer S16 interleaved ~RC_AUDIO_BUFFER_MS; tối thiểu vài period để tránh underrun. */
    ma_uint32 rb_frames = (ma_uint32)((long)a->sample_rate * RC_AUDIO_BUFFER_MS / 1000);
    if (rb_frames < 2048) rb_frames = 2048;
    if (ma_pcm_rb_init(ma_format_s16, (ma_uint32)a->channels, rb_frames, NULL, NULL, &a->rb) !=
        MA_SUCCESS) {
        fprintf(stderr, "[core] audio: không cấp được ring buffer — bỏ qua audio\n");
        goto fail;
    }
    a->rb_ok = 1;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = (ma_uint32)a->channels;
    cfg.sampleRate = (ma_uint32)a->sample_rate;
    cfg.dataCallback = audio_data_cb;
    cfg.pUserData = a;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    if (ma_device_init(NULL, &cfg, &a->device) != MA_SUCCESS) {
        fprintf(stderr, "[core] audio: không mở được thiết bị phát — bỏ qua audio\n");
        goto fail;
    }
    a->device_ok = 1;
    if (ma_device_start(&a->device) != MA_SUCCESS) {
        fprintf(stderr, "[core] audio: không start được thiết bị phát — bỏ qua audio\n");
        goto fail;
    }
    a->started = 1;

    fprintf(stderr, "[core] audio: phát Opus %dHz %dch qua miniaudio (%s)\n", a->sample_rate,
            a->channels, ma_get_backend_name(a->device.pContext->backend));
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
    if (a->started) ma_device_stop(&a->device);
    if (a->device_ok) ma_device_uninit(&a->device);
    if (a->rb_ok) ma_pcm_rb_uninit(&a->rb);
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
            rb_write_blocking(a, a->out, got);
            a->frames_played += got;
        }
        av_frame_unref(a->frame);
    }
    return RC_OK;
}
