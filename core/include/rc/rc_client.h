/*
 * RigControlNative — libcore public C API.
 *
 * Toàn bộ logic dùng chung (quản lý thiết bị, demux, decode, protocol) nằm trong libcore
 * và phơi ra qua header C thuần này. Mọi front-end (GTK, WinUI 3, SwiftUI) chỉ giao tiếp
 * với core qua các hàm dưới đây — không phụ thuộc chi tiết nội bộ.
 *
 * Xem docs/PROTOCOL.md cho các hằng số giao thức tương ứng.
 */
#ifndef RC_CLIENT_H
#define RC_CLIENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Mã trạng thái ---- */
typedef enum {
    RC_OK = 0,
    RC_ERR_GENERIC = -1,
    RC_ERR_INVALID_ARG = -2,
    RC_ERR_NO_DEVICE = -3,
    RC_ERR_ADB = -4,
    RC_ERR_CONNECT = -5,
    RC_ERR_PROTOCOL = -6,
    RC_ERR_DECODE = -7,
    RC_ERR_NOMEM = -8,
} rc_status;

/* ---- Cấu hình phiên ---- */
typedef enum {
    RC_TRANSPORT_USB = 0, /* qua adb reverse tunnel */
    RC_TRANSPORT_TCP = 1, /* trực tiếp TCP qua LAN   */
} rc_transport;

typedef enum {
    RC_CODEC_H264 = 0, /* MVP */
    RC_CODEC_H265 = 1, /* phase sau */
    RC_CODEC_AV1 = 2,  /* phase sau */
} rc_codec;

typedef enum {
    RC_ACODEC_OPUS = 0, /* mặc định */
    RC_ACODEC_AAC = 1,  /* phase sau */
    RC_ACODEC_RAW = 2,  /* PCM 16-bit, phase sau */
} rc_acodec;

typedef struct {
    const char *serial; /* adb serial; NULL = thiết bị mặc định */
    rc_transport transport;
    /* "ip[:port]" (port mặc định 27183); dùng khi transport == RC_TRANSPORT_TCP.
     * Nếu serial cũng được đặt → core tự push + chạy server (tcp=true) qua adb rồi mới
     * connect; nếu serial NULL → giả định server đã chạy sẵn trên thiết bị. */
    const char *tcp_addr;
    /* Cổng rc-server listen TRONG thiết bị; 0 = dùng port của tcp_addr (hành vi cũ).
     * Khác port của tcp_addr khi đi qua rc-agent: agent relay cổng public của tcp_addr về
     * đúng cổng này trong thiết bị (xem docs/AGENT_PROTOCOL.md §2.2). */
    int tcp_device_port;
    int max_size; /* giới hạn cạnh dài (px); 0 = full */
    int bit_rate; /* bit/s; 0 = mặc định core */
    int max_fps;  /* 0 = không giới hạn */
    rc_codec codec;
    int control; /* != 0 để bật kênh điều khiển chuột/bàn phím */
    int audio;   /* != 0 để stream + phát audio thiết bị */
    rc_acodec audio_codec;
} rc_config;

/* ---- Frame giải mã giao cho UI ----
 * MVP: frame CPU (planar/semi-planar). Phase sau bổ sung biến thể hwframe/dmabuf cho zero-copy.
 */
typedef enum {
    RC_PIX_I420 = 0, /* YUV 4:2:0 planar */
    RC_PIX_NV12 = 1, /* YUV 4:2:0 semi-planar */
} rc_pixfmt;

typedef struct {
    int width;
    int height;
    rc_pixfmt format;
    uint8_t *data[4]; /* con trỏ plane; sở hữu bởi core, chỉ hợp lệ trong callback */
    int linesize[4];
    int64_t pts_us; /* PTS micro-giây */
    /* Thông tin màu để UI convert YUV→RGB đúng (stream không tag → limited BT.601). */
    int full_range; /* 1 = full-range (JPEG); 0 = limited-range (Y 16-235) */
    int bt709;      /* 1 = BT.709; 0 = BT.601 */
} rc_frame;

/* ---- Callback ----
 * Chạy trên thread nội bộ của core; front-end tự marshal về UI thread (vd g_idle_add ở GTK).
 * Dữ liệu trong rc_frame chỉ hợp lệ trong phạm vi callback — sao chép nếu cần giữ lại.
 */
typedef void (*rc_frame_cb)(const rc_frame *frame, void *user);
typedef void (*rc_status_cb)(rc_status code, const char *msg, void *user);

/* ---- Vòng đời ---- */
typedef struct rc_client rc_client;

/* Khởi tạo với cấu hình copy nội bộ; trả NULL nếu lỗi cấp phát. */
rc_client *rc_client_create(const rc_config *cfg);

void rc_client_set_frame_callback(rc_client *c, rc_frame_cb cb, void *user);
void rc_client_set_status_callback(rc_client *c, rc_status_cb cb, void *user);

/* Deploy server, mở tunnel/kết nối, khởi động thread network + decode. Không chặn. */
rc_status rc_client_start(rc_client *c);

/* Yêu cầu hủy rc_client_start đang chạy trên thread khác (deploy/chờ kết nối thoát sớm).
 * Không chặn, an toàn gọi từ thread bất kỳ; sau đó vẫn phải stop/destroy như thường. */
void rc_client_abort(rc_client *c);

/* Dừng thread và đóng kết nối; an toàn khi gọi nhiều lần. */
void rc_client_stop(rc_client *c);
void rc_client_destroy(rc_client *c);

/* ---- Input (desktop → thiết bị). Toạ độ theo pixel VIDEO (device_meta / rc_frame);
 * server tự scale về pixel màn hình thiết bị khi inject (PROTOCOL.md quy ước chung). ---- */
#define RC_ACTION_UP 0
#define RC_ACTION_DOWN 1

#define RC_BUTTON_LEFT 0x01u
#define RC_BUTTON_RIGHT 0x02u
#define RC_BUTTON_MIDDLE 0x04u

rc_status rc_client_send_mouse_motion(rc_client *c, uint32_t buttons, float x, float y);
rc_status rc_client_send_mouse_button(rc_client *c, int action, uint32_t button, uint32_t buttons,
                                      float x, float y);
rc_status rc_client_send_scroll(rc_client *c, float x, float y, float hscroll, float vscroll);
rc_status rc_client_send_key(rc_client *c, int action, uint32_t keycode, uint32_t metastate,
                             uint32_t repeat);
rc_status rc_client_send_text(rc_client *c, const char *utf8);

/* ---- Nút điều hướng phần cứng: gửi qua KEY với Android keycode ---- */
#define RC_AKEYCODE_HOME 3
#define RC_AKEYCODE_BACK 4
#define RC_AKEYCODE_MENU 82
#define RC_AKEYCODE_APP_SWITCH 187
#define RC_AKEYCODE_POWER 26
#define RC_AKEYCODE_VOLUME_UP 24
#define RC_AKEYCODE_VOLUME_DOWN 25
#define RC_AKEYCODE_VOLUME_MUTE 164

/* Tiện ích: nhấn-thả một nút (DOWN rồi UP). */
rc_status rc_client_click_button(rc_client *c, uint32_t keycode);

/* ---- Hành động đặc biệt (nút trên UI desktop) ---- */
typedef enum {
    RC_DEVICE_SCREEN_OFF = 0,
    RC_DEVICE_SCREEN_ON = 1,
    RC_DEVICE_EXPAND_NOTIF = 2,
    RC_DEVICE_EXPAND_SETTINGS = 3,
    RC_DEVICE_COLLAPSE_PANELS = 4,
    RC_DEVICE_ROTATE = 5,
} rc_device_action;

rc_status rc_client_send_device_action(rc_client *c, rc_device_action action);

/* ---- Tiện ích ---- */
const char *rc_status_str(rc_status code);
/* Backend đang decode video, chuỗi dễ hiểu cho UI: "GPU NVIDIA (NVDEC)",
 * "GPU Intel/AMD (VAAPI)" hoặc "CPU (software)". Tự cập nhật nếu core fallback hw→sw giữa
 * chừng. NULL nếu phiên chưa start/chưa có decoder. An toàn gọi từ thread bất kỳ. */
const char *rc_client_get_decoder_desc(const rc_client *c);
/* Đường stream thực tế của phiên: "LAN trực tiếp", "LAN qua adb" (wireless adb — gồm cả khi
 * LAN trực tiếp không tới được và core đã fallback), "adb (máy ảo)" hoặc "USB". NULL tới khi
 * deploy xong. Trỏ string literal tĩnh — an toàn gọi từ thread bất kỳ. */
const char *rc_client_get_transport_desc(const rc_client *c);
/* Kích thước VIDEO (từ device_meta — bằng màn hình thiết bị khi max_size=0, đã làm tròn
 * xuống bội số 8); trả RC_ERR_* nếu chưa handshake. */
rc_status rc_client_get_device_size(const rc_client *c, int *width, int *height);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RC_CLIENT_H */
