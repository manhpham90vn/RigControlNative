package com.rigcontrol.server;

import android.os.Build;

/**
 * Entry point chạy qua app_process (không phải Activity/APK).
 *
 * Khởi động (desktop dựng lệnh này ở Phase 2):
 *   adb shell CLASSPATH=/data/local/tmp/rc-server \
 *     app_process / com.rigcontrol.server.Server max_size=0 bit_rate=8000000 ...
 *
 * Phase 1: mở socket, gửi device_meta, stream video (H.264) + audio (Opus).
 * Control inject là Phase 4 (hiện chỉ đọc & bỏ qua để giữ đúng thứ tự socket).
 */
public final class Server {
    public static void main(String[] args) {
        Options options = Options.parse(args);
        System.out.println("[rc-server] khởi động: codec=" + options.codec + " maxSize="
            + options.maxSize + " bitRate=" + options.bitRate + " maxFps=" + options.maxFps
            + " audio=" + options.audio + " control=" + options.control + " tcp=" + options.tcp);

        try {
            DesktopConnection conn = DesktopConnection.open(options);
            try {
                ScreenEncoder encoder = new ScreenEncoder(options);
                conn.sendDeviceMeta(Protocol.CODEC_ID_H264, encoder.getWidth(),
                    encoder.getHeight(), deviceName());

                if (options.audio && conn.getAudioOutput() != null) {
                    Thread at = new Thread(() -> runAudio(options, conn), "rc-audio");
                    at.setDaemon(true);
                    at.start();
                }

                if (options.control && conn.getControlInput() != null) {
                    ControlReceiver receiver = new ControlReceiver(
                        conn.getControlInput(), encoder, new DeviceController());
                    Thread t = new Thread(receiver, "rc-control");
                    t.setDaemon(true);
                    t.start();
                }

                encoder.streamTo(conn.getVideoOutput());
            } finally {
                conn.close();
            }
        } catch (Throwable t) {
            System.err.println("[rc-server] lỗi: " + t);
            t.printStackTrace();
            System.exit(1);
        }
    }

    /**
     * Chạy toàn bộ pipeline audio trên thread riêng: quyết định codec, gửi audio_meta, stream.
     * Nếu không khả dụng → gửi ACODEC_ID_NONE và giữ socket mở (video không bị ảnh hưởng).
     */
    private static void runAudio(Options options, DesktopConnection conn) {
        AudioEncoder audio = new AudioEncoder(options);
        int codecId = Protocol.ACODEC_ID_NONE;
        boolean ready = false;
        if (audio.isAvailable()) {
            try {
                audio.prepare();
                codecId = audio.codecId();
                ready = true;
            } catch (Throwable e) {
                System.err.println("[rc-server] audio không khả dụng, gửi NONE: " + e);
                codecId = Protocol.ACODEC_ID_NONE;
            }
        } else {
            System.err.println("[rc-server] audio cần Android 11+; gửi NONE");
        }

        try {
            conn.sendAudioMeta(codecId, audio.sampleRate(), audio.channels());
            if (ready) {
                audio.streamTo(conn.getAudioOutput());
            }
        } catch (Throwable e) {
            System.err.println("[rc-server] audio kết thúc: " + e);
        } finally {
            audio.release();
        }
    }

    private static String deviceName() {
        String name = Build.MODEL;
        return name != null ? name : "Android";
    }
}
