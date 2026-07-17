package com.rigcontrol.server.wrappers;

import android.view.InputEvent;
import java.lang.reflect.Method;

/**
 * Wrapper reflection để inject input event qua InputManager (hidden API).
 *
 * Android 14+ (API 34) đã bỏ {@code InputManager.getInstance()} → dùng
 * {@code InputManagerGlobal.getInstance()}; wrapper thử cả hai. Inject bằng chế độ ASYNC để độ
 * trễ thấp. Chạy từ app_process (uid shell có INJECT_EVENTS).
 */
public final class InputManager {
    public static final int INJECT_MODE_ASYNC = 0;

    private final Object manager;
    private final Method injectMethod;

    private InputManager(Object manager, Method injectMethod) {
        this.manager = manager;
        this.injectMethod = injectMethod;
    }

    public static InputManager create() {
        try {
            Object mgr = null;
            try {
                Class<?> g = Class.forName("android.hardware.input.InputManagerGlobal");
                mgr = g.getMethod("getInstance").invoke(null);
            } catch (Throwable ignore) {
                /* thiết bị cũ: rơi xuống InputManager.getInstance() */
            }
            if (mgr == null) {
                Class<?> c = Class.forName("android.hardware.input.InputManager");
                mgr = c.getMethod("getInstance").invoke(null);
            }
            Method m = mgr.getClass().getMethod("injectInputEvent", InputEvent.class, int.class);
            m.setAccessible(true);
            return new InputManager(mgr, m);
        } catch (Exception e) {
            throw new RuntimeException("khởi tạo InputManager thất bại", e);
        }
    }

    public boolean inject(InputEvent event) {
        try {
            Object r = injectMethod.invoke(manager, event, INJECT_MODE_ASYNC);
            return r instanceof Boolean ? (Boolean) r : true;
        } catch (Exception e) {
            return false;
        }
    }
}
