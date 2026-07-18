/*
 * client.c — orchestrator + cài đặt C API công khai.
 *
 * Phase 0: vòng đời (create/destroy/callbacks/config) hoàn chỉnh; start/stop dựng thread
 * và gọi các module (deploy/demux/decode) sẽ được hoàn thiện ở Phase 2. Các stub được đánh
 * dấu TODO(phaseN).
 */
#include "rc_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static char *dup_or_null(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

const char *rc_status_str(rc_status code) {
    switch (code) {
    case RC_OK:
        return "OK";
    case RC_ERR_GENERIC:
        return "lỗi chung";
    case RC_ERR_INVALID_ARG:
        return "tham số không hợp lệ";
    case RC_ERR_NO_DEVICE:
        return "không tìm thấy thiết bị";
    case RC_ERR_ADB:
        return "lỗi adb";
    case RC_ERR_CONNECT:
        return "lỗi kết nối";
    case RC_ERR_PROTOCOL:
        return "lỗi giao thức";
    case RC_ERR_DECODE:
        return "lỗi giải mã";
    case RC_ERR_NOMEM:
        return "hết bộ nhớ";
    default:
        return "không rõ";
    }
}

void rc_emit_status(rc_client *c, rc_status code, const char *msg) {
    if (c && c->status_cb) c->status_cb(code, msg ? msg : rc_status_str(code), c->status_user);
}

rc_client *rc_client_create(const rc_config *cfg) {
    if (!cfg) return NULL;
    rc_client *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->cfg = *cfg;
    c->serial_owned = dup_or_null(cfg->serial);
    c->tcp_addr_owned = dup_or_null(cfg->tcp_addr);
    c->cfg.serial = c->serial_owned;
    c->cfg.tcp_addr = c->tcp_addr_owned;

    c->video_fd = c->audio_fd = c->control_fd = c->listen_fd = -1;
    atomic_init(&c->have_meta, 0);
    atomic_init(&c->running, 0);
    atomic_init(&c->abort_requested, 0);

    /* Tên localabstract riêng cho mỗi phiên (đa session, kể cả cùng thiết bị). */
    static atomic_int scid = 0;
    int id = atomic_fetch_add(&scid, 1);
    snprintf(c->socket_name, sizeof c->socket_name, "%s_%d_%d", RC_LOCALABSTRACT_NAME,
             (int)getpid(), id);
    return c;
}

void rc_client_set_frame_callback(rc_client *c, rc_frame_cb cb, void *user) {
    if (!c) return;
    c->frame_cb = cb;
    c->frame_user = user;
}

void rc_client_set_status_callback(rc_client *c, rc_status_cb cb, void *user) {
    if (!c) return;
    c->status_cb = cb;
    c->status_user = user;
}

/* Mô tả backend decode dễ hiểu cho UI (string literal tĩnh — an toàn trả thẳng ra ngoài). */
static const char *decoder_desc_of(const rc_decoder *d) {
    const char *hw = rc_decoder_hw_name(d);
    if (!hw) return "CPU (software)";
    if (strcmp(hw, "cuda") == 0) return "GPU NVIDIA (NVDEC)";
    if (strcmp(hw, "vaapi") == 0) return "GPU Intel/AMD (VAAPI)";
    if (strcmp(hw, "videotoolbox") == 0) return "GPU Apple (VideoToolbox)";
    return "GPU (hw)"; /* backend hw khác trong tương lai */
}

const char *rc_client_get_decoder_desc(const rc_client *c) {
    if (!c) return NULL;
    return atomic_load(&((rc_client *)c)->decoder_desc);
}

const char *rc_client_get_transport_desc(const rc_client *c) {
    if (!c) return NULL;
    return atomic_load(&((rc_client *)c)->transport_desc);
}

/* Buffer lớn dần tái sử dụng: đảm bảo *cap >= need. Trả 0 nếu hết bộ nhớ. */
static int ensure_cap(uint8_t **buf, size_t *cap, size_t need) {
    if (*cap >= need) return 1;
    uint8_t *nb = realloc(*buf, need);
    if (!nb) return 0;
    *buf = nb;
    *cap = need;
    return 1;
}

/*
 * Vòng video: đọc packet → decode → frame_cb, tới khi socket đóng hoặc dừng.
 *
 * Gói CONFIG (SPS/PPS) không đưa lẻ vào decoder (libavcodec trả INVALIDDATA cho packet
 * không có VCL NAL) mà được cache rồi GHÉP vào đầu packet kế tiếp — như scrcpy. Bản cache
 * cũng dùng để mồi lại decoder khi hardware decode lỗi giữa chừng: hủy decoder hw, dựng
 * software, ghép config + packet hiện tại decode tiếp — hình phục hồi từ keyframe kế.
 */
static void *net_thread_fn(void *arg) {
    rc_client *c = arg;
    uint8_t *buf = NULL; /* buffer packet tái sử dụng (demuxer realloc khi thiếu) */
    size_t cap = 0;
    uint8_t *cfg_buf = NULL; /* bản sao gói CONFIG gần nhất */
    size_t cfg_len = 0, cfg_cap = 0;
    uint8_t *merged = NULL; /* buffer ghép config + packet (tái sử dụng) */
    size_t merged_cap = 0;
    int need_cfg = 1;    /* decoder (mới) chưa nhận config → ghép vào packet kế */
    int err_emitted = 0; /* chỉ báo lỗi decode một lần mỗi chuỗi lỗi */
    while (atomic_load(&c->running)) {
        size_t len = 0;
        int is_config = 0, is_key = 0;
        int64_t pts = 0;
        rc_status r =
            rc_demux_read_packet(c->video_fd, &buf, &cap, &len, &is_config, &is_key, &pts);
        if (r != RC_OK) {
            if (atomic_load(&c->running)) rc_emit_status(c, r, "luồng video kết thúc");
            break;
        }
        if (is_config) { /* cache lại, ghép vào packet kế tiếp (cả khi xoay màn hình) */
            if (ensure_cap(&cfg_buf, &cfg_cap, len)) {
                memcpy(cfg_buf, buf, len);
                cfg_len = len;
                need_cfg = 1;
            }
            continue;
        }

        const uint8_t *fdata = buf;
        size_t flen = len;
        if (need_cfg && cfg_len && ensure_cap(&merged, &merged_cap, cfg_len + len)) {
            memcpy(merged, cfg_buf, cfg_len);
            memcpy(merged + cfg_len, buf, len);
            fdata = merged;
            flen = cfg_len + len;
        }
        need_cfg = 0; /* config (nếu có) đã được giao cho decoder */

        r = rc_decoder_feed((rc_decoder *)c->decoder, fdata, flen, 0, pts, c->frame_cb,
                            c->frame_user);
        if (r == RC_OK) {
            err_emitted = 0;
            continue;
        }
        if (rc_decoder_is_hw((rc_decoder *)c->decoder)) {
            rc_emit_status(c, r, "hardware decode lỗi — chuyển sang CPU (software)");
            rc_decoder_destroy((rc_decoder *)c->decoder);
            c->decoder = rc_decoder_create(c->cfg.codec, 0 /* software */);
            if (!c->decoder) {
                atomic_store(&c->decoder_desc, NULL);
                rc_emit_status(c, RC_ERR_DECODE, "không dựng lại được decoder");
                break;
            }
            atomic_store(&c->decoder_desc, decoder_desc_of((rc_decoder *)c->decoder));
            /* Mồi decoder mới bằng config + packet hiện tại; P-frame thiếu tham chiếu sẽ
             * lỗi tới keyframe kế — bỏ qua kết quả. */
            if (cfg_len && ensure_cap(&merged, &merged_cap, cfg_len + len)) {
                memcpy(merged, cfg_buf, cfg_len);
                memcpy(merged + cfg_len, buf, len);
                rc_decoder_feed((rc_decoder *)c->decoder, merged, cfg_len + len, 0, pts,
                                c->frame_cb, c->frame_user);
            }
        } else if (!err_emitted) {
            err_emitted = 1;
            rc_emit_status(c, r, "lỗi giải mã (chờ keyframe kế tiếp)");
        }
    }
    free(merged);
    free(cfg_buf);
    free(buf);
    atomic_store(&c->running, 0);
    return NULL;
}

/*
 * Vòng audio: đọc audio_meta, tạo player (Opus→miniaudio) rồi feed từng packet. Nếu audio không khả
 * dụng (codec NONE / mở thiết bị lỗi) → player = NULL, chỉ drain để server không nghẽn.
 */
static void *audio_thread_fn(void *arg) {
    rc_client *c = arg;
    rc_audio_meta meta;
    if (rc_demux_read_audio_meta(c->audio_fd, &meta) != RC_OK) return NULL;

    rc_audio *player = rc_audio_create(&meta); /* NULL nếu NONE hoặc không mở được thiết bị */
    c->audio_player = player;

    uint8_t *buf = NULL; /* buffer packet tái sử dụng */
    size_t cap = 0;
    while (atomic_load(&c->running)) {
        size_t len = 0;
        int is_config = 0, is_key = 0;
        int64_t pts = 0;
        if (rc_demux_read_packet(c->audio_fd, &buf, &cap, &len, &is_config, &is_key, &pts) != RC_OK)
            break;
        if (player) rc_audio_feed(player, buf, len, is_config, pts);
    }
    free(buf);
    return NULL;
}

/* codec_id trong device_meta → rc_codec; -1 nếu không nhận diện được. */
static int codec_from_meta(uint32_t codec_id) {
    switch (codec_id) {
    case RC_CODEC_ID_H264:
        return RC_CODEC_H264;
    case RC_CODEC_ID_H265:
        return RC_CODEC_H265;
    case RC_CODEC_ID_AV1:
        return RC_CODEC_AV1;
    default:
        return -1;
    }
}

rc_status rc_client_start(rc_client *c) {
    if (!c) return RC_ERR_INVALID_ARG;
    if (atomic_load(&c->running)) return RC_OK;

    rc_status r = rc_server_deploy(c);
    if (r != RC_OK) {
        rc_server_teardown(c);
        return r;
    }

    r = rc_demux_read_meta(c->video_fd, &c->meta);
    if (r != RC_OK) {
        rc_emit_status(c, r, "đọc device_meta thất bại");
        rc_server_teardown(c);
        return r;
    }
    if (c->meta.version != RC_PROTO_VERSION) {
        rc_emit_status(c, RC_ERR_PROTOCOL, "phiên bản protocol của server không khớp");
        rc_server_teardown(c);
        return RC_ERR_PROTOCOL;
    }
    /* Decoder dựng theo codec server THỰC SỰ dùng (device_meta), không theo cfg gửi đi. */
    int meta_codec = codec_from_meta(c->meta.codec_id);
    if (meta_codec < 0) {
        rc_emit_status(c, RC_ERR_PROTOCOL, "codec trong device_meta không nhận diện được");
        rc_server_teardown(c);
        return RC_ERR_PROTOCOL;
    }
    c->cfg.codec = (rc_codec)meta_codec;
    atomic_store(&c->have_meta, 1);
    rc_emit_status(c, RC_OK, c->meta.device_name);

    c->decoder = rc_decoder_create(c->cfg.codec, 1 /* thử hw trước */);
    if (!c->decoder) {
        rc_emit_status(c, RC_ERR_DECODE, "khởi tạo decoder thất bại");
        rc_server_teardown(c);
        return RC_ERR_DECODE;
    }
    {
        const char *desc = decoder_desc_of((rc_decoder *)c->decoder);
        atomic_store(&c->decoder_desc, desc);
        char msg[64];
        snprintf(msg, sizeof msg, "decoder: %s", desc);
        rc_emit_status(c, RC_OK, msg);
    }

    atomic_store(&c->running, 1);

    pthread_t *nt = malloc(sizeof *nt);
    if (!nt || pthread_create(nt, NULL, net_thread_fn, c) != 0) {
        free(nt);
        atomic_store(&c->running, 0);
        rc_decoder_destroy((rc_decoder *)c->decoder);
        c->decoder = NULL;
        rc_server_teardown(c);
        return RC_ERR_GENERIC;
    }
    c->net_thread = nt;

    if (c->cfg.audio && c->audio_fd >= 0) {
        pthread_t *at = malloc(sizeof *at);
        if (at && pthread_create(at, NULL, audio_thread_fn, c) == 0) {
            c->audio_thread = at;
        } else {
            free(at); /* audio là phụ; không làm hỏng cả phiên */
        }
    }
    return RC_OK;
}

void rc_client_abort(rc_client *c) {
    if (!c) return;
    atomic_store(&c->abort_requested, 1);
}

void rc_client_stop(rc_client *c) {
    if (!c) return;
    atomic_store(&c->abort_requested, 1);
    atomic_store(&c->running, 0);

    /* Đánh thức thread đang block ở read bằng shutdown; đóng hẳn fd ở teardown. */
    if (c->video_fd >= 0) shutdown(c->video_fd, SHUT_RDWR);
    if (c->audio_fd >= 0) shutdown(c->audio_fd, SHUT_RDWR);
    if (c->control_fd >= 0) shutdown(c->control_fd, SHUT_RDWR);

    if (c->net_thread) {
        pthread_join(*(pthread_t *)c->net_thread, NULL);
        free(c->net_thread);
        c->net_thread = NULL;
    }
    if (c->audio_thread) {
        pthread_join(*(pthread_t *)c->audio_thread, NULL);
        free(c->audio_thread);
        c->audio_thread = NULL;
    }

    if (c->decoder) {
        rc_decoder_destroy((rc_decoder *)c->decoder);
        c->decoder = NULL;
        atomic_store(&c->decoder_desc, NULL);
    }
    if (c->audio_player) {
        rc_audio_destroy((rc_audio *)c->audio_player);
        c->audio_player = NULL;
    }

    rc_server_teardown(c);
}

void rc_client_destroy(rc_client *c) {
    if (!c) return;
    rc_client_stop(c);
    free(c->serial_owned);
    free(c->tcp_addr_owned);
    free(c);
}

rc_status rc_client_get_device_size(const rc_client *c, int *width, int *height) {
    if (!c || !width || !height) return RC_ERR_INVALID_ARG;
    if (!atomic_load((atomic_int *)&c->have_meta)) return RC_ERR_PROTOCOL;
    *width = c->meta.width;
    *height = c->meta.height;
    return RC_OK;
}

/* ---- Input: dựng buffer control rồi giao control_msg.c gửi đi ---- */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put_f32(uint8_t *p, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    put_u32(p, v);
}

rc_status rc_client_send_mouse_motion(rc_client *c, uint32_t buttons, float x, float y) {
    if (!c) return RC_ERR_INVALID_ARG;
    uint8_t b[1 + 4 + 4 + 4];
    b[0] = RC_CTRL_MOUSE_MOTION;
    put_u32(b + 1, buttons);
    put_f32(b + 5, x);
    put_f32(b + 9, y);
    return rc_control_send(c, b, sizeof b);
}

rc_status rc_client_send_mouse_button(rc_client *c, int action, uint32_t button, uint32_t buttons,
                                      float x, float y) {
    if (!c) return RC_ERR_INVALID_ARG;
    uint8_t b[1 + 1 + 4 + 4 + 4 + 4];
    b[0] = RC_CTRL_MOUSE_BUTTON;
    b[1] = (uint8_t)(action ? RC_ACTION_DOWN : RC_ACTION_UP);
    put_u32(b + 2, button);
    put_u32(b + 6, buttons);
    put_f32(b + 10, x);
    put_f32(b + 14, y);
    return rc_control_send(c, b, sizeof b);
}

rc_status rc_client_send_scroll(rc_client *c, float x, float y, float hscroll, float vscroll) {
    if (!c) return RC_ERR_INVALID_ARG;
    uint8_t b[1 + 4 + 4 + 4 + 4];
    b[0] = RC_CTRL_SCROLL;
    put_f32(b + 1, x);
    put_f32(b + 5, y);
    put_f32(b + 9, hscroll);
    put_f32(b + 13, vscroll);
    return rc_control_send(c, b, sizeof b);
}

rc_status rc_client_send_key(rc_client *c, int action, uint32_t keycode, uint32_t metastate,
                             uint32_t repeat) {
    if (!c) return RC_ERR_INVALID_ARG;
    uint8_t b[1 + 1 + 4 + 4 + 4];
    b[0] = RC_CTRL_KEY;
    b[1] = (uint8_t)(action ? RC_ACTION_DOWN : RC_ACTION_UP);
    put_u32(b + 2, keycode);
    put_u32(b + 6, metastate);
    put_u32(b + 10, repeat);
    return rc_control_send(c, b, sizeof b);
}

rc_status rc_client_send_text(rc_client *c, const char *utf8) {
    if (!c || !utf8) return RC_ERR_INVALID_ARG;
    size_t n = strlen(utf8);
    if (n > (1u << 20)) return RC_ERR_INVALID_ARG;
    uint8_t *b = malloc(1 + 4 + n);
    if (!b) return RC_ERR_NOMEM;
    b[0] = RC_CTRL_TEXT;
    put_u32(b + 1, (uint32_t)n);
    memcpy(b + 5, utf8, n);
    rc_status r = rc_control_send(c, b, 5 + n);
    free(b);
    return r;
}

rc_status rc_client_click_button(rc_client *c, uint32_t keycode) {
    rc_status r = rc_client_send_key(c, RC_ACTION_DOWN, keycode, 0, 0);
    if (r != RC_OK) return r;
    return rc_client_send_key(c, RC_ACTION_UP, keycode, 0, 0);
}

rc_status rc_client_send_device_action(rc_client *c, rc_device_action action) {
    if (!c) return RC_ERR_INVALID_ARG;
    uint8_t b[2];
    b[0] = RC_CTRL_DEVICE_ACTION;
    b[1] = (uint8_t)action;
    return rc_control_send(c, b, sizeof b);
}
