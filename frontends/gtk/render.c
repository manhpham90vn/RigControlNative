/*
 * render.c — hiển thị video: rc_frame (YUV I420) → 3 texture R8 → shader YUV→RGB trên
 * GtkGLArea, letterbox giữ tỉ lệ. Frame tới trên thread nội bộ của core nên được sao chép vào
 * buffer có mutex bảo vệ rồi marshal về UI thread bằng g_idle_add + gtk_gl_area_queue_render.
 */
#include "rcgtk.h"

#include <string.h>

/* ---------- Shader ---------- */

static const char *VERT_BODY =
    "in vec2 pos;\n"
    "in vec2 tex;\n"
    "out vec2 v_uv;\n"
    "void main() { v_uv = tex; gl_Position = vec4(pos, 0.0, 1.0); }\n";

/* YUV→RGB với ma trận màu cấp qua uniform (BT.601/709, limited/full-range theo frame). */
static const char *FRAG_BODY =
    "uniform sampler2D y_tex;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D v_tex;\n"
    "uniform mat3 cmat;\n"
    "uniform float y_off;\n"
    "in vec2 v_uv;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "  vec3 yuv = vec3(texture(y_tex, v_uv).r - y_off,\n"
    "                  texture(u_tex, v_uv).r - 0.5,\n"
    "                  texture(v_tex, v_uv).r - 0.5);\n"
    "  frag = vec4(cmat * yuv, 1.0);\n"
    "}\n";

/*
 * Dựng ma trận YUV→RGB (cột-major cho GL) từ hệ số kr/kb của chuẩn màu; limited-range
 * giãn Y (16-235) và chroma (16-240) về full trước khi nhân. Sai công thức range chính là
 * nguyên nhân ảnh "phủ màn sương" (đen thành xám).
 */
static void color_matrix(int bt709, int full_range, float m[9], float *y_off) {
    float kr = bt709 ? 0.2126f : 0.299f;
    float kb = bt709 ? 0.0722f : 0.114f;
    float kg = 1.f - kr - kb;
    float cr = 2.f * (1.f - kr);
    float cb = 2.f * (1.f - kb);
    float ys = full_range ? 1.f : 255.f / 219.f;
    float cs = full_range ? 1.f : 255.f / 224.f;
    *y_off = full_range ? 0.f : 16.f / 255.f;
    m[0] = ys;
    m[1] = ys;
    m[2] = ys;
    m[3] = 0.f;
    m[4] = -cs * cb * kb / kg;
    m[5] = cs * cb;
    m[6] = cs * cr;
    m[7] = -cs * cr * kr / kg;
    m[8] = 0.f;
}

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
    st->u_cmat = glGetUniformLocation(st->prog, "cmat");
    st->u_yoff = glGetUniformLocation(st->prog, "y_off");

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

/* Upload 1 plane vào texture (stride qua UNPACK_ROW_LENGTH), cấp lại storage nếu đổi cỡ. */
static void upload_plane(Session *st, int i, int unit, const guint8 *data, int stride, int w,
                         int h) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, st->tex[i]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
    if (st->tex_w[i] != w || st->tex_h[i] != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, data);
        st->tex_w[i] = w;
        st->tex_h[i] = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static gboolean on_render(GtkGLArea *area, GdkGLContext *ctx, gpointer user) {
    (void)ctx;
    Session *st = user;
    if (!st->prog) return FALSE;

    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    g_mutex_lock(&st->lock);
    int vw = st->vw, vh = st->vh;
    int full_range = st->full_range, bt709 = st->bt709;
    if (st->have_pending && vw > 0 && vh > 0) {
        upload_plane(st, 0, 0, st->plane[0], st->pstride[0], vw, vh);
        upload_plane(st, 1, 1, st->plane[1], st->pstride[1], vw / 2, vh / 2);
        upload_plane(st, 2, 2, st->plane[2], st->pstride[2], vw / 2, vh / 2);
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
    float cmat[9], y_off;
    color_matrix(bt709, full_range, cmat, &y_off);
    glUniformMatrix3fv(st->u_cmat, 1, GL_FALSE, cmat);
    glUniform1f(st->u_yoff, y_off);
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

/*
 * Chỉnh cửa sổ khớp kích thước video (= màn hình thiết bị khi max_size=0): 1:1 pixel nếu vừa,
 * quá to thì scale xuống ~90% monitor, luôn giữ tỉ lệ. Chạy trên UI thread khi có frame đầu
 * hoặc khi kích thước video đổi (xoay máy).
 */
static gboolean resize_to_video(gpointer user) {
    Session *st = user;
    if (!atomic_load(&st->alive) || !st->win) return G_SOURCE_REMOVE;
    int vw, vh;
    g_mutex_lock(&st->lock);
    vw = st->vw;
    vh = st->vh;
    g_mutex_unlock(&st->lock);
    if (vw <= 0 || vh <= 0) return G_SOURCE_REMOVE;

    int bar_h = st->bar ? gtk_widget_get_height(st->bar) : 0;
    if (bar_h <= 0) bar_h = 48;

    int max_w = vw, max_h = vh;
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(st->win));
    GdkMonitor *mon =
        surf ? gdk_display_get_monitor_at_surface(gtk_widget_get_display(st->win), surf) : NULL;
    if (mon) {
        GdkRectangle geo;
        gdk_monitor_get_geometry(mon, &geo);
        max_w = geo.width * 9 / 10;
        max_h = geo.height * 9 / 10 - bar_h;
    }
    double scale = 1.0;
    if (vw * scale > max_w) scale = (double)max_w / vw;
    if (vh * scale > max_h) scale = (double)max_h / vh;
    gtk_window_set_default_size(GTK_WINDOW(st->win), (int)(vw * scale + 0.5),
                                (int)(vh * scale + 0.5) + bar_h);
    return G_SOURCE_REMOVE;
}

/* Copy nguyên plane 1 lần memcpy, giữ stride của decoder (upload xử lý bằng ROW_LENGTH). */
static void copy_plane(Session *st, int i, const uint8_t *src, int stride, int w, int h) {
    size_t need = (size_t)stride * (h - 1) + w; /* dòng cuối chỉ chắc chắn có w byte */
    if (st->pcap[i] < need) {
        st->plane[i] = g_realloc(st->plane[i], need);
        st->pcap[i] = need;
    }
    memcpy(st->plane[i], src, need);
    st->pstride[i] = stride;
}

/* Chạy trên thread nội bộ của core. */
void render_on_frame(const rc_frame *f, void *user) {
    Session *st = user;
    if (f->format != RC_PIX_I420) return; /* MVP: chỉ I420 (sw H.264) */

    g_mutex_lock(&st->lock);
    int size_changed = (st->vw != f->width || st->vh != f->height);
    st->vw = f->width;
    st->vh = f->height;
    st->full_range = f->full_range;
    st->bt709 = f->bt709;
    copy_plane(st, 0, f->data[0], f->linesize[0], f->width, f->height);
    copy_plane(st, 1, f->data[1], f->linesize[1], f->width / 2, f->height / 2);
    copy_plane(st, 2, f->data[2], f->linesize[2], f->width / 2, f->height / 2);
    st->have_pending = 1;
    g_mutex_unlock(&st->lock);

    atomic_fetch_add(&st->frame_count, 1); /* đo FPS (session.c hiển thị) */

    static atomic_int first = 0;
    if (atomic_exchange(&first, 1) == 0) g_debug("[ui] frame đầu %dx%d", f->width, f->height);

    if (size_changed) g_idle_add(resize_to_video, st); /* frame đầu hoặc xoay máy */

    if (atomic_exchange(&st->render_scheduled, 1) == 0)
        g_idle_add(trigger_render, st); /* coalesce: một render/lần */
}

void render_free_planes(Session *st) {
    for (int i = 0; i < 3; i++) {
        g_free(st->plane[i]);
        st->plane[i] = NULL;
    }
}

gboolean render_widget_to_device(Session *st, double wx, double wy, float *dx, float *dy) {
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

GtkWidget *render_create_gl_area(Session *st) {
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
    return GTK_WIDGET(st->gl);
}
