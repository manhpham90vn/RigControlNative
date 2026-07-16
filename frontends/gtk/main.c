/*
 * RigControlNative — front-end GTK4 (MVP, Ubuntu): entry point + cấu hình.
 * Cấu trúc module: xem rcgtk.h.
 *
 * Cấu hình qua biến môi trường (tránh đụng arg-parser của GtkApplication):
 *   RC_SERIAL     adb serial; "ip:port" → wireless adb, core tự `adb connect`
 *   RC_TCP_ADDR   "ip:port" → transport TCP trực tiếp (server đã chạy sẵn, không qua adb)
 *   RC_MAX_SIZE   giới hạn cạnh dài (mặc định 0 = full)
 *   RC_BIT_RATE   bps (mặc định 8_000_000)
 *   RC_MAX_FPS    (mặc định 60)
 *   RC_AUDIO      0/1 (mặc định 0)
 *   RC_CONTROL    0/1 (mặc định 1; 0 = view-only)
 *   RC_SHOW_FPS   0/1 (mặc định 1) — overlay FPS trên video
 *   RC_SERVER_PATH  đường dẫn jar server (libcore đọc; mặc định "server/rc-server")
 */
#include "rcgtk.h"

#include <stdlib.h>
#include <string.h>

static void on_activate(GtkApplication *gtkapp, gpointer user) {
    App *app = user;
    app->gtk = gtkapp;

    /* TCP hoặc serial chỉ định qua env → mở thẳng một phiên, bỏ qua bộ chọn. */
    if (app->base.transport == RC_TRANSPORT_TCP || (app->base.serial && *app->base.serial)) {
        app->sel_max_size = app->base.max_size;
        app->sel_bit_rate = app->base.bit_rate;
        app->sel_audio = app->base.audio;
        app->sel_control = app->base.control;
        session_new(app, app->base.serial);
        return;
    }
    chooser_show(app);
}

static int env_int(const char *name, int def) {
    const char *v = g_getenv(name);
    return v && *v ? atoi(v) : def;
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
        .audio = env_int("RC_AUDIO", 0),
        .audio_codec = RC_ACODEC_OPUS,
    };
    app.sel_max_size = app.base.max_size;
    app.sel_bit_rate = app.base.bit_rate;
    app.sel_audio = app.base.audio;
    app.sel_control = app.base.control;
    app.sel_show_fps = env_int("RC_SHOW_FPS", 1);

    GtkApplication *gtkapp =
        gtk_application_new("com.rigcontrol.native", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtkapp, "activate", G_CALLBACK(on_activate), &app);
    int rc = g_application_run(G_APPLICATION(gtkapp), argc, argv);

    g_list_free_full(app.sessions, session_free);
    g_object_unref(gtkapp);
    return rc;
}
