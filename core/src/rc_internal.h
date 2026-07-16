/*
 * Định nghĩa nội bộ dùng chung giữa các .c của libcore. KHÔNG cài đặt front-end.
 * Hằng số giao thức phải khớp docs/PROTOCOL.md và mã Java phía server.
 */
#ifndef RC_INTERNAL_H
#define RC_INTERNAL_H

#include "rc/rc_client.h"
#include <stdatomic.h>

/* ---- Hằng số giao thức (xem docs/PROTOCOL.md) ---- */
#define RC_META_MAGIC 0x52434E31u /* "RCN1" */
#define RC_PROTO_VERSION 1

#define RC_CODEC_ID_H264 0x68323634u /* "h264" */
#define RC_CODEC_ID_H265 0x68323635u /* "h265" */
#define RC_CODEC_ID_AV1 0x00617631u  /* "\0av1" */

#define RC_AUDIO_MAGIC 0x52434E41u    /* "RCNA" */
#define RC_ACODEC_ID_NONE 0x00000000u /* audio không khả dụng */
#define RC_ACODEC_ID_OPUS 0x6F707573u /* "opus" */
#define RC_ACODEC_ID_AAC 0x00616163u  /* "\0aac" */
#define RC_ACODEC_ID_RAW 0x00726177u  /* "\0raw" */

#define RC_DEVICE_NAME_LEN 64

/* Cờ trong pts_flags của video_packet */
#define RC_PKT_FLAG_CONFIG (1ull << 63)
#define RC_PKT_FLAG_KEYFRAME (1ull << 62)
#define RC_PKT_PTS_MASK ((1ull << 62) - 1)

/* Control message types */
#define RC_CTRL_MOUSE_MOTION 0
#define RC_CTRL_MOUSE_BUTTON 1
#define RC_CTRL_SCROLL 2
#define RC_CTRL_KEY 3
#define RC_CTRL_TEXT 4
#define RC_CTRL_DEVICE_ACTION 5

#define RC_LOCALABSTRACT_NAME "rigcontrol"
#define RC_DEFAULT_TCP_PORT 27183

/* ---- Cấu trúc phiên ---- */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint32_t codec_id;
    uint16_t width;
    uint16_t height;
    char device_name[RC_DEVICE_NAME_LEN];
} rc_device_meta;

typedef struct {
    uint32_t magic;
    uint32_t codec_id; /* RC_ACODEC_ID_*; NONE = audio không khả dụng */
    uint32_t sample_rate;
    uint8_t channels;
} rc_audio_meta;

struct rc_client {
    rc_config cfg; /* copy sâu (serial/tcp_addr được strdup) */
    char *serial_owned;
    char *tcp_addr_owned;

    rc_frame_cb frame_cb;
    void *frame_user;
    rc_status_cb status_cb;
    void *status_user;

    /* socket fd; -1 khi chưa mở */
    int video_fd;
    int audio_fd;
    int control_fd;
    int listen_fd; /* dùng cho reverse tunnel (USB) */

    rc_device_meta meta;
    atomic_int have_meta;

    int server_pid;        /* pid tiến trình `adb shell app_process` (USB); 0 = không có */
    char socket_name[64];  /* tên localabstract riêng của session (đa phiên) */

    atomic_int running;
    /* Thread handles được khai báo trong client.c (pthread) để tránh include ở header này. */
    void *net_thread;   /* pthread_t*  — vòng video */
    void *audio_thread; /* pthread_t*  — vòng audio */
    void *decoder;      /* rc_decoder* (decoder.c) */
    void *audio_player; /* rc_audio* (audio.c) */
};

/* ---- Hàm nội bộ (cài đặt rải trong các .c) ---- */

/* net.c — I/O socket đầy đủ (đọc/ghi lặp tới khi đủ), big-endian helpers */
rc_status rc_net_read_full(int fd, void *buf, size_t len);
rc_status rc_net_write_full(int fd, const void *buf, size_t len);
int rc_net_listen_loopback(int *out_port);          /* trả fd listen, set *out_port */
int rc_net_accept(int listen_fd);                   /* trả fd đã accept */
int rc_net_connect_tcp(const char *host, int port); /* trả fd đã connect */

/* adb.c — bọc lệnh adb */
/* adb connect tới "ip:port" (wireless adb) rồi xác minh thiết bị online. */
rc_status rc_adb_connect(const char *addr);
rc_status rc_adb_push(const char *serial, const char *local, const char *remote);
rc_status rc_adb_reverse(const char *serial, const char *remote, int local_port);
rc_status rc_adb_reverse_remove(const char *serial, const char *remote);
/* Chạy server qua app_process (nền, không chặn); trả pid tiến trình adb qua *out_pid.
 * tcp_port > 0 → server listen TCP cổng đó (LAN); ngược lại connect localabstract socket_name. */
rc_status rc_adb_run_server(const char *serial, const rc_config *cfg, const char *socket_name,
                            int tcp_port, int *out_pid);
#define RC_SERVER_REMOTE_PATH "/data/local/tmp/rc-server"

/* server_deploy.c — đẩy + chạy rc-server, thiết lập tunnel, trả về video/control fd */
rc_status rc_server_deploy(rc_client *c);
void rc_server_teardown(rc_client *c);

/* demuxer.c — đọc device_meta / audio_meta rồi từng packet (khung dùng chung) */
rc_status rc_demux_read_meta(int fd, rc_device_meta *out);
rc_status rc_demux_read_audio_meta(int fd, rc_audio_meta *out);
/* Đọc 1 packet vào *buf (tái sử dụng giữa các lần gọi, realloc khi thiếu — caller sở hữu và
 * free sau vòng đọc; khởi tạo *buf=NULL, *cap=0). Set *out_len, cờ và *pts_us. */
rc_status rc_demux_read_packet(int fd, uint8_t **buf, size_t *cap, size_t *out_len, int *is_config,
                               int *is_key, int64_t *pts_us);

/* decoder.c — bọc FFmpeg libavcodec */
typedef struct rc_decoder rc_decoder;
/* hw != 0 → thử hardware decode VAAPI (output NV12 qua hwdownload); lỗi → fallback software. */
rc_decoder *rc_decoder_create(rc_codec codec, int hw);
int rc_decoder_is_hw(const rc_decoder *d);
void rc_decoder_destroy(rc_decoder *d);
/* Nạp packet, nếu ra frame thì gọi cb. is_config=1 cho packet SPS/PPS. */
rc_status rc_decoder_feed(rc_decoder *d, const uint8_t *data, size_t len, int is_config,
                          int64_t pts_us, rc_frame_cb cb, void *user);

/* audio.c — giải mã (FFmpeg) + phát (miniaudio) audio Opus/AAC/PCM */
typedef struct rc_audio rc_audio;
rc_audio *rc_audio_create(const rc_audio_meta *meta);
void rc_audio_destroy(rc_audio *a);
/* Nạp 1 audio packet đã đọc; giải mã và đẩy PCM ra thiết bị phát. */
rc_status rc_audio_feed(rc_audio *a, const uint8_t *data, size_t len, int is_config,
                        int64_t pts_us);

/* control_msg.c — serialize event thành byte control rồi ghi ra control_fd */
rc_status rc_control_send(rc_client *c, const uint8_t *buf, size_t len);

/* client.c — phát status callback tiện dụng */
void rc_emit_status(rc_client *c, rc_status code, const char *msg);

#endif /* RC_INTERNAL_H */
