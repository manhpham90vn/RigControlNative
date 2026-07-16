package com.rigcontrol.server;

import android.graphics.Rect;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.IBinder;
import android.view.Surface;

import com.rigcontrol.server.wrappers.DisplayInfo;
import com.rigcontrol.server.wrappers.DisplayManager;
import com.rigcontrol.server.wrappers.SurfaceControl;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;

/**
 * Capture màn hình qua SurfaceControl/VirtualDisplay, encode H.264 bằng MediaCodec (input Surface),
 * đóng gói theo video_packet (docs/PROTOCOL.md §3) và ghi ra video socket.
 *
 * Cấu hình low-latency: bitrate-mode CBR, I-frame interval lớn (chỉ IDR khi cần), gợi ý
 * low-latency/latency=1, giới hạn fps. Không B-frame (encoder Surface + realtime không sinh B).
 *
 * Phase 1 chụp ở kích thước hiện tại của display 0; theo dõi xoay động để phase sau.
 */
public final class ScreenEncoder {
    private static final int DISPLAY_ID = 0;
    private static final String DISPLAY_NAME = "rigcontrol";
    private static final int I_FRAME_INTERVAL_S = 10;
    private static final long DEQUEUE_TIMEOUT_US = 100_000; // 100ms

    private final Options options;

    private final int deviceWidth;
    private final int deviceHeight;
    private final int encWidth;
    private final int encHeight;
    private final int layerStack;

    public ScreenEncoder(Options options) {
        this.options = options;
        DisplayInfo info = DisplayManager.create().getDisplayInfo(DISPLAY_ID);
        if (info == null) {
            throw new RuntimeException("không lấy được DisplayInfo cho display " + DISPLAY_ID);
        }
        this.deviceWidth = info.width;
        this.deviceHeight = info.height;
        this.layerStack = info.layerStack;
        int[] size = computeVideoSize(info.width, info.height, options.maxSize);
        this.encWidth = size[0];
        this.encHeight = size[1];
        System.out.println("[rc-server] display " + deviceWidth + "x" + deviceHeight
            + " -> encode " + encWidth + "x" + encHeight);
    }

    public int getWidth() {
        return encWidth;
    }
    public int getHeight() {
        return encHeight;
    }
    public int getDeviceWidth() {
        return deviceWidth;
    }
    public int getDeviceHeight() {
        return deviceHeight;
    }

    /** Chạy vòng encode, ghi liên tục ra out cho tới khi socket đóng hoặc encoder dừng. */
    public void streamTo(OutputStream out) throws IOException {
        MediaFormat format = createFormat();
        MediaCodec codec;
        try {
            codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        } catch (IOException e) {
            throw new IOException("không tạo được encoder H.264", e);
        }

        Surface surface = null;
        IBinder display = null;
        try {
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            surface = codec.createInputSurface();
            display = createVirtualDisplay(surface);
            codec.start();
            encodeLoop(codec, out);
        } catch (IOException e) {
            throw e;
        } catch (Exception e) {
            throw new IOException("lỗi encode video", e);
        } finally {
            if (display != null) {
                try {
                    SurfaceControl.destroyDisplay(display);
                } catch (Exception ignored) {
                }
            }
            try {
                codec.stop();
            } catch (Exception ignored) {
            }
            codec.release();
            if (surface != null) {
                surface.release();
            }
        }
    }

    private void encodeLoop(MediaCodec codec, OutputStream out) throws IOException {
        MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();
        byte[] buffer = new byte[PACKET_HDR + 256 * 1024];
        while (true) {
            int index = codec.dequeueOutputBuffer(bufferInfo, DEQUEUE_TIMEOUT_US);
            if (index < 0) {
                // Mọi mã INFO_* (TRY_AGAIN/FORMAT_CHANGED/...) đều âm: chưa có output buffer.
                continue;
            }
            try {
                boolean eos = (bufferInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                if (bufferInfo.size > 0 && !eos) {
                    ByteBuffer bb = codec.getOutputBuffer(index);
                    bb.position(bufferInfo.offset);
                    bb.limit(bufferInfo.offset + bufferInfo.size);
                    if (buffer.length < PACKET_HDR + bufferInfo.size) {
                        buffer = new byte[PACKET_HDR + bufferInfo.size];
                    }
                    bb.get(buffer, PACKET_HDR, bufferInfo.size);
                    boolean config =
                        (bufferInfo.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
                    boolean key = (bufferInfo.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                    writePacket(out, config, key, bufferInfo.presentationTimeUs, buffer,
                        bufferInfo.size);
                }
                if (eos) {
                    break;
                }
            } finally {
                codec.releaseOutputBuffer(index, false);
            }
        }
    }

    private MediaFormat createFormat() {
        MediaFormat format = new MediaFormat();
        format.setString(MediaFormat.KEY_MIME, MediaFormat.MIMETYPE_VIDEO_AVC);
        format.setInteger(MediaFormat.KEY_WIDTH, encWidth);
        format.setInteger(MediaFormat.KEY_HEIGHT, encHeight);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
            MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        format.setInteger(MediaFormat.KEY_BIT_RATE, options.bitRate);
        int fps = options.maxFps > 0 ? options.maxFps : 60;
        format.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, I_FRAME_INTERVAL_S);
        format.setInteger(MediaFormat.KEY_BITRATE_MODE,
            MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
        // Gợi ý low-latency (bỏ qua nếu encoder không hỗ trợ).
        format.setInteger(MediaFormat.KEY_LATENCY, 1);
        format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1); // API 30+
        format.setInteger(MediaFormat.KEY_PRIORITY, 0);    // 0 = realtime
        format.setFloat("max-fps-to-encoder", fps);
        return format;
    }

    private IBinder createVirtualDisplay(Surface surface) {
        IBinder display = SurfaceControl.createDisplay(DISPLAY_NAME, false);
        SurfaceControl.openTransaction();
        try {
            SurfaceControl.setDisplaySurface(display, surface);
            SurfaceControl.setDisplayProjection(display, 0,
                new Rect(0, 0, deviceWidth, deviceHeight),
                new Rect(0, 0, encWidth, encHeight));
            SurfaceControl.setDisplayLayerStack(display, layerStack);
        } finally {
            SurfaceControl.closeTransaction();
        }
        return display;
    }

    /**
     * Tính kích thước encode: giới hạn cạnh dài về maxSize (nếu >0), giữ tỉ lệ, làm tròn xuống
     * bội số 8 (yêu cầu của hầu hết encoder H.264).
     */
    static int[] computeVideoSize(int w, int h, int maxSize) {
        if (maxSize > 0) {
            int major = Math.max(w, h);
            int minor = Math.min(w, h);
            if (major > maxSize) {
                int minorScaled = minor * maxSize / major;
                if (w > h) {
                    w = maxSize;
                    h = minorScaled;
                } else {
                    h = maxSize;
                    w = minorScaled;
                }
            }
        }
        return new int[] {w & ~7, h & ~7};
    }

    /** Header packet: [pts_flags:u64][len:u32] — payload phải nằm sau PACKET_HDR byte. */
    static final int PACKET_HDR = 12;

    /**
     * Đóng gói 1 packet: [pts_flags:u64][len:u32][payload]. `packet` chứa payload từ offset
     * PACKET_HDR (12 byte đầu chừa cho header) → ghi socket đúng 1 lần, không cấp phát.
     */
    static void writePacket(OutputStream out, boolean config, boolean key, long ptsUs,
        byte[] packet, int len) throws IOException {
        long flags = (ptsUs & Protocol.PKT_PTS_MASK);
        if (config)
            flags |= Protocol.PKT_FLAG_CONFIG;
        if (key)
            flags |= Protocol.PKT_FLAG_KEYFRAME;
        for (int i = 0; i < 8; i++) packet[i] = (byte) (flags >>> (56 - 8 * i));
        packet[8] = (byte) (len >>> 24);
        packet[9] = (byte) (len >>> 16);
        packet[10] = (byte) (len >>> 8);
        packet[11] = (byte) len;
        out.write(packet, 0, PACKET_HDR + len);
        out.flush();
    }
}
