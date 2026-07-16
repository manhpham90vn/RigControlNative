/*
 * RigControlNative — front-end GTK4 (MVP, Ubuntu). Phase 3.
 *
 * Render frame giải mã từ libcore: rc_frame (YUV I420) → 3 texture R8 → shader YUV→RGB trên
 * GtkGLArea, letterbox giữ tỉ lệ. Frame tới trên thread nội bộ của core nên được sao chép vào
 * buffer có mutex bảo vệ rồi marshal về UI thread bằng g_idle_add + gtk_gl_area_queue_render.
 *
 * Cấu hình qua biến môi trường (tránh đụng arg-parser của GtkApplication):
 *   RC_SERIAL     adb serial (mặc định: thiết bị mặc định)
 *   RC_TCP_ADDR   "ip:port" → dùng transport TCP thay vì USB
 *   RC_MAX_SIZE   giới hạn cạnh dài (mặc định 0 = full)
 *   RC_BIT_RATE   bps (mặc định 8_000_000)
 *   RC_MAX_FPS    (mặc định 60)
 *   RC_AUDIO      0/1 (mặc định 1) — Phase 3 chỉ drain, phát ở Phase 5
 *   RC_CONTROL    0/1 (mặc định 0) — inject ở Phase 4
 *   RC_SERVER_PATH  đường dẫn jar server (libcore đọc; mặc định "server/rc-server")
 */
#include <epoxy/gl.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/rc_client.h"

typedef struct App App;

typedef struct {
    App *app;
    rc_client *client;
    rc_config cfg;      /* cấu hình phiên */
    char *serial_owned; /* serial của phiên (sở hữu) */
    int torn;           /* đã dừng client chưa (tránh double-free) */
    GtkWidget *win;
    GtkGLArea *gl;

    /* GL objects (chỉ đụng trên UI thread) */
    GLuint prog, vao, vbo, tex[3];
    GLint u_tex[3];
    int tex_w[3], tex_h[3]; /* kích thước đã cấp cho từng texture */

    /* Frame chờ upload — bảo vệ bởi lock */
    GMutex lock;
    int have_pending;
    int vw, vh;         /* kích thước video hiện tại */
    guint8 *plane[3];   /* Y, U, V đóng gói khít (stride = width) */
    size_t pcap[3];     /* dung lượng đã cấp cho từng plane */

    atomic_int render_scheduled;
    atomic_int alive;

    int fb_w, fb_h; /* kích thước framebuffer (px) từ signal resize */

    /* Trạng thái input (UI thread) */
    uint32_t mouse_buttons; /* bitmask RC_BUTTON_* đang giữ */
    float dev_x, dev_y;     /* vị trí con trỏ gần nhất theo pixel thiết bị */
} Session;

/* ---------- Shader ---------- */

static const char *VERT_BODY =
    "in vec2 pos;\n"
    "in vec2 tex;\n"
    "out vec2 v_uv;\n"
    "void main() { v_uv = tex; gl_Position = vec4(pos, 0.0, 1.0); }\n";

/* BT.601 limited-range YUV→RGB. */
static const char *FRAG_BODY =
    "uniform sampler2D y_tex;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D v_tex;\n"
    "in vec2 v_uv;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "  float y = texture(y_tex, v_uv).r;\n"
    "  float u = texture(u_tex, v_uv).r - 0.5;\n"
    "  float v = texture(v_tex, v_uv).r - 0.5;\n"
    "  float r = y + 1.402 * v;\n"
    "  float g = y - 0.344136 * u - 0.714136 * v;\n"
    "  float b = y + 1.772 * u;\n"
    "  frag = vec4(r, g, b, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *header, const char *body) {
    const char *src[2] = {header, body};
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 2, src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        g_warning("compile shader lỗi: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint build_program(gboolean es) {
    const char *header = es ? "#version 300 es\nprecision highp float;\n" : "#version 150\n";
    GLuint vs = compile_shader(GL_VERTEX_SHADER, header, VERT_BODY);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, header, FRAG_BODY);
    if (!vs || !fs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glBindAttribLocation(prog, 1, "tex");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        g_warning("link program lỗi: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* ---------- GL lifecycle ---------- */

static void on_realize(GtkGLArea *area, gpointer user) {
    Session *st = user;
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != NULL) return;

    st->prog = build_program(gtk_gl_area_get_api(area) == GDK_GL_API_GLES);
    if (!st->prog) return;
    st->u_tex[0] = glGetUniformLocation(st->prog, "y_tex");
    st->u_tex[1] = glGetUniformLocation(st->prog, "u_tex");
    st->u_tex[2] = glGetUniformLocation(st->prog, "v_tex");

    static const float verts[] = {
        /* pos      tex (v lật để gốc ảnh nằm trên) */
        -1.f, -1.f, 0.f, 1.f, //
        1.f,  -1.f, 1.f, 1.f, //
        -1.f, 1.f,  0.f, 0.f, //
        1.f,  1.f,  1.f, 0.f, //
    };
    glGenVertexArrays(1, &st->vao);
    glBindVertexArray(st->vao);
    glGenBuffers(1, &st->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, st->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glGenTextures(3, st->tex);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, st->tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        st->tex_w[i] = st->tex_h[i] = 0;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

static void on_unrealize(GtkGLArea *area, gpointer user) {
    Session *st = user;
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != NULL) return;
    if (st->tex[0]) glDeleteTextures(3, st->tex);
    if (st->vbo) glDeleteBuffers(1, &st->vbo);
    if (st->vao) glDeleteVertexArrays(1, &st->vao);
    if (st->prog) glDeleteProgram(st->prog);
    st->prog = st->vao = st->vbo = 0;
    st->tex[0] = st->tex[1] = st->tex[2] = 0;
}

static void on_resize(GtkGLArea *area, int width, int height, gpointer user) {
    (void)area;
    Session *st = user;
    st->fb_w = width;
    st->fb_h = height;
}

/* Upload 1 plane (đóng gói khít) vào texture, cấp lại storage nếu đổi kích thước. */
static void upload_plane(Session *st, int i, int unit, const guint8 *data, int w, int h) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, st->tex[i]);
    if (st->tex_w[i] != w || st->tex_h[i] != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, data);
        st->tex_w[i] = w;
        st->tex_h[i] = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, data);
    }
}

static gboolean on_render(GtkGLArea *area, GdkGLContext *ctx, gpointer user) {
    (void)ctx;
    Session *st = user;
    if (!st->prog) return FALSE;

    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    g_mutex_lock(&st->lock);
    int vw = st->vw, vh = st->vh;
    if (st->have_pending && vw > 0 && vh > 0) {
        upload_plane(st, 0, 0, st->plane[0], vw, vh);
        upload_plane(st, 1, 1, st->plane[1], vw / 2, vh / 2);
        upload_plane(st, 2, 2, st->plane[2], vw / 2, vh / 2);
        st->have_pending = 0;
    }
    g_mutex_unlock(&st->lock);

    if (st->tex_w[0] == 0) return TRUE; /* chưa có frame nào */

    /* Letterbox: giữ tỉ lệ video trong framebuffer. */
    int fbw = st->fb_w > 0 ? st->fb_w : gtk_widget_get_width(GTK_WIDGET(area));
    int fbh = st->fb_h > 0 ? st->fb_h : gtk_widget_get_height(GTK_WIDGET(area));
    double win_a = (double)fbw / fbh;
    double vid_a = (double)vw / vh;
    int vpw, vph;
    if (win_a > vid_a) {
        vph = fbh;
        vpw = (int)(fbh * vid_a + 0.5);
    } else {
        vpw = fbw;
        vph = (int)(fbw / vid_a + 0.5);
    }
    glViewport((fbw - vpw) / 2, (fbh - vph) / 2, vpw, vph);

    glUseProgram(st->prog);
    glUniform1i(st->u_tex[0], 0);
    glUniform1i(st->u_tex[1], 1);
    glUniform1i(st->u_tex[2], 2);
    glBindVertexArray(st->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return TRUE;
}

/* ---------- Frame marshaling ---------- */

static gboolean trigger_render(gpointer user) {
    Session *st = user;
    atomic_store(&st->render_scheduled, 0);
    if (atomic_load(&st->alive) && st->gl) gtk_gl_area_queue_render(st->gl);
    return G_SOURCE_REMOVE;
}

static void copy_plane(Session *st, int i, const uint8_t *src, int stride, int w, int h) {
    size_t need = (size_t)w * h;
    if (st->pcap[i] < need) {
        st->plane[i] = g_realloc(st->plane[i], need);
        st->pcap[i] = need;
    }
    for (int y = 0; y < h; y++) memcpy(st->plane[i] + (size_t)y * w, src + (size_t)y * stride, w);
}

/* Chạy trên thread nội bộ của core. */
static void on_frame(const rc_frame *f, void *user) {
    Session *st = user;
    if (f->format != RC_PIX_I420) return; /* MVP: chỉ I420 (sw H.264) */

    g_mutex_lock(&st->lock);
    st->vw = f->width;
    st->vh = f->height;
    copy_plane(st, 0, f->data[0], f->linesize[0], f->width, f->height);
    copy_plane(st, 1, f->data[1], f->linesize[1], f->width / 2, f->height / 2);
    copy_plane(st, 2, f->data[2], f->linesize[2], f->width / 2, f->height / 2);
    st->have_pending = 1;
    g_mutex_unlock(&st->lock);

    static atomic_int first = 0;
    if (atomic_exchange(&first, 1) == 0) g_debug("[ui] frame đầu %dx%d", f->width, f->height);

    if (atomic_exchange(&st->render_scheduled, 1) == 0)
        g_idle_add(trigger_render, st); /* coalesce: một render/lần */
}

static void on_status(rc_status code, const char *msg, void *user) {
    (void)user;
    if (code == RC_OK)
        g_message("[core] %s", msg ? msg : "OK");
    else
        g_warning("[core] %s: %s", rc_status_str(code), msg ? msg : "");
}

/* ---------- Liệt kê thiết bị adb ---------- */

typedef struct {
    char serial[128];
    char model[96];
} DevInfo;

/* Chạy `adb devices -l`, trả mảng DevInfo* (chỉ máy trạng thái "device"). */
static GPtrArray *list_adb_devices(void) {
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    FILE *fp = popen("adb devices -l 2>/dev/null", "r");
    if (!fp) return arr;
    char line[512];
    gboolean first = TRUE;
    while (fgets(line, sizeof line, fp)) {
        if (first) {
            first = FALSE; /* dòng "List of devices attached" */
            continue;
        }
        char serial[128] = {0}, state[64] = {0};
        if (sscanf(line, "%127s %63s", serial, state) < 2) continue;
        if (strcmp(state, "device") != 0) continue;
        DevInfo *d = g_new0(DevInfo, 1);
        g_strlcpy(d->serial, serial, sizeof d->serial);
        char *m = strstr(line, "model:");
        if (m) sscanf(m + 6, "%95s", d->model);
        g_ptr_array_add(arr, d);
    }
    pclose(fp);
    return arr;
}

/* ---------- Kết nối tới một thiết bị ---------- */

static gpointer start_thread(gpointer user) {
    Session *st = user;
    rc_status r = rc_client_start(st->client);
    if (r != RC_OK) g_warning("[core] start thất bại: %s", rc_status_str(r));
    return NULL;
}

/* ---------- Input: chuột / bàn phím / nút thiết bị ---------- */

/* Ánh xạ toạ độ widget (logic) → pixel thiết bị, có tính letterbox. FALSE nếu ngoài vùng video. */
static gboolean widget_to_device(Session *st, double wx, double wy, float *dx, float *dy) {
    int vw, vh;
    g_mutex_lock(&st->lock);
    vw = st->vw;
    vh = st->vh;
    g_mutex_unlock(&st->lock);
    if (vw <= 0 || vh <= 0) return FALSE;
    int ww = gtk_widget_get_width(GTK_WIDGET(st->gl));
    int wh = gtk_widget_get_height(GTK_WIDGET(st->gl));
    if (ww <= 0 || wh <= 0) return FALSE;

    double win_a = (double)ww / wh, vid_a = (double)vw / vh;
    double vpw, vph;
    if (win_a > vid_a) {
        vph = wh;
        vpw = wh * vid_a;
    } else {
        vpw = ww;
        vph = ww / vid_a;
    }
    double vpx = (ww - vpw) / 2, vpy = (wh - vph) / 2;
    if (wx < vpx || wx > vpx + vpw || wy < vpy || wy > vpy + vph) return FALSE;
    *dx = (float)((wx - vpx) / vpw * vw);
    *dy = (float)((wy - vpy) / vph * vh);
    return TRUE;
}

static uint32_t gdk_button_to_rc(guint button) {
    switch (button) {
    case GDK_BUTTON_PRIMARY:
        return RC_BUTTON_LEFT;
    case GDK_BUTTON_SECONDARY:
        return RC_BUTTON_RIGHT;
    case GDK_BUTTON_MIDDLE:
        return RC_BUTTON_MIDDLE;
    default:
        return 0;
    }
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user) {
    (void)n;
    Session *st = user;
    if (!st->client) return;
    float dx, dy;
    if (!widget_to_device(st, x, y, &dx, &dy)) return;
    uint32_t b = gdk_button_to_rc(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)));
    st->mouse_buttons |= b;
    st->dev_x = dx;
    st->dev_y = dy;
    rc_client_send_mouse_button(st->client, RC_ACTION_DOWN, b, st->mouse_buttons, dx, dy);
    gtk_widget_grab_focus(GTK_WIDGET(st->gl));
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user) {
    (void)n;
    Session *st = user;
    if (!st->client) return;
    float dx, dy;
    if (!widget_to_device(st, x, y, &dx, &dy)) {
        dx = st->dev_x;
        dy = st->dev_y;
    }
    uint32_t b = gdk_button_to_rc(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)));
    st->mouse_buttons &= ~b;
    rc_client_send_mouse_button(st->client, RC_ACTION_UP, b, st->mouse_buttons, dx, dy);
}

static void on_motion(GtkEventControllerMotion *m, double x, double y, gpointer user) {
    (void)m;
    Session *st = user;
    if (!st->client || st->mouse_buttons == 0) return; /* chỉ gửi khi đang kéo */
    float dx, dy;
    if (!widget_to_device(st, x, y, &dx, &dy)) return;
    st->dev_x = dx;
    st->dev_y = dy;
    rc_client_send_mouse_motion(st->client, st->mouse_buttons, dx, dy);
}

static gboolean on_scroll(GtkEventControllerScroll *s, double dx, double dy, gpointer user) {
    (void)s;
    Session *st = user;
    if (!st->client) return FALSE;
    rc_client_send_scroll(st->client, st->dev_x, st->dev_y, (float)-dx, (float)-dy);
    return TRUE;
}

/* GDK keyval → Android keycode cho phím điều hướng/sửa; -1 nếu nên gửi dạng TEXT. */
static int gdk_to_android_key(guint keyval) {
    switch (keyval) {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        return 66; /* ENTER */
    case GDK_KEY_BackSpace:
        return 67; /* DEL */
    case GDK_KEY_Delete:
        return 112; /* FORWARD_DEL */
    case GDK_KEY_Tab:
        return 61; /* TAB */
    case GDK_KEY_Escape:
        return RC_AKEYCODE_BACK;
    case GDK_KEY_Left:
        return 21;
    case GDK_KEY_Right:
        return 22;
    case GDK_KEY_Up:
        return 19;
    case GDK_KEY_Down:
        return 20;
    case GDK_KEY_Home:
        return 122;
    case GDK_KEY_End:
        return 123;
    case GDK_KEY_Page_Up:
        return 92;
    case GDK_KEY_Page_Down:
        return 93;
    default:
        return -1;
    }
}

static gboolean on_key_pressed(GtkEventControllerKey *k, guint keyval, guint keycode,
                               GdkModifierType state, gpointer user) {
    (void)k;
    (void)keycode;
    (void)state;
    Session *st = user;
    if (!st->client) return FALSE;
    int ac = gdk_to_android_key(keyval);
    if (ac >= 0) {
        rc_client_send_key(st->client, RC_ACTION_DOWN, (uint32_t)ac, 0, 0);
        return TRUE;
    }
    guint32 uc = gdk_keyval_to_unicode(keyval);
    if (uc >= 0x20 && uc != 0x7f) {
        char buf[8];
        int n = g_unichar_to_utf8(uc, buf);
        buf[n] = '\0';
        rc_client_send_text(st->client, buf);
        return TRUE;
    }
    return FALSE;
}

static void on_key_released(GtkEventControllerKey *k, guint keyval, guint keycode,
                            GdkModifierType state, gpointer user) {
    (void)k;
    (void)keycode;
    (void)state;
    Session *st = user;
    if (!st->client) return;
    int ac = gdk_to_android_key(keyval);
    if (ac >= 0) rc_client_send_key(st->client, RC_ACTION_UP, (uint32_t)ac, 0, 0);
}

static void on_navbtn(GtkButton *btn, gpointer user) {
    Session *st = user;
    if (!st->client) return;
    int code = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "keycode"));
    rc_client_click_button(st->client, (uint32_t)code);
}

static void on_rotate(GtkButton *btn, gpointer user) {
    (void)btn;
    Session *st = user;
    if (st->client) rc_client_send_device_action(st->client, RC_DEVICE_ROTATE);
}

static void add_nav(GtkWidget *bar, Session *st, const char *label, int keycode) {
    GtkWidget *b = gtk_button_new_with_label(label);
    g_object_set_data(G_OBJECT(b), "keycode", GINT_TO_POINTER(keycode));
    g_signal_connect(b, "clicked", G_CALLBACK(on_navbtn), st);
    gtk_box_append(GTK_BOX(bar), b);
}

static void build_gl_view(Session *st) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Thanh nút điều khiển thiết bị. */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    add_nav(bar, st, "◀ Back", RC_AKEYCODE_BACK);
    add_nav(bar, st, "● Home", RC_AKEYCODE_HOME);
    add_nav(bar, st, "▢ Recent", RC_AKEYCODE_APP_SWITCH);
    add_nav(bar, st, "⏻ Power", RC_AKEYCODE_POWER);
    add_nav(bar, st, "🔉", RC_AKEYCODE_VOLUME_DOWN);
    add_nav(bar, st, "🔊", RC_AKEYCODE_VOLUME_UP);
    GtkWidget *rot = gtk_button_new_with_label("⟳ Xoay");
    g_signal_connect(rot, "clicked", G_CALLBACK(on_rotate), st);
    gtk_box_append(GTK_BOX(bar), rot);
    gtk_box_append(GTK_BOX(root), bar);

    /* Vùng render. */
    st->gl = GTK_GL_AREA(gtk_gl_area_new());
    gtk_gl_area_set_auto_render(st->gl, FALSE); /* chỉ render khi có frame mới */
    gtk_gl_area_set_has_depth_buffer(st->gl, FALSE);
    gtk_widget_set_hexpand(GTK_WIDGET(st->gl), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(st->gl), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(st->gl), TRUE);
    g_signal_connect(st->gl, "realize", G_CALLBACK(on_realize), st);
    g_signal_connect(st->gl, "unrealize", G_CALLBACK(on_unrealize), st);
    g_signal_connect(st->gl, "resize", G_CALLBACK(on_resize), st);
    g_signal_connect(st->gl, "render", G_CALLBACK(on_render), st);

    /* Controller chuột/cuộn gắn vào GL area. */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); /* mọi nút */
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), st);
    g_signal_connect(click, "released", G_CALLBACK(on_released), st);
    gtk_widget_add_controller(GTK_WIDGET(st->gl), GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), st);
    gtk_widget_add_controller(GTK_WIDGET(st->gl), motion);

    GtkEventController *scroll =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), st);
    gtk_widget_add_controller(GTK_WIDGET(st->gl), scroll);

    gtk_box_append(GTK_BOX(root), GTK_WIDGET(st->gl));

    /* Bàn phím gắn vào cửa sổ (hoạt động bất kể focus con nào). */
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), st);
    g_signal_connect(key, "key-released", G_CALLBACK(on_key_released), st);
    gtk_widget_add_controller(st->win, key);

    gtk_window_set_child(GTK_WINDOW(st->win), root);
}

/* ---------- Vòng đời phiên (đa session) ---------- */

struct App {
    GtkApplication *gtk;
    rc_config base;   /* mẫu cấu hình từ env */
    int sel_max_size; /* lựa chọn hiện tại (dropdown hoặc env) */
    int sel_bit_rate;
    GtkDropDown *dd_size; /* NULL nếu kết nối thẳng qua env */
    GtkDropDown *dd_bitrate;
    GList *sessions; /* Session* — giải phóng khi thoát app */
};

static const int SIZE_VALUES[] = {0, 1920, 1280, 1024, 800};
static const int BITRATE_VALUES[] = {2000000, 4000000, 8000000, 16000000};

static void session_teardown(Session *s) {
    if (s->torn) return;
    s->torn = 1;
    atomic_store(&s->alive, 0);
    s->gl = NULL; /* chặn trigger_render đụng widget đã hủy */
    rc_client_stop(s->client);
    rc_client_destroy(s->client);
    s->client = NULL;
    for (int i = 0; i < 3; i++) {
        g_free(s->plane[i]);
        s->plane[i] = NULL;
    }
}

static void on_session_destroy(GtkWidget *win, gpointer user) {
    (void)win;
    session_teardown((Session *)user);
}

static void read_settings(App *app) {
    if (app->dd_size) app->sel_max_size = SIZE_VALUES[gtk_drop_down_get_selected(app->dd_size)];
    if (app->dd_bitrate)
        app->sel_bit_rate = BITRATE_VALUES[gtk_drop_down_get_selected(app->dd_bitrate)];
}

/* Mở một phiên mới trong cửa sổ riêng. serial: NULL = mặc định / TCP. */
static void new_session(App *app, const char *serial) {
    Session *s = g_new0(Session, 1);
    s->app = app;
    g_mutex_init(&s->lock);
    atomic_init(&s->render_scheduled, 0);
    atomic_init(&s->alive, 1);

    s->cfg = app->base;
    s->cfg.max_size = app->sel_max_size;
    s->cfg.bit_rate = app->sel_bit_rate;
    s->serial_owned = serial ? g_strdup(serial) : NULL;
    s->cfg.serial = s->serial_owned;

    s->client = rc_client_create(&s->cfg);
    if (!s->client) {
        g_warning("Không tạo được rc_client");
        g_mutex_clear(&s->lock);
        g_free(s->serial_owned);
        g_free(s);
        return;
    }
    rc_client_set_frame_callback(s->client, on_frame, s);
    rc_client_set_status_callback(s->client, on_status, s);

    s->win = gtk_application_window_new(app->gtk);
    char title[192];
    const char *tag = serial ? serial
                             : (s->cfg.transport == RC_TRANSPORT_TCP ? s->cfg.tcp_addr : "default");
    g_snprintf(title, sizeof title, "RigControlNative — %s", tag ? tag : "default");
    gtk_window_set_title(GTK_WINDOW(s->win), title);
    gtk_window_set_default_size(GTK_WINDOW(s->win), 540, 960);
    g_signal_connect(s->win, "destroy", G_CALLBACK(on_session_destroy), s);
    build_gl_view(s);
    gtk_window_present(GTK_WINDOW(s->win));

    app->sessions = g_list_prepend(app->sessions, s);

    GThread *t = g_thread_new("rc-start", start_thread, s); /* deploy nền, không đơ UI */
    g_thread_unref(t);
}

/* ---------- Màn chọn thiết bị ---------- */

static void populate_devices(App *app, GtkListBox *list) {
    (void)app;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))))
        gtk_list_box_remove(list, child);

    GPtrArray *devs = list_adb_devices();
    if (devs->len == 0) {
        GtkWidget *lbl =
            gtk_label_new("Không thấy thiết bị. Cắm máy / bật USB debugging rồi Làm mới.");
        gtk_widget_set_margin_top(lbl, 16);
        gtk_widget_set_margin_bottom(lbl, 16);
        gtk_list_box_append(list, lbl);
    }
    for (guint i = 0; i < devs->len; i++) {
        DevInfo *d = devs->pdata[i];
        char text[256];
        g_snprintf(text, sizeof text, "<b>%s</b>\n<small>%s</small>",
                   d->model[0] ? d->model : "(không rõ model)", d->serial);
        GtkWidget *lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(lbl), text);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_widget_set_margin_top(lbl, 10);
        gtk_widget_set_margin_bottom(lbl, 10);
        gtk_widget_set_margin_start(lbl, 12);
        gtk_widget_set_margin_end(lbl, 12);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        g_object_set_data_full(G_OBJECT(row), "serial", g_strdup(d->serial), g_free);
        gtk_list_box_append(list, row);
    }
    g_ptr_array_free(devs, TRUE);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user) {
    (void)box;
    App *app = user;
    const char *serial = g_object_get_data(G_OBJECT(row), "serial");
    read_settings(app);
    new_session(app, serial); /* serial sống nhờ row-data tới khi create copy xong */
}

static void on_refresh(GtkButton *btn, gpointer user) {
    GtkListBox *list = g_object_get_data(G_OBJECT(btn), "list");
    populate_devices((App *)user, list);
}

static GtkWidget *labeled_dropdown(const char *label, const char *const *items, guint def,
                                   GtkDropDown **out) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(box), gtk_label_new(label));
    GtkWidget *dd = gtk_drop_down_new_from_strings(items);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), def);
    gtk_widget_set_hexpand(dd, TRUE);
    gtk_box_append(GTK_BOX(box), dd);
    *out = GTK_DROP_DOWN(dd);
    return box;
}

static void show_chooser(App *app) {
    GtkWidget *win = gtk_application_window_new(app->gtk);
    gtk_window_set_title(GTK_WINDOW(win), "RigControlNative — Chọn thiết bị");
    gtk_window_set_default_size(GTK_WINDOW(win), 420, 520);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);

    static const char *const size_items[] = {"Full (gốc)", "1920", "1280", "1024", "800", NULL};
    static const char *const bitrate_items[] = {"2 Mbps", "4 Mbps", "8 Mbps", "16 Mbps", NULL};
    gtk_box_append(GTK_BOX(box), labeled_dropdown("Kích thước:", size_items, 2, &app->dd_size));
    gtk_box_append(GTK_BOX(box), labeled_dropdown("Bitrate:", bitrate_items, 2, &app->dd_bitrate));

    GtkWidget *hint = gtk_label_new("Bấm một thiết bị để mở (mở được nhiều máy cùng lúc).");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_box_append(GTK_BOX(box), hint);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), app);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
    gtk_box_append(GTK_BOX(box), scroller);

    GtkWidget *refresh = gtk_button_new_with_label("Làm mới");
    g_object_set_data(G_OBJECT(refresh), "list", list);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), app);
    gtk_box_append(GTK_BOX(box), refresh);

    populate_devices(app, GTK_LIST_BOX(list));
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));
}

static void on_activate(GtkApplication *gtkapp, gpointer user) {
    App *app = user;
    app->gtk = gtkapp;

    /* TCP hoặc serial chỉ định qua env → mở thẳng một phiên, bỏ qua bộ chọn. */
    if (app->base.transport == RC_TRANSPORT_TCP || (app->base.serial && *app->base.serial)) {
        app->sel_max_size = app->base.max_size;
        app->sel_bit_rate = app->base.bit_rate;
        new_session(app, app->base.serial);
        return;
    }
    show_chooser(app);
}

static int env_int(const char *name, int def) {
    const char *v = g_getenv(name);
    return v && *v ? atoi(v) : def;
}

static void free_session(gpointer data) {
    Session *s = data;
    session_teardown(s);
    g_mutex_clear(&s->lock);
    g_free(s->serial_owned);
    g_free(s);
}

int main(int argc, char **argv) {
    const char *tcp = g_getenv("RC_TCP_ADDR");
    App app;
    memset(&app, 0, sizeof app);
    app.base = (rc_config){
        .serial = g_getenv("RC_SERIAL"),
        .transport = (tcp && *tcp) ? RC_TRANSPORT_TCP : RC_TRANSPORT_USB,
        .tcp_addr = tcp,
        .max_size = env_int("RC_MAX_SIZE", 0),
        .bit_rate = env_int("RC_BIT_RATE", 8000000),
        .max_fps = env_int("RC_MAX_FPS", 60),
        .codec = RC_CODEC_H264,
        .control = env_int("RC_CONTROL", 1),
        .audio = env_int("RC_AUDIO", 1),
        .audio_codec = RC_ACODEC_OPUS,
    };
    app.sel_max_size = app.base.max_size;
    app.sel_bit_rate = app.base.bit_rate;

    GtkApplication *gtkapp =
        gtk_application_new("com.rigcontrol.native", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtkapp, "activate", G_CALLBACK(on_activate), &app);
    int rc = g_application_run(G_APPLICATION(gtkapp), argc, argv);

    g_list_free_full(app.sessions, free_session);
    g_object_unref(gtkapp);
    return rc;
}
