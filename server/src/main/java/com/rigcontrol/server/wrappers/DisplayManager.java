package com.rigcontrol.server.wrappers;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * Wrapper reflection cho {@code android.hardware.display.DisplayManagerGlobal}.
 *
 * Chạy từ app_process (uid shell) không có Context, nên ta lấy instance qua
 * {@code DisplayManagerGlobal.getInstance()} rồi đọc {@code DisplayInfo} bằng reflection.
 */
public final class DisplayManager {
    private final Object manager; // android.hardware.display.DisplayManagerGlobal

    private DisplayManager(Object manager) {
        this.manager = manager;
    }

    public static DisplayManager create() {
        try {
            Class<?> cls = Class.forName("android.hardware.display.DisplayManagerGlobal");
            Method getInstance = cls.getDeclaredMethod("getInstance");
            Object instance = getInstance.invoke(null);
            return new DisplayManager(instance);
        } catch (Exception e) {
            throw new RuntimeException("không khởi tạo được DisplayManagerGlobal", e);
        }
    }

    /** Trả về thông tin display theo id (0 = màn hình chính), hoặc null nếu không có. */
    public DisplayInfo getDisplayInfo(int displayId) {
        try {
            Object info = manager.getClass()
                             .getMethod("getDisplayInfo", int.class)
                             .invoke(manager, displayId);
            if (info == null) {
                return null;
            }
            Class<?> cls = info.getClass();
            int width = getInt(cls, info, "logicalWidth");
            int height = getInt(cls, info, "logicalHeight");
            int rotation = getInt(cls, info, "rotation");
            int layerStack = getInt(cls, info, "layerStack");
            int flags = getInt(cls, info, "flags");
            return new DisplayInfo(width, height, rotation, layerStack, flags);
        } catch (Exception e) {
            throw new RuntimeException("không đọc được DisplayInfo", e);
        }
    }

    private static int getInt(Class<?> cls, Object obj, String field) throws Exception {
        Field f = cls.getDeclaredField(field);
        f.setAccessible(true);
        return f.getInt(obj);
    }
}
