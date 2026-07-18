/*
 * session.c — vòng đời một phiên mirror: tạo rc_client + cửa sổ (navbar + GL area + input),
 * deploy trên thread nền để không đơ UI, teardown khi đóng cửa sổ hoặc thoát app.
 */
#include "rcgtk.h"

#include <string.h>

/* Cổng stream LAN đầu tiên thử cấp; trùng Protocol.DEFAULT_TCP_PORT phía server và STREAM_BASE
 * phía rc-agent. */
#define RC_LAN_BASE_PORT 27183
#define STREAM_COUNT 4 /* số phiên LAN trực tiếp đồng thời mỗi thiết bị (khớp rc-agent) */

/* Cổng stream LAN trống thấp nhất chưa bị phiên đang chạy nào chiếm. Mỗi phiên LAN khiến server
 * bind một cổng riêng trên thiết bị; cấp cổng khác nhau để mở nhiều phiên cùng máy không dính
 * BindException. Bỏ qua phiên đã torn (server đã bị SIGTERM → cổng trên thiết bị đã giải phóng).
 * Chỉ gọi trên UI thread nên không cần khóa app->sessions. Dùng cho wireless TRỰC TIẾP (connect
 * == listen); thiết bị qua agent dùng alloc_stream_k theo dải riêng. */
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

/* k stream trống thấp nhất (0..STREAM_COUNT-1) chưa bị phiên đang sống nào CỦA CÙNG serial
 * chiếm — thiết bị qua agent có dải stream RIÊNG nên cấp theo từng thiết bị, không toàn cục.
 * Bỏ phiên torn (cổng trong thiết bị đã giải phóng). -1 = hết → đi adb tunnel. UI thread. */
static int alloc_stream_k(App *app, const char *serial) {
    for (int k = 0; k < STREAM_COUNT; k++) {
        int used = 0;
        for (GList *l = app->sessions; l; l = l->next) {
            const Session *s = l->data;
            if (!s->torn && s->stream_k == k && s->serial_owned && serial &&
                strcmp(s->serial_owned, serial) == 0) {
                used = 1;
                break;
            }
        }
        if (!used) return k;
    }
    return -1;
}

static gpointer start_thread(gpointer user) {
    Session *st = user;
    rc_status r = rc_client_start(st->client);
    /* Phiên bị đóng giữa chừng (abort) thì lỗi là chủ đích — không cần cảnh báo. */
    if (r != RC_OK && atomic_load(&st->alive))
        g_warning("[core] start thất bại: %s", rc_status_str(r));
    return NULL;
}

static void on_status(rc_status code, const char *msg, void *user) {
    (void)user;
    if (code == RC_OK)
        g_message("[core] %s", msg ? msg : "OK");
    else
        g_warning("[core] %s: %s", rc_status_str(code), msg ? msg : "");
}

/* Timer 1s trên UI thread: ghép FPS + backend decode ("CPU (software)" / "GPU ... (VAAPI)")
 * vào sau tiêu đề cửa sổ. Đọc lại mỗi tick vì core có thể fallback hw→sw giữa chừng. */
static gboolean fps_tick(gpointer user) {
    Session *st = user;
    if (!atomic_load(&st->alive)) return G_SOURCE_REMOVE;
    const char *dec = st->client ? rc_client_get_decoder_desc(st->client) : NULL;
    int fps = atomic_exchange(&st->frame_count, 0);
    char buf[256];
    if (dec)
        g_snprintf(buf, sizeof buf, "%s · %d FPS · %s", st->title_base, fps, dec);
    else
        g_snprintf(buf, sizeof buf, "%s · %d FPS", st->title_base, fps);
    gtk_window_set_title(GTK_WINDOW(st->win), buf);
    return G_SOURCE_CONTINUE;
}

/* Poll 500ms tới khi core biết đường stream thực tế (chỉ có sau deploy — LAN trực tiếp hay
 * đã fallback adb tunnel) → gắn vào tiêu đề cửa sổ rồi tự dừng. */
static gboolean title_tick(gpointer user) {
    Session *st = user;
    if (!atomic_load(&st->alive)) {
        st->title_timer = 0;
        return G_SOURCE_REMOVE;
    }
    const char *t = st->client ? rc_client_get_transport_desc(st->client) : NULL;
    if (!t) return G_SOURCE_CONTINUE;
    const char *tag =
        st->serial_owned ? st->serial_owned : (st->tcp_owned ? st->tcp_owned : "default");
    g_snprintf(st->title_base, sizeof st->title_base, "RigControlNative — %s (%s)", tag, t);
    gtk_window_set_title(GTK_WINDOW(st->win), st->title_base); /* fps_tick sẽ ghép thêm FPS */
    st->title_timer = 0;
    return G_SOURCE_REMOVE;
}

static void build_view(Session *st) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* View-only: không thanh nút, không controller input (kênh control cũng tắt ở cfg). */
    if (st->cfg.control) gtk_box_append(GTK_BOX(root), input_create_navbar(st));

    GtkWidget *video = render_create_gl_area(st);
    if (st->show_fps) st->fps_timer = g_timeout_add_seconds(1, fps_tick, st);
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
    if (s->title_timer) {
        g_source_remove(s->title_timer);
        s->title_timer = 0;
    }
    /* Thread start có thể còn đang deploy/chờ kết nối → yêu cầu hủy rồi join TRƯỚC khi
     * destroy client, tránh use-after-free (thread dùng rc_client sau khi free). */
    if (s->client) rc_client_abort(s->client);
    if (s->start_thread) {
        g_thread_join(s->start_thread);
        s->start_thread = NULL;
    }
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
    s->stream_k = -1;
    s->tcp_owned = tcp_addr ? g_strdup(tcp_addr) : NULL;
    if (s->tcp_owned) {
        s->cfg.transport = RC_TRANSPORT_TCP;
        AgentDev *ad = serial ? g_hash_table_lookup(app->agent_devs, serial) : NULL;
        if (strchr(s->tcp_owned, ':')) {
            /* Địa chỉ kèm port sẵn (env RC_TCP_ADDR) → tôn trọng; connect == listen
             * (tcp_device_port = 0). */
            s->cfg.tcp_addr = s->tcp_owned;
            g_message("phiên %s: dùng địa chỉ TCP cho sẵn %s (env RC_TCP_ADDR)",
                      serial ? serial : "?", s->cfg.tcp_addr);
        } else if (ad && ad->stream_base > 0) {
            /* Thiết bị qua rc-agent: cổng public = stream_base + k (client connect tới máy
             * agent), server listen RC_LAN_BASE_PORT + k TRONG thiết bị; agent forward giữa hai
             * cổng (docs/AGENT_PROTOCOL.md §2.2). k cấp theo từng thiết bị. */
            int k = alloc_stream_k(app, serial);
            if (k >= 0) {
                s->stream_k = k;
                char *withport = g_strdup_printf("%s:%d", s->tcp_owned, ad->stream_base + k);
                g_free(s->tcp_owned);
                s->tcp_owned = withport;
                s->cfg.tcp_addr = s->tcp_owned;
                s->cfg.tcp_device_port = RC_LAN_BASE_PORT + k;
                g_message("phiên %s: thử LAN trực tiếp qua agent — connect %s, server listen %d "
                          "trong thiết bị (slot k=%d)",
                          serial ? serial : "?", s->cfg.tcp_addr, s->cfg.tcp_device_port, k);
            } else {
                /* Hết STREAM_COUNT phiên LAN đang sống của thiết bị này → đi adb tunnel. */
                g_message("phiên %s: hết %d slot stream LAN đang sống của thiết bị → adb tunnel",
                          serial ? serial : "?", STREAM_COUNT);
                g_free(s->tcp_owned);
                s->tcp_owned = NULL;
                s->cfg.transport = RC_TRANSPORT_USB;
                s->cfg.tcp_addr = NULL;
            }
        } else {
            /* Wireless trực tiếp (không qua agent) → cấp cổng toàn cục, connect == listen. */
            s->lan_port = alloc_lan_port(app);
            char *withport = g_strdup_printf("%s:%d", s->tcp_owned, s->lan_port);
            g_free(s->tcp_owned);
            s->tcp_owned = withport;
            s->cfg.tcp_addr = s->tcp_owned;
            g_message("phiên %s: thử LAN trực tiếp wireless — connect %s (connect == listen)",
                      serial ? serial : "?", s->cfg.tcp_addr);
        }
    } else {
        g_message("phiên %s: đi adb tunnel ngay từ đầu (USB/emulator, hoặc agent không relay "
                  "stream)",
                  serial ? serial : "?");
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
    const char *tag = serial ? serial : (s->tcp_owned ? s->tcp_owned : "default");
    g_snprintf(s->title_base, sizeof s->title_base, "RigControlNative — %s (đang kết nối…)", tag);
    gtk_window_set_title(GTK_WINDOW(s->win), s->title_base);
    s->title_timer = g_timeout_add(500, title_tick, s);
    gtk_window_set_default_size(GTK_WINDOW(s->win), 540, 960);
    g_signal_connect(s->win, "destroy", G_CALLBACK(on_session_destroy), s);
    build_view(s);
    gtk_window_present(GTK_WINDOW(s->win));

    app->sessions = g_list_prepend(app->sessions, s);

    /* Deploy nền, không đơ UI; giữ handle để teardown join trước khi destroy client. */
    s->start_thread = g_thread_new("rc-start", start_thread, s);
}

void session_free(gpointer data) {
    Session *s = data;
    session_teardown(s);
    g_mutex_clear(&s->lock);
    g_free(s->serial_owned);
    g_free(s->tcp_owned);
    g_free(s);
}
