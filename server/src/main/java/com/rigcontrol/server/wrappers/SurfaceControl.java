package com.rigcontrol.server.wrappers;

import android.graphics.Rect;
import android.os.IBinder;
import android.view.Surface;

import java.lang.reflect.Method;

/**
 * Wrapper reflection cho {@code android.view.SurfaceControl} (hidden API).
 *
 * Dùng cách "classic" như scrcpy trên Android ≤ 13: tạo một display ảo, gắn Surface đầu vào
 * của encoder vào nó và cho nó soi cùng layer stack với màn hình chính để mirror nội dung.
 *
 * Ghi chú tương thích: {@code createDisplay(String, boolean)} bị gỡ ở Android 14 (API 34);
 * hỗ trợ thiết bị mới hơn là việc của phase sau.
 */
public final class SurfaceControl {
    private static final Class<?> CLASS;

    static {
        try {
            CLASS = Class.forName("android.view.SurfaceControl");
        } catch (ClassNotFoundException e) {
            throw new AssertionError(e);
        }
    }

    private static Method createDisplay;
    private static Method destroyDisplay;
    private static Method setDisplaySurface;
    private static Method setDisplayProjection;
    private static Method setDisplayLayerStack;
    private static Method openTransaction;
    private static Method closeTransaction;
    private static Method getInternalDisplayToken;
    private static Method setDisplayPowerMode;

    public static final int POWER_MODE_OFF = 0;
    public static final int POWER_MODE_NORMAL = 2;

    private SurfaceControl() {}

    private static Method method(String name, Class<?>... params) {
        try {
            Method m = CLASS.getDeclaredMethod(name, params);
            m.setAccessible(true);
            return m;
        } catch (NoSuchMethodException e) {
            return null;
        }
    }

    public static IBinder createDisplay(String name, boolean secure) {
        try {
            if (createDisplay == null) {
                createDisplay = method("createDisplay", String.class, boolean.class);
            }
            return (IBinder) createDisplay.invoke(null, name, secure);
        } catch (Exception e) {
            throw new RuntimeException("SurfaceControl.createDisplay thất bại", e);
        }
    }

    public static void destroyDisplay(IBinder displayToken) {
        try {
            if (destroyDisplay == null) {
                destroyDisplay = method("destroyDisplay", IBinder.class);
            }
            destroyDisplay.invoke(null, displayToken);
        } catch (Exception e) {
            throw new RuntimeException("SurfaceControl.destroyDisplay thất bại", e);
        }
    }

    public static void setDisplaySurface(IBinder displayToken, Surface surface) {
        try {
            if (setDisplaySurface == null) {
                setDisplaySurface = method("setDisplaySurface", IBinder.class, Surface.class);
            }
            setDisplaySurface.invoke(null, displayToken, surface);
        } catch (Exception e) {
            throw new RuntimeException("SurfaceControl.setDisplaySurface thất bại", e);
        }
    }

    public static void setDisplayProjection(
        IBinder displayToken, int orientation, Rect layerStackRect, Rect displayRect) {
        try {
            if (setDisplayProjection == null) {
                setDisplayProjection = method(
                    "setDisplayProjection", IBinder.class, int.class, Rect.class, Rect.class);
            }
            setDisplayProjection.invoke(null, displayToken, orientation, layerStackRect, displayRect);
        } catch (Exception e) {
            throw new RuntimeException("SurfaceControl.setDisplayProjection thất bại", e);
        }
    }

    public static void setDisplayLayerStack(IBinder displayToken, int layerStack) {
        try {
            if (setDisplayLayerStack == null) {
                setDisplayLayerStack = method("setDisplayLayerStack", IBinder.class, int.class);
            }
            setDisplayLayerStack.invoke(null, displayToken, layerStack);
        } catch (Exception e) {
            throw new RuntimeException("SurfaceControl.setDisplayLayerStack thất bại", e);
        }
    }

    /** Mở global transaction (không tồn tại ở một số bản → bỏ qua). */
    public static void openTransaction() {
        try {
            if (openTransaction == null) {
                openTransaction = method("openTransaction");
            }
            if (openTransaction != null) {
                openTransaction.invoke(null);
            }
        } catch (Exception e) {
            throw new RuntimeException("SurfaceControl.openTransaction thất bại", e);
        }
    }

    public static void closeTransaction() {
        try {
            if (closeTransaction == null) {
                closeTransaction = method("closeTransaction");
            }
            if (closeTransaction != null) {
                closeTransaction.invoke(null);
            }
        } catch (Exception e) {
            throw new RuntimeException("SurfaceControl.closeTransaction thất bại", e);
        }
    }

    /** Token màn hình chính (để đổi power mode); null nếu API không có. */
    public static IBinder getInternalDisplayToken() {
        try {
            if (getInternalDisplayToken == null) {
                getInternalDisplayToken = method("getInternalDisplayToken");
            }
            return getInternalDisplayToken != null ? (IBinder) getInternalDisplayToken.invoke(null)
                                                    : null;
        } catch (Exception e) {
            return null;
        }
    }

    public static void setDisplayPowerMode(IBinder displayToken, int mode) {
        try {
            if (setDisplayPowerMode == null) {
                setDisplayPowerMode = method("setDisplayPowerMode", IBinder.class, int.class);
            }
            if (setDisplayPowerMode != null) {
                setDisplayPowerMode.invoke(null, displayToken, mode);
            }
        } catch (Exception e) {
            System.err.println("[rc-server] setDisplayPowerMode lỗi: " + e);
        }
    }
}
