package com.rigcontrol.server;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import java.io.Closeable;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;
import java.util.List;

/**
 * Quản lý các socket (video / [audio] / [control]) tới desktop và gửi các gói meta.
 *
 * USB: mỗi luồng là một {@link LocalSocket} connect tới localabstract "rigcontrol" (desktop đã
 * {@code adb reverse localabstract:rigcontrol tcp:<port>} và đang listen).
 * TCP/LAN: server listen 0.0.0.0:port rồi accept lần lượt.
 *
 * Thứ tự thiết lập luôn cố định: video → [audio nếu bật] → [control nếu bật]
 * (xem docs/PROTOCOL.md §1).
 */
public final class DesktopConnection implements Closeable {
    private final List<Closeable> closeables = new ArrayList<>();
    private ServerSocket serverSocket;

    private OutputStream videoOut;
    private OutputStream audioOut;
    private InputStream controlIn;
    private OutputStream controlOut;

    private DesktopConnection() {}

    public static DesktopConnection open(Options options) throws IOException {
        DesktopConnection conn = new DesktopConnection();
        try {
            if (options.tcp) {
                conn.openTcp(options);
            } else {
                conn.openUsb(options);
            }
        } catch (IOException e) {
            conn.close();
            throw e;
        }
        return conn;
    }

    private void openTcp(Options options) throws IOException {
        serverSocket = new ServerSocket(options.port, 8, InetAddress.getByName("0.0.0.0"));
        closeables.add(serverSocket);
        System.out.println("[rc-server] listen TCP 0.0.0.0:" + options.port
            + (options.token != null ? " (yêu cầu token)" : ""));

        Socket video = acceptAuthed(options, "video");
        register(video);
        videoOut = video.getOutputStream();

        if (options.audio) {
            Socket audio = acceptAuthed(options, "audio");
            register(audio);
            audioOut = audio.getOutputStream();
        }
        if (options.control) {
            Socket control = acceptAuthed(options, "control");
            register(control);
            controlIn = control.getInputStream();
            controlOut = control.getOutputStream();
        }
    }

    /**
     * Accept một kết nối TCP; nếu server chạy với {@code token=...} thì client phải gửi đúng
     * token ({@link Protocol#LAN_TOKEN_LEN} byte ASCII) trước khi được phục vụ — cổng LAN mở
     * cho cả mạng nên đây là hàng rào chống bên lạ chiếm stream/inject input. Kết nối sai
     * token (hoặc im lặng quá 5s) bị đóng và server accept tiếp.
     */
    private Socket acceptAuthed(Options options, String what) throws IOException {
        while (true) {
            Socket s = serverSocket.accept();
            if (options.token == null) {
                System.out.println(
                    "[rc-server] accept " + what + " từ " + s.getRemoteSocketAddress());
                return s;
            }
            try {
                s.setSoTimeout(5000); // client hợp lệ gửi token ngay; đừng để kẻ lạ giữ slot
                byte[] buf = new byte[Protocol.LAN_TOKEN_LEN];
                InputStream in = s.getInputStream();
                int off = 0;
                while (off < buf.length) {
                    int n = in.read(buf, off, buf.length - off);
                    if (n < 0) {
                        break;
                    }
                    off += n;
                }
                if (off == buf.length
                    && java.security.MessageDigest.isEqual(
                        buf, options.token.getBytes("US-ASCII"))) {
                    s.setSoTimeout(0);
                    System.out.println("[rc-server] accept " + what + " từ "
                        + s.getRemoteSocketAddress() + " (token OK)");
                    return s;
                }
                System.err.println("[rc-server] kết nối TCP " + what + " từ "
                    + s.getRemoteSocketAddress() + " sai token — đóng");
            } catch (IOException e) {
                System.err.println("[rc-server] lỗi đọc token (" + what + " từ "
                    + s.getRemoteSocketAddress() + "): " + e);
            }
            try {
                s.close();
            } catch (IOException ignored) {
            }
        }
    }

    private void openUsb(Options options) throws IOException {
        System.out.println("[rc-server] connect localabstract:" + options.socketName);
        LocalSocket video = connectLocal(options.socketName);
        videoOut = video.getOutputStream();

        if (options.audio) {
            LocalSocket audio = connectLocal(options.socketName);
            audioOut = audio.getOutputStream();
        }
        if (options.control) {
            LocalSocket control = connectLocal(options.socketName);
            controlIn = control.getInputStream();
            controlOut = control.getOutputStream();
        }
    }

    private LocalSocket connectLocal(String name) throws IOException {
        LocalSocket socket = new LocalSocket();
        socket.connect(new LocalSocketAddress(name, LocalSocketAddress.Namespace.ABSTRACT));
        closeables.add(socket);
        return socket;
    }

    private void register(Socket socket) {
        try {
            socket.setTcpNoDelay(true); // giảm độ trễ: gửi ngay, không Nagle
        } catch (Exception ignored) {
        }
        closeables.add(socket);
    }

    /** Gửi device_meta (docs/PROTOCOL.md §2) qua video socket — gọi một lần trước khi stream. */
    public void sendDeviceMeta(int codecId, int width, int height, String deviceName)
        throws IOException {
        writeDeviceMeta(videoOut, codecId, width, height, deviceName);
    }

    /** Gửi audio_meta (docs/PROTOCOL.md §3b) qua audio socket nếu có. */
    public void sendAudioMeta(int codecId, int sampleRate, int channels) throws IOException {
        if (audioOut != null) {
            writeAudioMeta(audioOut, codecId, sampleRate, channels);
        }
    }

    static void writeDeviceMeta(OutputStream out, int codecId, int width, int height,
        String deviceName) throws IOException {
        byte[] buf = new byte[4 + 2 + 4 + 2 + 2 + Protocol.DEVICE_NAME_LEN];
        int p = 0;
        p = putInt(buf, p, Protocol.META_MAGIC);
        p = putShort(buf, p, Protocol.VERSION);
        p = putInt(buf, p, codecId);
        p = putShort(buf, p, width);
        p = putShort(buf, p, height);
        byte[] name = deviceName.getBytes("UTF-8");
        System.arraycopy(name, 0, buf, p, Math.min(name.length, Protocol.DEVICE_NAME_LEN - 1));
        out.write(buf);
        out.flush();
    }

    static void writeAudioMeta(OutputStream out, int codecId, int sampleRate, int channels)
        throws IOException {
        byte[] buf = new byte[4 + 4 + 4 + 1];
        int p = 0;
        p = putInt(buf, p, Protocol.AUDIO_MAGIC);
        p = putInt(buf, p, codecId);
        p = putInt(buf, p, sampleRate);
        buf[p] = (byte) channels;
        out.write(buf);
        out.flush();
    }

    private static int putInt(byte[] b, int p, int v) {
        b[p] = (byte) (v >>> 24);
        b[p + 1] = (byte) (v >>> 16);
        b[p + 2] = (byte) (v >>> 8);
        b[p + 3] = (byte) v;
        return p + 4;
    }
    private static int putShort(byte[] b, int p, int v) {
        b[p] = (byte) (v >>> 8);
        b[p + 1] = (byte) v;
        return p + 2;
    }

    public OutputStream getVideoOutput() {
        return videoOut;
    }
    public OutputStream getAudioOutput() {
        return audioOut;
    }
    public InputStream getControlInput() {
        return controlIn;
    }
    public OutputStream getControlOutput() {
        return controlOut;
    }

    @Override
    public void close() {
        for (int i = closeables.size() - 1; i >= 0; i--) {
            try {
                closeables.get(i).close();
            } catch (Exception ignored) {
            }
        }
        closeables.clear();
    }
}
