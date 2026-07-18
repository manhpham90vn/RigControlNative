/*
 * SessionManager.swift — mở/đóng cửa sổ phiên (đa session), và cấp cổng stream LAN theo thiết bị
 * giống session.c: mỗi phiên LAN khiến server bind cổng riêng trên thiết bị nên phải cấp cổng
 * khác nhau để mở nhiều phiên cùng máy không dính BindException.
 */
import AppKit
import SwiftUI

private let RC_LAN_BASE_PORT = 27183
private let STREAM_COUNT = 4 // số phiên LAN trực tiếp đồng thời mỗi thiết bị (khớp rc-agent)

final class SessionManager {
    static let shared = SessionManager()
    private var controllers = Set<SessionWindowController>()

    /// Cổng LAN trống thấp nhất chưa bị phiên đang sống nào chiếm (wireless trực tiếp).
    private func allocLanPort() -> Int {
        var port = RC_LAN_BASE_PORT
        while controllers.contains(where: { !$0.model.torn && $0.model.lanPort == port }) { port += 1 }
        return port
    }

    /// k stream trống thấp nhất (0..<STREAM_COUNT) chưa bị phiên đang sống CỦA CÙNG serial chiếm;
    /// -1 = hết → đi adb tunnel.
    private func allocStreamK(_ serial: String) -> Int {
        for k in 0..<STREAM_COUNT {
            let used = controllers.contains {
                !$0.model.torn && $0.model.streamK == k && $0.model.serialForAlloc == serial
            }
            if !used { return k }
        }
        return -1
    }

    /// Mở một phiên. `agentDev` != nil → thiết bị qua rc-agent (dùng dải stream của agent).
    func open(base: SessionConfig, serial: String?, agentDev: AgentDev?) {
        var cfg = base
        cfg.serial = serial
        var lanPort = 0
        var streamK = -1

        if let ad = agentDev, ad.streamBase > 0, let serial = serial {
            // Qua rc-agent: cổng public = stream_base + k; server listen RC_LAN_BASE_PORT + k trong
            // thiết bị; agent forward giữa hai cổng (AGENT_PROTOCOL §2.2).
            let k = allocStreamK(serial)
            if k >= 0 {
                streamK = k
                cfg.tcpAddr = "\(ad.host):\(ad.streamBase + k)"
                cfg.tcpDevicePort = Int32(RC_LAN_BASE_PORT + k)
            } else {
                cfg.tcpAddr = nil // hết dải → adb tunnel
            }
        } else if agentDev != nil {
            cfg.tcpAddr = nil // agent không relay stream → adb tunnel thẳng
        } else if let serial = serial, let host = wirelessHost(serial) {
            // Wireless trực tiếp → cấp cổng LAN toàn cục, connect == listen.
            lanPort = allocLanPort()
            cfg.tcpAddr = "\(host):\(lanPort)"
        } else {
            cfg.tcpAddr = nil // USB / máy ảo cục bộ
        }

        let tag = serial ?? cfg.tcpAddr ?? "default"
        let model = SessionModel(config: cfg, tag: tag)
        model.lanPort = lanPort
        model.streamK = streamK
        model.serialForAlloc = serial

        let controller = SessionWindowController(model: model)
        controllers.insert(controller)
        controller.onClose = { [weak self, weak controller] in
            if let c = controller { self?.controllers.remove(c) }
        }
        controller.showWindow(nil)
    }

    /// Mở thẳng theo env (RC_TCP_ADDR địa chỉ có sẵn port → tôn trọng, connect == listen).
    func openDirect(base: SessionConfig) {
        let tag = base.serial ?? base.tcpAddr ?? "default"
        let model = SessionModel(config: base, tag: tag)
        model.serialForAlloc = base.serial
        let controller = SessionWindowController(model: model)
        controllers.insert(controller)
        controller.onClose = { [weak self, weak controller] in
            if let c = controller { self?.controllers.remove(c) }
        }
        controller.showWindow(nil)
    }

    private func wirelessHost(_ serial: String) -> String? {
        guard let colon = serial.lastIndex(of: ":") else { return nil }
        let host = String(serial[serial.startIndex..<colon])
        return host.isEmpty ? nil : host
    }
}

/// Một cửa sổ phiên: host SwiftUI SessionView, teardown model khi đóng.
final class SessionWindowController: NSWindowController, NSWindowDelegate {
    let model: SessionModel
    var onClose: (() -> Void)?

    init(model: SessionModel) {
        self.model = model
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 400, height: 720),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        window.title = "RigControlNative — \(model.tag) (đang kết nối…)"
        window.isReleasedWhenClosed = false
        window.center()
        window.contentView = NSHostingView(rootView: SessionView(model: model))
        super.init(window: window)
        window.delegate = self
        model.window = window
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    func windowWillClose(_ notification: Notification) {
        model.teardown()
        onClose?()
    }
}
