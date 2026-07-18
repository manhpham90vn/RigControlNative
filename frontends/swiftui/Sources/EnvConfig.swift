/*
 * EnvConfig.swift — đọc cấu hình từ biến môi trường (khớp main.c của front-end GTK):
 *   RC_SERIAL, RC_TCP_ADDR, RC_MAX_SIZE, RC_BIT_RATE, RC_MAX_FPS, RC_AUDIO, RC_CONTROL, RC_SHOW_FPS
 *   (RC_HWDEC, RC_SERVER_PATH do libcore tự đọc)
 */
import Foundation

enum EnvConfig {
    static func str(_ name: String) -> String? {
        guard let v = ProcessInfo.processInfo.environment[name], !v.isEmpty else { return nil }
        return v
    }
    static func int(_ name: String, _ def: Int32) -> Int32 {
        guard let v = str(name), let n = Int32(v) else { return def }
        return n
    }

    /// Cấu hình mẫu từ env; showFps mặc định bật.
    static func base() -> SessionConfig {
        var cfg = SessionConfig()
        cfg.serial = str("RC_SERIAL")
        cfg.tcpAddr = str("RC_TCP_ADDR")
        cfg.maxSize = int("RC_MAX_SIZE", 0)
        cfg.bitRate = int("RC_BIT_RATE", 8_000_000)
        cfg.maxFps = int("RC_MAX_FPS", 60)
        cfg.audio = int("RC_AUDIO", 0) != 0
        cfg.control = int("RC_CONTROL", 1) != 0
        cfg.showFps = int("RC_SHOW_FPS", 1) != 0
        return cfg
    }

    /// TCP hoặc serial chỉ định qua env → mở thẳng, bỏ qua bộ chọn.
    static var hasDirectTarget: Bool {
        str("RC_TCP_ADDR") != nil || str("RC_SERIAL") != nil
    }
}
