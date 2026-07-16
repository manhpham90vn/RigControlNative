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
    if (!st->client || st->mouse_buttons == 0) return; /* chỉ gửi khi đang kéo */
    float dx, dy;
    if (!render_widget_to_device(st, x, y, &dx, &dy)) return;
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

GtkWidget *input_create_navbar(Session *st) {
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
