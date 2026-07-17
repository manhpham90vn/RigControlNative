/*
 * session.c — vòng đời một phiên mirror: tạo rc_client + cửa sổ (navbar + GL area + input),
 * deploy trên thread nền để không đơ UI, teardown khi đóng cửa sổ hoặc thoát app.
 */
#include "rcgtk.h"

#include <string.h>

/* Cổng stream LAN đầu tiên thử cấp; trùng Protocol.DEFAULT_TCP_PORT phía server. */
#define RC_LAN_BASE_PORT 27183

/* Cổng stream LAN trống thấp nhất chưa bị phiên đang chạy nào chiếm. Mỗi phiên LAN khiến server
 * bind một cổng riêng trên thiết bị; cấp cổng khác nhau để mở nhiều phiên cùng máy không dính
 * BindException. Bỏ qua phiên đã torn (server đã bị SIGTERM → cổng trên thiết bị đã giải phóng).
 * Chỉ gọi trên UI thread nên không cần khóa app->sessions. */
static int alloc_lan_port(App *app) {
    for (int port = RC_LAN_BASE_PORT;; port++) {
        int used = 0;
        for (GList *l = app->sessions; l; l = l->next) {
            const Session *s = l->data;
            if (!s->torn && s->lan_port == port) {
                used = 1;
                break;
            }
        }
        if (!used) return port;
    }
}

static gpointer start_thread(gpointer user) {
    Session *st = user;
    rc_status r = rc_client_start(st->client);
    if (r != RC_OK) g_warning("[core] start thất bại: %s", rc_status_str(r));
    return NULL;
}

static void on_status(rc_status code, const char *msg, void *user) {
    (void)user;
    if (code == RC_OK)
        g_message("[core] %s", msg ? msg : "OK");
    else
        g_warning("[core] %s: %s", rc_status_str(code), msg ? msg : "");
}

/* Timer 1s trên UI thread: hiện số frame nhận được trong giây vừa qua. */
static gboolean fps_tick(gpointer user) {
    Session *st = user;
    if (!atomic_load(&st->alive) || !st->fps_label) return G_SOURCE_REMOVE;
    char buf[32];
    g_snprintf(buf, sizeof buf, " %d FPS ", atomic_exchange(&st->frame_count, 0));
    gtk_label_set_text(GTK_LABEL(st->fps_label), buf);
    return G_SOURCE_CONTINUE;
}

static void build_view(Session *st) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* View-only: không thanh nút, không controller input (kênh control cũng tắt ở cfg). */
    if (st->cfg.control) gtk_box_append(GTK_BOX(root), input_create_navbar(st));

    GtkWidget *video = render_create_gl_area(st);
    if (st->show_fps) {
        GtkWidget *overlay = gtk_overlay_new();
        gtk_widget_set_hexpand(overlay, TRUE);
        gtk_widget_set_vexpand(overlay, TRUE);
        gtk_overlay_set_child(GTK_OVERLAY(overlay), video);
        st->fps_label = gtk_label_new(" — FPS ");
        gtk_widget_add_css_class(st->fps_label, "osd"); /* nền tối mờ kiểu OSD */
        gtk_widget_set_halign(st->fps_label, GTK_ALIGN_START);
        gtk_widget_set_valign(st->fps_label, GTK_ALIGN_START);
        gtk_widget_set_margin_top(st->fps_label, 8);
        gtk_widget_set_margin_start(st->fps_label, 8);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), st->fps_label);
        video = overlay;
        st->fps_timer = g_timeout_add_seconds(1, fps_tick, st);
    }
    gtk_box_append(GTK_BOX(root), video);

    if (st->cfg.control) input_attach(st);
    gtk_window_set_child(GTK_WINDOW(st->win), root);
}

static void session_teardown(Session *s) {
    if (s->torn) return;
    s->torn = 1;
    atomic_store(&s->alive, 0);
    s->gl = NULL; /* chặn trigger_render đụng widget đã hủy */
    if (s->fps_timer) {
        g_source_remove(s->fps_timer);
        s->fps_timer = 0;
    }
    s->fps_label = NULL;
    rc_client_stop(s->client);
    rc_client_destroy(s->client);
    s->client = NULL;
    render_free_planes(s);
}

static void on_session_destroy(GtkWidget *win, gpointer user) {
    (void)win;
    session_teardown((Session *)user);
}

void session_new(App *app, const char *serial, const char *tcp_addr) {
    Session *s = g_new0(Session, 1);
    s->app = app;
    g_mutex_init(&s->lock);
    atomic_init(&s->render_scheduled, 0);
    atomic_init(&s->alive, 1);
    atomic_init(&s->frame_count, 0);

    s->cfg = app->base;
    s->cfg.max_size = app->sel_max_size;
    s->cfg.bit_rate = app->sel_bit_rate;
    s->cfg.audio = app->sel_audio;
    s->cfg.control = app->sel_control;
    s->show_fps = app->sel_show_fps;
    s->serial_owned = serial ? g_strdup(serial) : NULL;
    s->cfg.serial = s->serial_owned;
    s->tcp_owned = tcp_addr ? g_strdup(tcp_addr) : NULL;
    if (s->tcp_owned) {
        s->cfg.transport = RC_TRANSPORT_TCP;
        /* Địa chỉ không kèm port (từ bộ chọn Wi-Fi) → tự cấp cổng stream trống theo phiên để
         * nhiều phiên LAN cùng thiết bị không trùng cổng. Có port sẵn (env RC_TCP_ADDR) → tôn
         * trọng lựa chọn của người dùng. */
        if (!strchr(s->tcp_owned, ':')) {
            s->lan_port = alloc_lan_port(app);
            char *withport = g_strdup_printf("%s:%d", s->tcp_owned, s->lan_port);
            g_free(s->tcp_owned);
            s->tcp_owned = withport;
        }
        s->cfg.tcp_addr = s->tcp_owned;
    }

    s->client = rc_client_create(&s->cfg);
    if (!s->client) {
        g_warning("Không tạo được rc_client");
        g_mutex_clear(&s->lock);
        g_free(s->serial_owned);
        g_free(s->tcp_owned);
        g_free(s);
        return;
    }
    rc_client_set_frame_callback(s->client, render_on_frame, s);
    rc_client_set_status_callback(s->client, on_status, s);

    s->win = gtk_application_window_new(app->gtk);
    char title[192];
    const char *tag = s->tcp_owned            ? s->tcp_owned
                      : serial                ? serial
                      : (s->cfg.transport == RC_TRANSPORT_TCP ? s->cfg.tcp_addr : "default");
    g_snprintf(title, sizeof title, "RigControlNative — %s%s", tag ? tag : "default",
               s->tcp_owned ? " (LAN)" : "");
    gtk_window_set_title(GTK_WINDOW(s->win), title);
    gtk_window_set_default_size(GTK_WINDOW(s->win), 540, 960);
    g_signal_connect(s->win, "destroy", G_CALLBACK(on_session_destroy), s);
    build_view(s);
    gtk_window_present(GTK_WINDOW(s->win));

    app->sessions = g_list_prepend(app->sessions, s);

    GThread *t = g_thread_new("rc-start", start_thread, s); /* deploy nền, không đơ UI */
    g_thread_unref(t);
}

void session_free(gpointer data) {
    Session *s = data;
    session_teardown(s);
    g_mutex_clear(&s->lock);
    g_free(s->serial_owned);
    g_free(s->tcp_owned);
    g_free(s);
}
