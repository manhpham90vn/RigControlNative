package com.rigcontrol.server;

/**
 * Hằng số giao thức — phải khớp docs/PROTOCOL.md và core/src/rc_internal.h.
 * KHÔNG đổi nghĩa hằng cũ; chỉ thêm mới ở cuối.
 */
public final class Protocol {
    private Protocol() {}

    public static final int META_MAGIC = 0x52434E31; // "RCN1"
    public static final int VERSION = 1;

    public static final int CODEC_ID_H264 = 0x68323634; // "h264"
    public static final int CODEC_ID_H265 = 0x68323635; // "h265"
    public static final int CODEC_ID_AV1 = 0x00617631; // "\0av1"

    public static final int AUDIO_MAGIC = 0x52434E41; // "RCNA"
    public static final int ACODEC_ID_NONE = 0x00000000; // audio không khả dụng
    public static final int ACODEC_ID_OPUS = 0x6F707573; // "opus"
    public static final int ACODEC_ID_AAC = 0x00616163; // "\0aac"
    public static final int ACODEC_ID_RAW = 0x00726177; // "\0raw"

    public static final int DEVICE_NAME_LEN = 64;

    // Cờ trong pts_flags (uint64) của video_packet
    public static final long PKT_FLAG_CONFIG = 1L << 63;
    public static final long PKT_FLAG_KEYFRAME = 1L << 62;
    public static final long PKT_PTS_MASK = (1L << 62) - 1;

    // Control message types
    public static final int CTRL_MOUSE_MOTION = 0;
    public static final int CTRL_MOUSE_BUTTON = 1;
    public static final int CTRL_SCROLL = 2;
    public static final int CTRL_KEY = 3;
    public static final int CTRL_TEXT = 4;
    public static final int CTRL_DEVICE_ACTION = 5;

    // DEVICE_ACTION ids (xem docs/PROTOCOL.md §4.6)
    public static final int DEV_SCREEN_OFF = 0;
    public static final int DEV_SCREEN_ON = 1;
    public static final int DEV_EXPAND_NOTIF = 2;
    public static final int DEV_EXPAND_SETTINGS = 3;
    public static final int DEV_COLLAPSE_PANELS = 4;
    public static final int DEV_ROTATE = 5;

    // Button bitmask
    public static final int BUTTON_LEFT = 0x01;
    public static final int BUTTON_RIGHT = 0x02;
    public static final int BUTTON_MIDDLE = 0x04;

    public static final String LOCALABSTRACT_NAME = "rigcontrol";
    public static final int DEFAULT_TCP_PORT = 27183;
}
