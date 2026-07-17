/*
 * chooser.c — màn chọn thiết bị: liệt kê `adb devices`, dropdown kích thước/bitrate,
 * thêm thiết bị wireless adb qua ô nhập ip:port (chỉ adb connect + cài sẵn rc-server rồi
 * cập nhật danh sách). Bấm một thiết bị trong danh sách → mở một phiên (đa session).
 */
#include "rcgtk.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

static const int SIZE_VALUES[] = {0, 1920, 1280, 1024, 800};
static const int BITRATE_VALUES[] = {2000000,  4000000,  8000000, 16000000,
                                     24000000, 32000000, 48000000};

/* ---------- Liệt kê thiết bị adb ---------- */

typedef struct {
    char serial[128];
    char model[96];
} DevInfo;

/* Serial dạng "ip:port" = thiết bị wireless (adb connect qua mạng). */
static gboolean dev_is_wireless(const char *serial) { return strchr(serial, ':') != NULL; }

/* Nhãn cách thiết bị đấu nối với adb. Đường stream thực tế (LAN trực tiếp hay fallback adb
 * tunnel) chỉ biết sau khi kết nối — hiện trên tiêu đề cửa sổ phiên. */
static const char *conn_label(const char *serial) {
    if (dev_is_wireless(serial)) return "LAN";
    if (g_str_has_prefix(serial, "emulator-")) return "Máy ảo";
    return "USB";
}

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
        g_snprintf(text, sizeof text, "<b>%s</b>  <small>· %s</small>\n<small>%s</small>",
                   d->model[0] ? d->model : "(không rõ model)", conn_label(d->serial), d->serial);
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

/* ---------- Callback ---------- */

static void read_settings(App *app) {
    if (app->dd_size) app->sel_max_size = SIZE_VALUES[gtk_drop_down_get_selected(app->dd_size)];
    if (app->dd_bitrate)
        app->sel_bit_rate = BITRATE_VALUES[gtk_drop_down_get_selected(app->dd_bitrate)];
    if (app->cb_audio) app->sel_audio = gtk_check_button_get_active(app->cb_audio);
    if (app->cb_control) app->sel_control = gtk_check_button_get_active(app->cb_control);
    if (app->cb_fps) app->sel_show_fps = gtk_check_button_get_active(app->cb_fps);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user) {
    (void)box;
    App *app = user;
    const char *serial = g_object_get_data(G_OBJECT(row), "serial");
    read_settings(app);
    /* Máy wireless → luôn thử LAN trực tiếp (độ trễ thấp hơn); core tự fallback adb tunnel
     * nếu cổng LAN không tới được (thiết bị sau NAT/firewall). */
    if (dev_is_wireless(serial)) {
        char host[128];
        const char *colon = strrchr(serial, ':');
        size_t n = (size_t)(colon - serial);
        if (n > 0 && n < sizeof host) {
            memcpy(host, serial, n);
            host[n] = '\0';
            session_new(app, serial, host);
            return;
        }
    }
    session_new(app, serial, NULL); /* serial sống nhờ row-data tới khi create copy xong */
}

static void on_refresh(GtkButton *btn, gpointer user) {
    GtkListBox *list = g_object_get_data(G_OBJECT(btn), "list");
    populate_devices((App *)user, list);
}

/* ---------- Thêm thiết bị wireless adb (không mở phiên ngay) ---------- */

/* Chạy adb đồng bộ, nuốt output; TRUE nếu exit 0 trong hạn timeout_ms, quá hạn thì kill
 * (`adb connect` tới IP chết treo theo TCP SYN retry hàng phút). Chỉ gọi từ thread nền. */
static gboolean adb_run(const char *const argv[], int timeout_ms) {
    GPid pid;
    if (!g_spawn_async(NULL, (gchar **)argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                           G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &pid, NULL))
        return FALSE;
    gboolean ok = FALSE;
    for (int waited = 0;; waited += 50) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            break;
        }
        if (r < 0) break;
        if (waited >= timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            break;
        }
        g_usleep(50 * 1000);
    }
    g_spawn_close_pid(pid);
    return ok;
}

typedef struct {
    App *app;
    GtkWidget *entry, *btn; /* giữ ref tới khi xong (cửa sổ có thể đóng giữa chừng) */
    GtkLabel *status;
    GtkListBox *list;
    char *addr;
    char *msg; /* kết quả từ thread nền để hiện lên status */
} WifiAdd;

static gboolean wifi_add_done(gpointer data) {
    WifiAdd *ctx = data;
    gtk_label_set_text(ctx->status, ctx->msg);
    gtk_widget_set_sensitive(ctx->entry, TRUE);
    gtk_widget_set_sensitive(ctx->btn, TRUE);
    populate_devices(ctx->app, ctx->list);
    g_object_unref(ctx->entry);
    g_object_unref(ctx->btn);
    g_object_unref(ctx->status);
    g_object_unref(ctx->list);
    g_free(ctx->addr);
    g_free(ctx->msg);
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

/* adb connect + xác minh get-state (adb connect exit 0 kể cả khi thất bại) + push sẵn
 * rc-server. Không mở phiên — thiết bị hiện trong danh sách, bấm vào mới mở. */
static gpointer wifi_add_thread(gpointer data) {
    WifiAdd *ctx = data;
    const char *connect_argv[] = {"adb", "connect", ctx->addr, NULL};
    const char *state_argv[] = {"adb", "-s", ctx->addr, "get-state", NULL};
    if (!adb_run(connect_argv, 10000) || !adb_run(state_argv, 5000)) {
        ctx->msg = g_strdup_printf("Kết nối adb tới %s thất bại (kiểm tra ip:port / adb tcpip).",
                                   ctx->addr);
    } else {
        const char *server = g_getenv("RC_SERVER_PATH");
        if (!server || !*server) server = "server/rc-server"; /* cùng mặc định với libcore */
        const char *push_argv[] = {"adb",  "-s",   ctx->addr,
                                   "push", server, "/data/local/tmp/rc-server", NULL};
        ctx->msg = adb_run(push_argv, 30000)
                       ? g_strdup_printf("Đã thêm %s — bấm vào thiết bị trong danh sách để mở.",
                                         ctx->addr)
                       : g_strdup_printf("Đã kết nối %s nhưng cài rc-server thất bại "
                                         "(sẽ thử lại khi mở phiên).",
                                         ctx->addr);
    }
    g_idle_add(wifi_add_done, ctx);
    return NULL;
}

static void wifi_add(App *app, GtkWidget *entry) {
    const char *addr = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!addr || !*addr) return;
    GtkWidget *btn = g_object_get_data(G_OBJECT(entry), "btn");
    WifiAdd *ctx = g_new0(WifiAdd, 1);
    ctx->app = app;
    ctx->entry = g_object_ref(entry);
    ctx->btn = g_object_ref(btn);
    ctx->status = g_object_ref(g_object_get_data(G_OBJECT(entry), "status"));
    ctx->list = g_object_ref(g_object_get_data(G_OBJECT(entry), "list"));
    ctx->addr = g_strdup(addr);
    gtk_widget_set_sensitive(entry, FALSE);
    gtk_widget_set_sensitive(btn, FALSE);
    gtk_label_set_text(ctx->status, "Đang kết nối adb và cài rc-server…");
    g_thread_unref(g_thread_new("wifi-add", wifi_add_thread, ctx));
}

static void on_wifi_activate(GtkEntry *entry, gpointer user) {
    wifi_add((App *)user, GTK_WIDGET(entry));
}

static void on_wifi_clicked(GtkButton *btn, gpointer user) {
    wifi_add((App *)user, g_object_get_data(G_OBJECT(btn), "entry"));
}

/* ---------- Dựng UI ---------- */

/* Checkbox kèm mô tả (tooltip) rồi gắn vào parent; trả handle để lưu vào App. */
static GtkCheckButton *add_option(GtkWidget *parent, const char *label, const char *tip,
                                  gboolean active) {
    GtkWidget *cb = gtk_check_button_new_with_label(label);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), active);
    if (tip) gtk_widget_set_tooltip_text(cb, tip);
    gtk_box_append(GTK_BOX(parent), cb);
    return GTK_CHECK_BUTTON(cb);
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

void chooser_show(App *app) {
    GtkWidget *win = gtk_application_window_new(app->gtk);
    gtk_window_set_title(GTK_WINDOW(win), "RigControlNative — Chọn thiết bị");
    gtk_window_set_default_size(GTK_WINDOW(win), 500, 640);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);

    static const char *const size_items[] = {"Full (gốc)", "1920", "1280", "1024", "800", NULL};
    static const char *const bitrate_items[] = {"2 Mbps",  "4 Mbps",  "8 Mbps",  "16 Mbps",
                                                "24 Mbps", "32 Mbps", "48 Mbps", NULL};
    gtk_box_append(GTK_BOX(box), labeled_dropdown("Kích thước:", size_items, 0, &app->dd_size));
    gtk_box_append(GTK_BOX(box), labeled_dropdown("Bitrate:", bitrate_items, 2, &app->dd_bitrate));

    /* Nhóm tùy chọn phiên: tiêu đề + các checkbox thụt vào, mỗi ô có tooltip mô tả. */
    GtkWidget *opt_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(opt_title), "<b>Tùy chọn</b>");
    gtk_label_set_xalign(GTK_LABEL(opt_title), 0);
    gtk_box_append(GTK_BOX(box), opt_title);

    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(opts, 6);

    /* Khẳng định (tick sẵn) dễ hiểu hơn ô "chỉ xem" phủ định; bỏ tick = view-only. */
    app->cb_control = add_option(opts, "Điều khiển chuột & bàn phím",
                                 "Bỏ tick để chỉ xem — không gửi chuột/bàn phím tới thiết bị",
                                 app->base.control != 0);
    /* Tắt = không capture/encode/stream audio trên cả server lẫn client (nhẹ hơn). */
    app->cb_audio =
        add_option(opts, "Phát âm thanh thiết bị",
                   "Stream và phát audio của thiết bị (tốn thêm băng thông)", app->base.audio != 0);
    app->cb_fps = add_option(opts, "Hiện FPS trên tiêu đề cửa sổ",
                             "Ghép số khung hình/giây và backend decode vào tiêu đề cửa sổ phiên",
                             app->sel_show_fps != 0);
    gtk_box_append(GTK_BOX(box), opts);

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

    /* Thêm thiết bị wireless adb: nhập ip:port (máy đã bật `adb tcpip 5555`). Chỉ adb
     * connect + cài sẵn rc-server rồi cập nhật danh sách — bấm vào thiết bị mới mở phiên. */
    GtkWidget *wifi_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "192.168.1.x:5555");
    gtk_widget_set_tooltip_text(entry, "Máy đã bật `adb tcpip 5555`. Chỉ kết nối adb và cài "
                                       "sẵn rc-server; bấm vào thiết bị trong danh sách để mở "
                                       "phiên (thử LAN trực tiếp, tự chuyển qua adb tunnel "
                                       "nếu cổng LAN không tới được).");
    gtk_widget_set_hexpand(entry, TRUE);
    g_signal_connect(entry, "activate", G_CALLBACK(on_wifi_activate), app);
    gtk_box_append(GTK_BOX(wifi_row), entry);
    GtkWidget *wifi_btn = gtk_button_new_with_label("Thêm thiết bị Wi-Fi");
    g_object_set_data(G_OBJECT(wifi_btn), "entry", entry);
    g_signal_connect(wifi_btn, "clicked", G_CALLBACK(on_wifi_clicked), app);
    gtk_box_append(GTK_BOX(wifi_row), wifi_btn);
    gtk_box_append(GTK_BOX(box), wifi_row);

    /* Trạng thái thêm thiết bị (đang kết nối / kết quả) — cập nhật từ wifi_add_done. */
    GtkWidget *wifi_status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(wifi_status), 0);
    gtk_label_set_wrap(GTK_LABEL(wifi_status), TRUE);
    gtk_widget_add_css_class(wifi_status, "dim-label");
    gtk_box_append(GTK_BOX(box), wifi_status);

    g_object_set_data(G_OBJECT(entry), "btn", wifi_btn);
    g_object_set_data(G_OBJECT(entry), "status", wifi_status);
    g_object_set_data(G_OBJECT(entry), "list", list);

    populate_devices(app, GTK_LIST_BOX(list));
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));
}
