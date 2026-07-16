package com.rigcontrol.server;

import android.content.pm.ApplicationInfo;
import android.os.Looper;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;

/**
 * Vá môi trường app_process để các API cần "app context" chạy được khi ta không phải một app.
 *
 * Cụ thể {@code AudioRecord} (capture audio) yêu cầu một {@code ActivityThread} +
 * {@code ApplicationInfo} hợp lệ để tra AppOps/package name; chạy trần qua app_process thì các
 * thứ này null. Ta dựng một ActivityThread giả với package "com.android.shell" (kỹ thuật giống
 * scrcpy). Chỉ cần khi bật audio.
 */
public final class Workarounds {
    private Workarounds() {}

    /** AudioRecord cần một Looper trên thread khởi tạo. */
    public static void prepareMainLooper() {
        if (Looper.myLooper() == null) {
            Looper.prepareMainLooper();
        }
    }

    /** Dựng ActivityThread giả để AudioRecord có package name hợp lệ. */
    public static void fillAppInfo() {
        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");

            Constructor<?> atCtor = activityThreadClass.getDeclaredConstructor();
            atCtor.setAccessible(true);
            Object activityThread = atCtor.newInstance();

            Field sCurrentActivityThread =
                activityThreadClass.getDeclaredField("sCurrentActivityThread");
            sCurrentActivityThread.setAccessible(true);
            sCurrentActivityThread.set(null, activityThread);

            Class<?> appBindDataClass = Class.forName("android.app.ActivityThread$AppBindData");
            Constructor<?> abdCtor = appBindDataClass.getDeclaredConstructor();
            abdCtor.setAccessible(true);
            Object appBindData = abdCtor.newInstance();

            ApplicationInfo appInfo = new ApplicationInfo();
            appInfo.packageName = "com.android.shell";

            Field appInfoField = appBindDataClass.getDeclaredField("appInfo");
            appInfoField.setAccessible(true);
            appInfoField.set(appBindData, appInfo);

            Field mBoundApplication = activityThreadClass.getDeclaredField("mBoundApplication");
            mBoundApplication.setAccessible(true);
            mBoundApplication.set(activityThread, appBindData);
        } catch (Throwable e) {
            // Không chặn video: nếu vá thất bại, audio sẽ tự báo không khả dụng.
            System.err.println("[rc-server] Workarounds.fillAppInfo thất bại: " + e);
        }
    }
}
