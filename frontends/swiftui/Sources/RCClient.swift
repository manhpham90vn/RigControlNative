/*
 * RCClient.swift — wrapper Swift quanh C API của libcore (rc_client).
 *
 * Giữ callback frame/status của core (chạy trên thread nội bộ của core) và chuyển tiếp qua closure
 * Swift. Dữ liệu frame chỉ hợp lệ TRONG callback nên consumer phải copy ngay (giống hợp đồng C).
 */
import Foundation
import RCCore

/// Cấu hình một phiên (map sang rc_config khi tạo client).
struct SessionConfig {
    var serial: String?       // adb serial; "ip:port" = wireless adb (core tự adb connect)
    var tcpAddr: String?      // "ip[:port]" → LAN trực tiếp; nil = qua adb tunnel
    var tcpDevicePort: Int32 = 0 // cổng server listen TRONG thiết bị (khác khi qua rc-agent)
    var maxSize: Int32 = 0
    var bitRate: Int32 = 8_000_000
    var maxFps: Int32 = 60
    var control: Bool = true
    var audio: Bool = false
    var showFps: Bool = true

    var isTCP: Bool { tcpAddr != nil }
}

/// Frame giải mã, chỉ hợp lệ trong phạm vi callback — copy ngay nếu cần giữ.
struct RCFrameRef {
    let width: Int
    let height: Int
    let pixfmt: rc_pixfmt
    let fullRange: Bool
    let bt709: Bool
    let data: (UnsafePointer<UInt8>?, UnsafePointer<UInt8>?, UnsafePointer<UInt8>?)
    let linesize: (Int, Int, Int)
}

final class RCClient {
    private var handle: OpaquePointer?

    /// Gọi trên thread nội bộ của core — consumer copy ngay rồi marshal về main thread.
    var onFrame: ((RCFrameRef) -> Void)?
    /// Gọi trên thread nội bộ của core.
    var onStatus: ((rc_status, String) -> Void)?

    init?(config: SessionConfig) {
        // serial/tcp_addr chỉ cần sống trong lúc create (core copy nội bộ). Lồng withCString.
        func build(_ serialPtr: UnsafePointer<CChar>?, _ tcpPtr: UnsafePointer<CChar>?) -> OpaquePointer? {
            var cfg = rc_config()
            cfg.serial = serialPtr
            cfg.transport = config.isTCP ? RC_TRANSPORT_TCP : RC_TRANSPORT_USB
            cfg.tcp_addr = tcpPtr
            cfg.tcp_device_port = config.tcpDevicePort
            cfg.max_size = config.maxSize
            cfg.bit_rate = config.bitRate
            cfg.max_fps = config.maxFps
            cfg.codec = RC_CODEC_H264
            cfg.control = config.control ? 1 : 0
            cfg.audio = config.audio ? 1 : 0
            cfg.audio_codec = RC_ACODEC_OPUS
            return rc_client_create(&cfg)
        }

        func withTcp(_ serialPtr: UnsafePointer<CChar>?) -> OpaquePointer? {
            if let tcp = config.tcpAddr {
                return tcp.withCString { build(serialPtr, $0) }
            }
            return build(serialPtr, nil)
        }

        if let serial = config.serial {
            handle = serial.withCString { withTcp($0) }
        } else {
            handle = withTcp(nil)
        }
        guard handle != nil else { return nil }

        let user = Unmanaged.passUnretained(self).toOpaque()
        rc_client_set_frame_callback(handle, RCClient.frameThunk, user)
        rc_client_set_status_callback(handle, RCClient.statusThunk, user)
    }

    func start() -> rc_status { rc_client_start(handle) }
    func abort() { rc_client_abort(handle) }
    func stop() { rc_client_stop(handle) }

    func destroy() {
        if let h = handle { rc_client_destroy(h); handle = nil }
    }

    // ---- Thông tin phiên ----
    var decoderDesc: String? {
        guard let p = rc_client_get_decoder_desc(handle) else { return nil }
        return String(cString: p)
    }
    var transportDesc: String? {
        guard let p = rc_client_get_transport_desc(handle) else { return nil }
        return String(cString: p)
    }
    var deviceSize: (Int, Int)? {
        var w: Int32 = 0, h: Int32 = 0
        guard rc_client_get_device_size(handle, &w, &h) == RC_OK else { return nil }
        return (Int(w), Int(h))
    }

    // ---- Input ----
    func sendMouseMotion(buttons: UInt32, x: Float, y: Float) {
        rc_client_send_mouse_motion(handle, buttons, x, y)
    }
    func sendMouseButton(action: Int32, button: UInt32, buttons: UInt32, x: Float, y: Float) {
        rc_client_send_mouse_button(handle, action, button, buttons, x, y)
    }
    func sendScroll(x: Float, y: Float, h: Float, v: Float) {
        rc_client_send_scroll(handle, x, y, h, v)
    }
    func sendKey(action: Int32, keycode: UInt32, meta: UInt32, repeatCount: UInt32) {
        rc_client_send_key(handle, action, keycode, meta, repeatCount)
    }
    func sendText(_ s: String) { _ = s.withCString { rc_client_send_text(handle, $0) } }
    func clickButton(_ keycode: UInt32) { rc_client_click_button(handle, keycode) }
    func sendDeviceAction(_ action: rc_device_action) { rc_client_send_device_action(handle, action) }

    // ---- Thunk C → Swift ----
    private static let frameThunk: rc_frame_cb = { framePtr, user in
        guard let framePtr = framePtr, let user = user else { return }
        let client = Unmanaged<RCClient>.fromOpaque(user).takeUnretainedValue()
        let f = framePtr.pointee
        let ref = RCFrameRef(
            width: Int(f.width),
            height: Int(f.height),
            pixfmt: f.format,
            fullRange: f.full_range != 0,
            bt709: f.bt709 != 0,
            data: (UnsafePointer(f.data.0), UnsafePointer(f.data.1), UnsafePointer(f.data.2)),
            linesize: (Int(f.linesize.0), Int(f.linesize.1), Int(f.linesize.2)))
        client.onFrame?(ref)
    }

    private static let statusThunk: rc_status_cb = { code, msgPtr, user in
        guard let user = user else { return }
        let client = Unmanaged<RCClient>.fromOpaque(user).takeUnretainedValue()
        let msg = msgPtr != nil ? String(cString: msgPtr!) : ""
        client.onStatus?(code, msg)
    }
}

/// Chuỗi mô tả mã lỗi (tiện log).
func rcStatusString(_ code: rc_status) -> String {
    guard let p = rc_status_str(code) else { return "?" }
    return String(cString: p)
}
