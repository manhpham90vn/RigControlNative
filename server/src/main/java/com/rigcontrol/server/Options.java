package com.rigcontrol.server;

/** Tham số phiên do desktop truyền qua dòng lệnh app_process (key=value). */
public final class Options {
    public int maxSize = 0; // giới hạn cạnh dài; 0 = full
    public int bitRate = 8_000_000; // bps (video)
    public int maxFps = 60;
    public String codec = "h264";
    public boolean control = true;
    public boolean audio = true; // stream audio thiết bị
    public String audioCodec = "opus";
    public boolean tcp = false; // true = listen TCP (LAN); false = localabstract (USB)
    public int port = Protocol.DEFAULT_TCP_PORT;
    public String socketName = Protocol.LOCALABSTRACT_NAME; // tên localabstract (đa session)

    public static Options parse(String[] args) {
        Options o = new Options();
        for (String a : args) {
            int eq = a.indexOf('=');
            if (eq < 0)
                continue;
            String k = a.substring(0, eq);
            String v = a.substring(eq + 1);
            switch (k) {
                case "max_size":
                    o.maxSize = Integer.parseInt(v);
                    break;
                case "bit_rate":
                    o.bitRate = Integer.parseInt(v);
                    break;
                case "max_fps":
                    o.maxFps = Integer.parseInt(v);
                    break;
                case "codec":
                    o.codec = v;
                    break;
                case "control":
                    o.control = Boolean.parseBoolean(v);
                    break;
                case "audio":
                    o.audio = Boolean.parseBoolean(v);
                    break;
                case "audio_codec":
                    o.audioCodec = v;
                    break;
                case "tcp":
                    o.tcp = Boolean.parseBoolean(v);
                    break;
                case "port":
                    o.port = Integer.parseInt(v);
                    break;
                case "socket_name":
                    o.socketName = v;
                    break;
                default: /* bỏ qua key lạ để tương thích tiến */
                    break;
            }
        }
        return o;
    }
}
