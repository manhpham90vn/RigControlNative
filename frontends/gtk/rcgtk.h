/*
 * rcgtk.h — cấu trúc App/Session + API giữa các module của front-end GTK4:
 *   main.c    entry point, đọc cấu hình env, activate
 *   chooser.c màn chọn thiết bị (adb devices, wireless adb, dropdown cấu hình)
 *   session.c vòng đời một phiên mirror (cửa sổ, rc_client, teardown)
 *   render.c  GtkGLArea + shader YUV→RGB, marshal frame từ thread core về UI
 *   input.c   chuột / bàn phím / nút điều hướng → control message
 */
#ifndef RCGTK_H
#define RCGTK_H

#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <stdatomic.h>

#include "rc/rc_client.h"

typedef struct App App;

/* Một phiên mirror = một cửa sổ + một rc_client (đa session, kể cả cùng thiết bị). */
typedef struct {
    App *app;
    rc_client *client;
    rc_config cfg;         /* cấu hình phiên */
    char *serial_owned;    /* serial của phiên (sở hữu) */
    char *tcp_owned;       /* "ip[:port]" LAN trực tiếp (sở hữu); NULL = qua adb */
    int lan_port;          /* cổng stream LAN tự cấp cho phiên; 0 = không phải LAN/không tự cấp */
    int torn;              /* đã dừng client chưa (tránh double-free) */
    GThread *start_thread; /* thread chạy rc_client_start; join trước khi destroy client */
    GtkWidget *win;
    GtkWidget *bar; /* thanh nút điều khiển (đo chiều cao khi resize cửa sổ) */
    GtkGLArea *gl;

    /* GL objects (chỉ đụng trên UI thread) */
    GLuint prog, vao, vbo, tex[3];
    GLint u_tex[3], u_cmat, u_yoff, u_nv12;
    int tex_w[3], tex_h[3]; /* kích thước đã cấp cho từng texture */
    GLenum tex_ifmt[3];     /* internal format đã cấp (R8 / RG8) */

    /* Frame chờ upload — bảo vệ bởi lock */
    GMutex lock;
    int have_pending;
    int vw, vh;            /* kích thước video hiện tại */
    int pixfmt;            /* rc_pixfmt của frame chờ upload (I420 sw / NV12 hwdec) */
    int full_range, bt709; /* thông tin màu của frame gần nhất */
    guint8 *plane[3];      /* Y, U, V giữ nguyên stride của decoder (copy 1 memcpy) */
    int pstride[3];        /* stride từng plane; GL upload qua UNPACK_ROW_LENGTH */
    size_t pcap[3];        /* dung lượng đã cấp cho từng plane */

    atomic_int render_scheduled;
    atomic_int alive;
    int logged_first; /* đã log frame đầu của phiên chưa (debug) */

    int fb_w, fb_h; /* kích thước framebuffer (px) từ signal resize */

    /* Đo FPS (bật qua checkbox/env): đếm frame tới, timer 1s ghép vào tiêu đề cửa sổ */
    int show_fps;
    atomic_int frame_count;
    guint fps_timer;
    guint title_timer;    /* poll đường stream thực tế từ core → cập nhật tiêu đề rồi tự dừng */
    char title_base[160]; /* "RigControlNative — <tag> (<đường stream>)"; fps_tick ghép thêm */

    /* Trạng thái input (UI thread) */
    uint32_t mouse_buttons; /* bitmask RC_BUTTON_* đang giữ */
    float dev_x, dev_y;     /* vị trí con trỏ gần nhất theo pixel thiết bị */
    uint8_t keys_down[32];  /* bitmap Android keycode (<256) đã gửi KEY DOWN, chờ UP */
} Session;

struct App {
    GtkApplication *gtk;
    rc_config base;   /* mẫu cấu hình từ env */
    int sel_max_size; /* lựa chọn hiện tại (dropdown/checkbox hoặc env) */
    int sel_bit_rate;
    int sel_audio;
    int sel_control;
    int sel_show_fps;
    GtkDropDown *dd_size; /* NULL nếu kết nối thẳng qua env */
    GtkDropDown *dd_bitrate;
    GtkCheckButton *cb_audio;
    GtkCheckButton *cb_control; /* tick = bật điều khiển; bỏ tick = chỉ xem */
    GtkCheckButton *cb_fps;
    GList *sessions; /* Session* — giải phóng khi thoát app */
};

/* render.c */
/* Tạo st->gl + gắn signal GL (realize/render/...); trả widget để add vào layout. */
GtkWidget *render_create_gl_area(Session *st);
/* rc_frame_cb — chạy trên thread nội bộ của core, marshal về UI thread. */
void render_on_frame(const rc_frame *frame, void *user);
void render_free_planes(Session *st);
/* Ánh xạ toạ độ widget (logic) → pixel thiết bị, có tính letterbox. FALSE nếu ngoài video. */
gboolean render_widget_to_device(Session *st, double wx, double wy, float *dx, float *dy);

/* input.c */
/* Tạo thanh nút điều hướng (Back/Home/.../Xoay); cũng gán st->bar. */
GtkWidget *input_create_navbar(Session *st);
/* Gắn controller chuột/cuộn vào st->gl và bàn phím vào st->win. */
void input_attach(Session *st);

/* session.c */
/* Mở một phiên mới trong cửa sổ riêng. serial: NULL = thiết bị adb mặc định.
 * tcp_addr != NULL → thử LAN trực tiếp tới "ip[:port]"; kèm serial thì core tự deploy server
 * qua adb trước khi connect, và tự fallback adb tunnel nếu cổng LAN không tới được. */
void session_new(App *app, const char *serial, const char *tcp_addr);
/* GDestroyNotify cho App.sessions. */
void session_free(gpointer data);

/* chooser.c */
void chooser_show(App *app);

#endif /* RCGTK_H */
