package com.rigcontrol.server.wrappers;

import android.os.IBinder;

import java.lang.reflect.Method;

/** Lấy binder của system service qua {@code android.os.ServiceManager.getService} (hidden API). */
public final class ServiceManager {
    private static Method getService;

    private ServiceManager() {}

    public static IBinder getService(String name) {
        try {
            if (getService == null) {
                getService = Class.forName("android.os.ServiceManager")
                                 .getMethod("getService", String.class);
            }
            return (IBinder) getService.invoke(null, name);
        } catch (Exception e) {
            return null;
        }
    }
}
