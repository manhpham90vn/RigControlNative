/*
 * chooser.c — màn chọn thiết bị: liệt kê `adb devices`, dropdown kích thước/bitrate, và ô
 * "Quét agent" để nhập IP một máy chạy rc-agent (docs/AGENT_PROTOCOL.md): app hỏi cổng
 * discovery, `adb connect` từng thiết bị agent expose rồi hiện vào danh sách. Bấm một thiết bị
 * → mở một phiên (đa session).
 */
#include "rcgtk.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define DEFAULT_DISCOVERY_PORT 8888

static const int SIZE_VALUES[] = {0, 1920, 1280, 1024, 800};
static const int BITRATE_VALUES[] = {2000000,  4000000,  8000000, 16000000,
                                     24000000, 32000000, 48000000};

/* ---------- Liệt kê thiết bị adb ---------- */

typedef struct {
    char serial[128];
    char model[96];
} DevInfo;

/* Serial dạng "ip:port" = thiết bị wireless (adb connect qua mạng). */
static gboolean dev_is_wireless(const char *serial) {
    return strchr(serial, ':') != NULL;
}

/* Nhãn cách thiết bị đấu nối. Thiết bị qua rc-agent (tra registry) mang hậu tố "(agent)". Đường
 * stream thực tế (LAN trực tiếp hay fallback adb tunnel) chỉ biết sau khi kết nối — hiện trên
 * tiêu đề cửa sổ phiên. */
static const char *conn_label(App *app, const char *serial) {
    AgentDev *ad = g_hash_table_lookup(app->agent_devs, serial);
    if (ad) {
        if (strcmp(ad->kind, "emulator") == 0) return "Máy ảo (agent)";
        if (strcmp(ad->kind, "USB") == 0) return "USB (agent)";
        return "LAN (agent)";
    }
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

static void auto_probe_agents(App *app, GtkListBox *list, GPtrArray *devs);

static void populate_devices(App *app, GtkListBox *list) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))))
        gtk_list_box_remove(list, child);

    GPtrArray *devs = list_adb_devices();
    if (devs->len == 0) {
        GtkWidget *lbl =
            gtk_label_new("Không thấy thiết bị. Cắm máy / bật USB debugging, hoặc quét agent.");
        gtk_widget_set_margin_top(lbl, 16);
        gtk_widget_set_margin_bottom(lbl, 16);
        gtk_list_box_append(list, lbl);
    }
    for (guint i = 0; i < devs->len; i++) {
        DevInfo *d = devs->pdata[i];
        char text[256];
        g_snprintf(text, sizeof text, "<b>%s</b>  <small>· %s</small>\n<small>%s</small>",
                   d->model[0] ? d->model : "(không rõ model)", conn_label(app, d->serial),
                   d->serial);
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
    auto_probe_agents(app, list, devs);
    g_ptr_array_free(devs, TRUE);
}

/* ---------- Tự dò rc-agent cho thiết bị wireless lạ ----------
 *
 * Thiết bị wireless không có trong registry agent có thể là máy do rc-agent expose mà lần chạy
 * app này chưa quét (adb connect thẳng, hoặc còn dính trong `adb devices` từ trước). Khi đó
 * nhánh LAN trực tiếp giả định connect == listen sẽ trúng nhầm cổng relay của agent (lệch slot)
 * và chết ở bước device_meta. Dò cổng discovery trên host đó ở thread nền; đúng là agent thì
 * đăng ký mapping y như quét tay — phiên mở sau sẽ đi đúng dải stream_base + k. */

typedef struct {
    App *app;
    GtkListBox *list; /* ref — refresh nhãn "(agent)" khi tìm thấy */
    char *host;
    GPtrArray *devs; /* AgentDev* tìm được; NULL = không phải agent */
} AutoProbe;

static gboolean auto_probe_done(gpointer data) {
    AutoProbe *ctx = data;
    if (ctx->devs) {
        for (guint i = 0; i < ctx->devs->len; i++) {
            AgentDev *ad = ctx->devs->pdata[i];
            g_hash_table_replace(ctx->app->agent_devs, g_strdup(ad->serial),
                                 g_memdup2(ad, sizeof *ad));
        }
        g_message("tự phát hiện rc-agent tại %s — đăng ký %u thiết bị (map đúng dải stream)",
                  ctx->host, ctx->devs->len);
        populate_devices(ctx->app, ctx->list); /* cập nhật nhãn "(agent)" */
        g_ptr_array_free(ctx->devs, TRUE);
    }
    g_object_unref(ctx->list);
    g_free(ctx->host);
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

static gpointer auto_probe_thread(gpointer data) {
    AutoProbe *ctx = data;
    GError *err = NULL;
    if (!agent_scan(ctx->host, DEFAULT_DISCOVERY_PORT, &ctx->devs, &err)) {
        g_clear_error(&err); /* không trả lời / không phải agent — im lặng, đây chỉ là probe */
        ctx->devs = NULL;
    }
    g_idle_add(auto_probe_done, ctx);
    return NULL;
}

/* Mỗi host wireless lạ chỉ dò MỘT lần mỗi lần chạy app (probed_hosts) — kể cả khi không phải
 * agent, khỏi probe lại mỗi lần bấm "Làm mới". Chạy trên UI thread; việc nặng đẩy sang thread. */
static void auto_probe_agents(App *app, GtkListBox *list, GPtrArray *devs) {
    for (guint i = 0; i < devs->len; i++) {
        DevInfo *d = devs->pdata[i];
        if (!dev_is_wireless(d->serial)) continue;
        if (g_hash_table_contains(app->agent_devs, d->serial)) continue;
        char host[128];
        const char *colon = strrchr(d->serial, ':');
        size_t n = (size_t)(colon - d->serial);
        if (n == 0 || n >= sizeof host) continue;
        memcpy(host, d->serial, n);
        host[n] = '\0';
        if (g_hash_table_contains(app->probed_hosts, host)) continue;
        g_hash_table_add(app->probed_hosts, g_strdup(host));
        AutoProbe *ctx = g_new0(AutoProbe, 1);
        ctx->app = app;
        ctx->list = g_object_ref(list);
        ctx->host = g_strdup(host);
        g_message("%s: dò rc-agent tại %s:%d (nền)…", d->serial, host, DEFAULT_DISCOVERY_PORT);
        g_thread_unref(g_thread_new("agent-probe", auto_probe_thread, ctx));
    }
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

    /* Thiết bị qua rc-agent: stream_base > 0 → LAN trực tiếp (session cấp cổng theo thiết bị từ
     * dải của agent); = 0 → agent không relay stream → đi thẳng adb tunnel (khỏi phí vài giây
     * retry LAN rồi mới fallback). */
    AgentDev *ad = g_hash_table_lookup(app->agent_devs, serial);
    if (ad) {
        g_message("%s: thiết bị qua agent %s (adb cổng %d, stream_base %d)%s", serial, ad->host,
                  ad->adb_port, ad->stream_base,
                  ad->stream_base > 0 ? "" : " — agent không relay stream, đi adb tunnel");
        session_new(app, serial, ad->stream_base > 0 ? ad->host : NULL);
        return;
    }

    /* Máy wireless trực tiếp → luôn thử LAN trực tiếp; core tự fallback adb tunnel nếu cổng LAN
     * không tới được (thiết bị sau NAT/firewall). */
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

/* ---------- Quét agent (không mở phiên ngay) ---------- */

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
    char *ip;
    int port;
    GPtrArray *ok_devs; /* AgentDev* đã adb connect thành công (nạp registry trên UI thread) */
    char *msg;          /* kết quả từ thread nền để hiện lên status */
} AgentScan;

/* Nạp registry + cập nhật UI trên UI thread (GHashTable + widget không thread-safe). */
static gboolean agent_scan_done(gpointer data) {
    AgentScan *ctx = data;
    for (guint i = 0; i < ctx->ok_devs->len; i++) {
        AgentDev *ad = ctx->ok_devs->pdata[i];
        g_hash_table_replace(ctx->app->agent_devs, g_strdup(ad->serial), g_memdup2(ad, sizeof *ad));
    }
    gtk_label_set_text(ctx->status, ctx->msg);
    gtk_widget_set_sensitive(ctx->entry, TRUE);
    gtk_widget_set_sensitive(ctx->btn, TRUE);
    populate_devices(ctx->app, ctx->list);
    g_object_unref(ctx->entry);
    g_object_unref(ctx->btn);
    g_object_unref(ctx->status);
    g_object_unref(ctx->list);
    g_ptr_array_free(ctx->ok_devs, TRUE);
    g_free(ctx->ip);
    g_free(ctx->msg);
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

/* agent_scan() → với mỗi thiết bị: `adb connect` + xác minh `get-state` (adb connect exit 0 kể
 * cả khi thất bại). KHÔNG push rc-server ở đây — core tự push trong deploy_tcp khi mở phiên;
 * push N máy qua tailnet lúc quét chỉ làm chậm mà không thêm bảo đảm gì. */
static gpointer agent_scan_thread(gpointer data) {
    AgentScan *ctx = data;
    GPtrArray *devs = NULL;
    GError *err = NULL;
    if (!agent_scan(ctx->ip, ctx->port, &devs, &err)) {
        ctx->msg = g_strdup(err->message);
        g_error_free(err);
        g_idle_add(agent_scan_done, ctx);
        return NULL;
    }

    for (guint i = 0; i < devs->len; i++) {
        AgentDev *ad = devs->pdata[i];
        const char *connect_argv[] = {"adb", "connect", ad->serial, NULL};
        const char *state_argv[] = {"adb", "-s", ad->serial, "get-state", NULL};
        if (adb_run(connect_argv, 10000) && adb_run(state_argv, 5000))
            g_ptr_array_add(ctx->ok_devs, g_memdup2(ad, sizeof *ad));
    }
    guint total = devs->len, ok = ctx->ok_devs->len;
    g_ptr_array_free(devs, TRUE);

    ctx->msg =
        ok ? g_strdup_printf("Agent %s: nối được %u/%u thiết bị — bấm để mở.", ctx->ip, ok, total)
           : g_strdup_printf("Agent %s: %u thiết bị nhưng không nối được máy nào "
                             "(kiểm tra adb / mạng).",
                             ctx->ip, total);
    g_idle_add(agent_scan_done, ctx);
    return NULL;
}

static void agent_scan_start(App *app, GtkWidget *entry) {
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!text || !*text) return;
    char *ip = g_strdup(text);
    int port = DEFAULT_DISCOVERY_PORT;
    char *colon = strrchr(ip, ':'); /* "ip:port" tuỳ chọn — không có thì cổng discovery mặc định */
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0) port = DEFAULT_DISCOVERY_PORT;
    }

    g_hash_table_add(app->probed_hosts, g_strdup(ip)); /* quét tay = đã dò, auto-probe bỏ qua */

    GtkWidget *btn = g_object_get_data(G_OBJECT(entry), "btn");
    AgentScan *ctx = g_new0(AgentScan, 1);
    ctx->app = app;
    ctx->entry = g_object_ref(entry);
    ctx->btn = g_object_ref(btn);
    ctx->status = g_object_ref(g_object_get_data(G_OBJECT(entry), "status"));
    ctx->list = g_object_ref(g_object_get_data(G_OBJECT(entry), "list"));
    ctx->ip = ip;
    ctx->port = port;
    ctx->ok_devs = g_ptr_array_new_with_free_func(g_free);
    gtk_widget_set_sensitive(entry, FALSE);
    gtk_widget_set_sensitive(btn, FALSE);
    gtk_label_set_text(ctx->status, "Đang quét agent…");
    g_thread_unref(g_thread_new("agent-scan", agent_scan_thread, ctx));
}

static void on_agent_activate(GtkEntry *entry, gpointer user) {
    agent_scan_start((App *)user, GTK_WIDGET(entry));
}

static void on_agent_clicked(GtkButton *btn, gpointer user) {
    agent_scan_start((App *)user, g_object_get_data(G_OBJECT(btn), "entry"));
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

    /* Quét agent: nhập IP máy chạy rc-agent (LAN hoặc Tailscale). App hỏi cổng discovery, adb
     * connect từng thiết bị agent expose rồi hiện vào danh sách — bấm vào thiết bị mới mở phiên. */
    GtkWidget *agent_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "100.x.y.z (IP máy agent)");
    gtk_widget_set_tooltip_text(
        entry, "IP máy đang chạy rc-agent (LAN hoặc Tailscale). App hỏi cổng "
               "discovery 8888 rồi liệt kê mọi thiết bị máy đó có; thêm \":cổng\" "
               "nếu agent chạy cổng khác. Bấm vào thiết bị trong danh sách để mở.");
    gtk_widget_set_hexpand(entry, TRUE);
    g_signal_connect(entry, "activate", G_CALLBACK(on_agent_activate), app);
    gtk_box_append(GTK_BOX(agent_row), entry);
    GtkWidget *agent_btn = gtk_button_new_with_label("Quét agent");
    g_object_set_data(G_OBJECT(agent_btn), "entry", entry);
    g_signal_connect(agent_btn, "clicked", G_CALLBACK(on_agent_clicked), app);
    gtk_box_append(GTK_BOX(agent_row), agent_btn);
    gtk_box_append(GTK_BOX(box), agent_row);

    /* Trạng thái quét agent (đang quét / kết quả) — cập nhật từ agent_scan_done. */
    GtkWidget *agent_status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(agent_status), 0);
    gtk_label_set_wrap(GTK_LABEL(agent_status), TRUE);
    gtk_widget_add_css_class(agent_status, "dim-label");
    gtk_box_append(GTK_BOX(box), agent_status);

    g_object_set_data(G_OBJECT(entry), "btn", agent_btn);
    g_object_set_data(G_OBJECT(entry), "status", agent_status);
    g_object_set_data(G_OBJECT(entry), "list", list);

    populate_devices(app, GTK_LIST_BOX(list));
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));
}
