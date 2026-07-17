/*
 * input.c — chuyển sự kiện GTK thành control message của core: chuột → cảm ứng
 * (nhấn/kéo/thả/cuộn), bàn phím (phím thường → TEXT, điều hướng/sửa → KEY), và thanh nút
 * thiết bị (Back/Home/Recent/Power/Volume/Xoay).
 */
#include "rcgtk.h"

#include <gdk/gdkkeysyms.h>

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
    if (!render_widget_to_device(st, x, y, &dx, &dy)) return;
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
    if (!render_widget_to_device(st, x, y, &dx, &dy)) {
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
    if (!st->client) return;
    float dx, dy;
    if (!render_widget_to_device(st, x, y, &dx, &dy)) return;
    /* Luôn nhớ vị trí (scroll cần tọa độ hiện tại); chỉ GỬI motion khi đang kéo. */
    st->dev_x = dx;
    st->dev_y = dy;
    if (st->mouse_buttons != 0) rc_client_send_mouse_motion(st->client, st->mouse_buttons, dx, dy);
}

static gboolean on_scroll(GtkEventControllerScroll *s, double dx, double dy, gpointer user) {
    (void)s;
    Session *st = user;
    if (!st->client) return FALSE;
    rc_client_send_scroll(st->client, st->dev_x, st->dev_y, (float)-dx, (float)-dy);
    return TRUE;
}

/* ---- Bàn phím ---- */

/* GDK modifier → Android metastate (kèm biến thể *_LEFT vì nhiều app kiểm tra bit đó). */
static uint32_t gdk_state_to_ameta(GdkModifierType state) {
    uint32_t meta = 0;
    if (state & GDK_SHIFT_MASK) meta |= 0x1 | 0x40;        /* SHIFT_ON | SHIFT_LEFT_ON */
    if (state & GDK_ALT_MASK) meta |= 0x02 | 0x10;         /* ALT_ON | ALT_LEFT_ON */
    if (state & GDK_CONTROL_MASK) meta |= 0x1000 | 0x2000; /* CTRL_ON | CTRL_LEFT_ON */
    return meta;
}

/* Chữ/số → Android keycode (A=29..Z=54, 0=7..9=16, SPACE=62); -1 nếu không phải.
 * Dùng cho tổ hợp Ctrl/Alt (Ctrl+A, Ctrl+C...) — phím thường vẫn đi đường TEXT. */
static int gdk_alnum_to_android_key(guint keyval) {
    guint kv = gdk_keyval_to_lower(keyval);
    if (kv >= GDK_KEY_a && kv <= GDK_KEY_z) return 29 + (int)(kv - GDK_KEY_a);
    if (kv >= GDK_KEY_0 && kv <= GDK_KEY_9) return 7 + (int)(kv - GDK_KEY_0);
    if (kv == GDK_KEY_space) return 62;
    return -1;
}

/* Bitmap phím đã gửi KEY DOWN (chờ UP) — để release luôn có UP tương ứng kể cả khi
 * modifier đã nhả trước (tránh "kẹt phím" trên thiết bị). */
static void key_mark(Session *st, int code, int down) {
    if (code < 0 || code >= 256) return;
    if (down)
        st->keys_down[code / 8] |= (uint8_t)(1u << (code % 8));
    else
        st->keys_down[code / 8] &= (uint8_t)~(1u << (code % 8));
}
static int key_marked(const Session *st, int code) {
    if (code < 0 || code >= 256) return 0;
    return (st->keys_down[code / 8] >> (code % 8)) & 1;
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
    Session *st = user;
    if (!st->client) return FALSE;
    uint32_t meta = gdk_state_to_ameta(state);

    int ac = gdk_to_android_key(keyval);
    /* Tổ hợp Ctrl/Alt: gửi KEY (kèm metastate) thay vì TEXT để app nhận đúng shortcut. */
    if (ac < 0 && (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)))
        ac = gdk_alnum_to_android_key(keyval);
    if (ac >= 0) {
        rc_client_send_key(st->client, RC_ACTION_DOWN, (uint32_t)ac, meta, 0);
        key_mark(st, ac, 1);
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
    Session *st = user;
    if (!st->client) return;
    int ac = gdk_to_android_key(keyval);
    if (ac < 0) ac = gdk_alnum_to_android_key(keyval);
    /* Chỉ UP những phím đã DOWN qua KEY (phím TEXT không có down/up). */
    if (ac >= 0 && key_marked(st, ac)) {
        rc_client_send_key(st->client, RC_ACTION_UP, (uint32_t)ac, gdk_state_to_ameta(state), 0);
        key_mark(st, ac, 0);
    }
}

static void on_navbtn(GtkButton *btn, gpointer user) {
    Session *st = user;
    if (!st->client) return;
    int code = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "keycode"));
    rc_client_click_button(st->client, (uint32_t)code);
}

static void on_devbtn(GtkButton *btn, gpointer user) {
    Session *st = user;
    if (!st->client) return;
    int action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "devaction"));
    rc_client_send_device_action(st->client, (rc_device_action)action);
}

static void add_nav(GtkWidget *bar, Session *st, const char *label, const char *tip, int keycode) {
    GtkWidget *b = gtk_button_new_with_label(label);
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    g_object_set_data(G_OBJECT(b), "keycode", GINT_TO_POINTER(keycode));
    g_signal_connect(b, "clicked", G_CALLBACK(on_navbtn), st);
    gtk_box_append(GTK_BOX(bar), b);
}

static void add_dev(GtkWidget *bar, Session *st, const char *label, const char *tip, int action) {
    GtkWidget *b = gtk_button_new_with_label(label);
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    g_object_set_data(G_OBJECT(b), "devaction", GINT_TO_POINTER(action));
    g_signal_connect(b, "clicked", G_CALLBACK(on_devbtn), st);
    gtk_box_append(GTK_BOX(bar), b);
}

GtkWidget *input_create_navbar(Session *st) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    add_nav(bar, st, "◀", "Back", RC_AKEYCODE_BACK);
    add_nav(bar, st, "●", "Home", RC_AKEYCODE_HOME);
    add_nav(bar, st, "▢", "Ứng dụng gần đây", RC_AKEYCODE_APP_SWITCH);
    add_nav(bar, st, "☰", "Menu", RC_AKEYCODE_MENU);
    add_nav(bar, st, "⏻", "Power", RC_AKEYCODE_POWER);
    add_nav(bar, st, "🔉", "Giảm âm lượng", RC_AKEYCODE_VOLUME_DOWN);
    add_nav(bar, st, "🔊", "Tăng âm lượng", RC_AKEYCODE_VOLUME_UP);
    add_dev(bar, st, "⟳", "Xoay màn hình", RC_DEVICE_ROTATE);
    add_dev(bar, st, "🔔", "Mở thanh thông báo", RC_DEVICE_EXPAND_NOTIF);
    add_dev(bar, st, "🌙", "Tắt màn hình thiết bị (vẫn mirror)", RC_DEVICE_SCREEN_OFF);
    add_dev(bar, st, "☀", "Bật lại màn hình thiết bị", RC_DEVICE_SCREEN_ON);
    st->bar = bar;
    return bar;
}

void input_attach(Session *st) {
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

    /* Bàn phím gắn vào cửa sổ (hoạt động bất kể focus con nào). */
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), st);
    g_signal_connect(key, "key-released", G_CALLBACK(on_key_released), st);
    gtk_widget_add_controller(st->win, key);
}
