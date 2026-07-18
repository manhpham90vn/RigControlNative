/*
 * SessionModel.swift — vòng đời một phiên mirror: tạo rc_client, chạy start trên thread nền, nhận
 * frame/status, tự resize cửa sổ theo video (xoay máy), ghép FPS + đường stream + backend decode
 * vào tiêu đề. Tái hiện session.c của front-end GTK.
 */
import AppKit
import Combine
import RCCore

final class SessionModel: ObservableObject {
    let config: SessionConfig
    let tag: String
    private(set) var client: RCClient?

    // Liên kết với view Metal (đặt bởi MetalVideoView.makeNSView).
    weak var metalView: RCMetalView?
    var renderer: VideoRenderer?

    @Published var titleSuffix = "đang kết nối…"

    // Dùng cho cấp cổng đa phiên trong SessionManager.
    var lanPort = 0
    var streamK = -1
    var serialForAlloc: String?

    private var startThread: Thread?
    private(set) var torn = false
    private var fpsTimer: Timer?
    private var titleResolved = false
    private var frameCounter = Atomic(0)
    weak var window: NSWindow?

    init(config: SessionConfig, tag: String) {
        self.config = config
        self.tag = tag
        self.client = RCClient(config: config)
    }

    /// Gọi sau khi view Metal sẵn sàng (đã gắn renderer). An toàn gọi nhiều lần.
    func startIfNeeded() {
        guard let client = client, startThread == nil, !torn else { return }
        client.onFrame = { [weak self] frame in
            guard let self = self else { return }
            self.renderer?.store.store(frame)
            self.frameCounter.increment()
            DispatchQueue.main.async { self.metalView?.setNeedsDisplay(self.metalView?.bounds ?? .zero) }
        }
        client.onStatus = { code, msg in
            if code == RC_OK { NSLog("[core] %@", msg) }
            else { NSLog("[core] %@: %@", rcStatusString(code), msg) }
        }
        renderer?.onSizeChanged = { [weak self] w, h in
            DispatchQueue.main.async { self?.resizeToVideo(w, h) }
        }

        let t = Thread { [weak self] in
            let r = client.start()
            if r != RC_OK, let self = self, !self.torn {
                NSLog("[core] start thất bại: %@", rcStatusString(r))
            }
        }
        t.stackSize = 1 << 20
        startThread = t
        t.start()

        if config.showFps {
            let timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
                self?.updateTitle()
            }
            fpsTimer = timer
        }
    }

    /// Chỉnh cửa sổ khớp video: 1:1 nếu vừa, quá to thì scale ~90% màn hình, giữ tỉ lệ.
    private func resizeToVideo(_ vw: Int, _ vh: Int) {
        guard let window = window, vw > 0, vh > 0 else { return }
        let barH: CGFloat = config.control ? 44 : 0
        var maxW = CGFloat(vw), maxH = CGFloat(vh)
        if let screen = window.screen ?? NSScreen.main {
            let vf = screen.visibleFrame
            maxW = vf.width * 0.9
            maxH = vf.height * 0.9 - barH
        }
        var scale = 1.0
        if CGFloat(vw) * scale > maxW { scale = maxW / CGFloat(vw) }
        if CGFloat(vh) * scale > maxH { scale = maxH / CGFloat(vh) }
        let w = CGFloat(vw) * scale
        let h = CGFloat(vh) * scale + barH
        var frame = window.frame
        let dtop = frame.origin.y + frame.size.height // giữ mép trên cố định
        frame.size = window.frameRect(forContentRect: NSRect(x: 0, y: 0, width: w, height: h)).size
        frame.origin.y = dtop - frame.size.height
        window.setFrame(frame, display: true, animate: false)
    }

    private func updateTitle() {
        guard let window = window, let client = client else { return }
        if !titleResolved, let t = client.transportDesc {
            titleSuffix = t
            titleResolved = true
        }
        let fps = frameCounter.exchange(0)
        var parts = ["RigControlNative — \(tag)"]
        parts.append("(\(titleResolved ? titleSuffix : "đang kết nối…"))")
        if config.showFps {
            parts.append("· \(fps) FPS")
            if let d = client.decoderDesc { parts.append("· \(d)") }
        }
        window.title = parts.joined(separator: " ")
    }

    func teardown() {
        if torn { return }
        torn = true
        fpsTimer?.invalidate(); fpsTimer = nil
        client?.onFrame = nil
        client?.abort()
        // Join thread deploy trước khi destroy để tránh use-after-free.
        if let t = startThread { while !t.isFinished { usleep(5_000) } }
        startThread = nil
        client?.stop()
        client?.destroy()
        client = nil
    }

    deinit { teardown() }

    // ---- Input tiện dụng gọi từ view / navbar ----
    func sendDeviceAction(_ a: rc_device_action) { client?.sendDeviceAction(a) }
    func clickButton(_ code: UInt32) { client?.clickButton(code) }
}

/// Bộ đếm nguyên tử nhỏ cho FPS (frame callback tăng trên thread core, timer đọc trên main).
final class Atomic {
    private var value: Int32
    private let lock = NSLock()
    init(_ v: Int32) { value = v }
    func increment() { lock.lock(); value += 1; lock.unlock() }
    func exchange(_ v: Int32) -> Int32 {
        lock.lock(); defer { lock.unlock() }
        let old = value; value = v; return old
    }
}
