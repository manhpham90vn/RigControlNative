/*
 * chooser.c — màn chọn thiết bị: liệt kê `adb devices`, dropdown kích thước/bitrate,
 * kết nối wireless adb qua ô nhập ip:port. Bấm một thiết bị → mở một phiên (đa session).
 */
#include "rcgtk.h"

#include <stdio.h>
#include <string.h>

static const int SIZE_VALUES[] = {0, 1920, 1280, 1024, 800};
static const int BITRATE_VALUES[] = {2000000,  4000000,  8000000, 16000000,
                                     24000000, 32000000, 48000000};

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

/* ---------- Callback ---------- */

static void read_settings(App *app) {
    if (app->dd_size) app->sel_max_size = SIZE_VALUES[gtk_drop_down_get_selected(app->dd_size)];
    if (app->dd_bitrate)
        app->sel_bit_rate = BITRATE_VALUES[gtk_drop_down_get_selected(app->dd_bitrate)];
    if (app->cb_audio) app->sel_audio = gtk_check_button_get_active(app->cb_audio);
    if (app->cb_viewonly) app->sel_control = !gtk_check_button_get_active(app->cb_viewonly);
    if (app->cb_fps) app->sel_show_fps = gtk_check_button_get_active(app->cb_fps);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user) {
    (void)box;
    App *app = user;
    const char *serial = g_object_get_data(G_OBJECT(row), "serial");
    read_settings(app);
    session_new(app, serial); /* serial sống nhờ row-data tới khi create copy xong */
}

static void on_refresh(GtkButton *btn, gpointer user) {
    GtkListBox *list = g_object_get_data(G_OBJECT(btn), "list");
    populate_devices((App *)user, list);
}

/* Mở phiên tới thiết bị wireless adb; core tự `adb connect` vì serial chứa ':'. */
static void wifi_session_open(App *app, GtkEntry *entry) {
    const char *addr = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!addr || !*addr) return;
    read_settings(app);
    session_new(app, addr);
}

static void on_wifi_activate(GtkEntry *entry, gpointer user) {
    wifi_session_open((App *)user, entry);
}

static void on_wifi_clicked(GtkButton *btn, gpointer user) {
    wifi_session_open((App *)user, g_object_get_data(G_OBJECT(btn), "entry"));
}

/* ---------- Dựng UI ---------- */

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
    gtk_window_set_default_size(GTK_WINDOW(win), 420, 520);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);

    static const char *const size_items[] = {"Full (gốc)", "1920", "1280", "1024", "800", NULL};
    static const char *const bitrate_items[] = {"2 Mbps",  "4 Mbps",  "8 Mbps", "16 Mbps",
                                                "24 Mbps", "32 Mbps", "48 Mbps", NULL};
    gtk_box_append(GTK_BOX(box), labeled_dropdown("Kích thước:", size_items, 0, &app->dd_size));
    gtk_box_append(GTK_BOX(box), labeled_dropdown("Bitrate:", bitrate_items, 2, &app->dd_bitrate));

    /* Tắt = không capture/encode/stream audio trên cả server lẫn client (nhẹ hơn). */
    GtkWidget *cb_audio = gtk_check_button_new_with_label("Stream audio thiết bị");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(cb_audio), app->base.audio != 0);
    app->cb_audio = GTK_CHECK_BUTTON(cb_audio);
    gtk_box_append(GTK_BOX(box), cb_audio);

    /* View-only: không mở kênh điều khiển, ẩn thanh nút + bỏ qua chuột/bàn phím. */
    GtkWidget *cb_viewonly = gtk_check_button_new_with_label("Chỉ xem (không điều khiển)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(cb_viewonly), app->base.control == 0);
    app->cb_viewonly = GTK_CHECK_BUTTON(cb_viewonly);
    gtk_box_append(GTK_BOX(box), cb_viewonly);

    GtkWidget *cb_fps = gtk_check_button_new_with_label("Hiện FPS");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(cb_fps), app->sel_show_fps != 0);
    app->cb_fps = GTK_CHECK_BUTTON(cb_fps);
    gtk_box_append(GTK_BOX(box), cb_fps);

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

    /* Kết nối wireless adb: nhập ip:port (máy đã bật `adb tcpip 5555`). */
    GtkWidget *wifi_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "192.168.1.x:5555");
    gtk_widget_set_hexpand(entry, TRUE);
    g_signal_connect(entry, "activate", G_CALLBACK(on_wifi_activate), app);
    gtk_box_append(GTK_BOX(wifi_row), entry);
    GtkWidget *wifi_btn = gtk_button_new_with_label("Kết nối Wi-Fi");
    g_object_set_data(G_OBJECT(wifi_btn), "entry", entry);
    g_signal_connect(wifi_btn, "clicked", G_CALLBACK(on_wifi_clicked), app);
    gtk_box_append(GTK_BOX(wifi_row), wifi_btn);
    gtk_box_append(GTK_BOX(box), wifi_row);

    populate_devices(app, GTK_LIST_BOX(list));
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));
}
