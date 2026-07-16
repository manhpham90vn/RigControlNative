package com.rigcontrol.server;

import android.os.IBinder;

import com.rigcontrol.server.wrappers.ServiceManager;
import com.rigcontrol.server.wrappers.SurfaceControl;

import java.lang.reflect.Method;

/**
 * Thực thi DEVICE_ACTION (docs/PROTOCOL.md §4.6) qua hidden API. Mọi thao tác best-effort:
 * lỗi chỉ log, không làm hỏng phiên video/control.
 *
 *  - SCREEN_OFF/ON : SurfaceControl.setDisplayPowerMode
 *  - panel         : IStatusBarService (expand/collapse)
 *  - ROTATE        : IWindowManager.freezeRotation (toggle hướng)
 */
public final class DeviceController {
    private Object statusBar;
    private Object windowManager;

    public void perform(int action) {
        try {
            switch (action) {
                case Protocol.DEV_SCREEN_OFF:
                    screenPower(false);
                    break;
                case Protocol.DEV_SCREEN_ON:
                    screenPower(true);
                    break;
                case Protocol.DEV_EXPAND_NOTIF:
                    statusBarCall("expandNotificationsPanel");
                    break;
                case Protocol.DEV_EXPAND_SETTINGS:
                    statusBarCall("expandSettingsPanel");
                    break;
                case Protocol.DEV_COLLAPSE_PANELS:
                    statusBarCall("collapsePanels");
                    break;
                case Protocol.DEV_ROTATE:
                    rotate();
                    break;
                default:
                    System.err.println("[rc-server] device action lạ: " + action);
            }
        } catch (Throwable e) {
            System.err.println("[rc-server] device action " + action + " lỗi: " + e);
        }
    }

    private void screenPower(boolean on) {
        IBinder token = SurfaceControl.getInternalDisplayToken();
        if (token == null) {
            System.err.println("[rc-server] không lấy được display token cho screen power");
            return;
        }
        SurfaceControl.setDisplayPowerMode(
            token, on ? SurfaceControl.POWER_MODE_NORMAL : SurfaceControl.POWER_MODE_OFF);
    }

    private Object statusBar() throws Exception {
        if (statusBar == null) {
            IBinder b = ServiceManager.getService("statusbar");
            Class<?> stub = Class.forName("com.android.internal.statusbar.IStatusBarService$Stub");
            statusBar = stub.getMethod("asInterface", IBinder.class).invoke(null, b);
        }
        return statusBar;
    }

    private void statusBarCall(String method) throws Exception {
        Object sb = statusBar();
        if (sb == null) return;
        sb.getClass().getMethod(method).invoke(sb);
    }

    private Object windowManager() throws Exception {
        if (windowManager == null) {
            IBinder b = ServiceManager.getService("window");
            Class<?> stub = Class.forName("android.view.IWindowManager$Stub");
            windowManager = stub.getMethod("asInterface", IBinder.class).invoke(null, b);
        }
        return windowManager;
    }

    private void rotate() throws Exception {
        Object wm = windowManager();
        if (wm == null) return;

        int rotation = 0;
        try {
            rotation = (Integer) wm.getClass().getMethod("getDefaultDisplayRotation").invoke(wm);
        } catch (NoSuchMethodException e) {
            rotation = (Integer) wm.getClass().getMethod("getRotation").invoke(wm);
        }
        int next = (rotation + 1) % 4;

        // Thử vài chữ ký freezeRotation qua các bản Android.
        Method m = findMethod(wm.getClass(), "freezeRotation", int.class);
        if (m != null) {
            m.invoke(wm, next);
            return;
        }
        m = findMethod(wm.getClass(), "freezeRotation", int.class, int.class);
        if (m != null) {
            m.invoke(wm, -1 /* displayId mặc định */, next);
            return;
        }
        System.err.println("[rc-server] không tìm được freezeRotation phù hợp");
    }

    private static Method findMethod(Class<?> cls, String name, Class<?>... params) {
        try {
            return cls.getMethod(name, params);
        } catch (NoSuchMethodException e) {
            return null;
        }
    }
}
