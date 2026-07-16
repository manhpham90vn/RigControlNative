package com.rigcontrol.server.wrappers;

/** Ảnh chụp thông tin một display (đọc từ android.view.DisplayInfo qua reflection). */
public final class DisplayInfo {
    /** Kích thước logic (đã tính theo rotation hiện tại), đơn vị px. */
    public final int width;
    public final int height;
    /** Surface.ROTATION_0..3. */
    public final int rotation;
    /** Layer stack của display — dùng để mirror nội dung sang virtual display. */
    public final int layerStack;
    public final int flags;

    public DisplayInfo(int width, int height, int rotation, int layerStack, int flags) {
        this.width = width;
        this.height = height;
        this.rotation = rotation;
        this.layerStack = layerStack;
        this.flags = flags;
    }
}
