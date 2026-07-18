/*
 * MetalVideoView.swift — NSView (MTKView) hiển thị video + bắt chuột/bàn phím, và wrapper
 * NSViewRepresentable để nhúng vào SwiftUI. Chuột → cảm ứng (nhấn/kéo/thả/cuộn), bàn phím →
 * KEY/TEXT. Toạ độ ánh xạ theo letterbox sang pixel VIDEO. Tái hiện input.c của front-end GTK.
 */
import AppKit
import MetalKit
import RCCore
import SwiftUI

final class RCMetalView: MTKView {
    weak var model: SessionModel?
    private var mouseButtons: UInt32 = 0
    private var devX: Float = 0
    private var devY: Float = 0
    private var keysDown = Set<UInt32>()
    private var trackingAreaRef: NSTrackingArea?

    override var isFlipped: Bool { true }          // gốc toạ độ trên-trái, khớp video
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = trackingAreaRef { removeTrackingArea(t) }
        let t = NSTrackingArea(rect: bounds,
                               options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                               owner: self, userInfo: nil)
        addTrackingArea(t)
        trackingAreaRef = t
    }

    /// Điểm (view, points) → pixel VIDEO theo letterbox; nil nếu ngoài vùng video.
    private func toDevice(_ p: NSPoint) -> (Float, Float)? {
        guard let (vw, vh) = model?.renderer?.store.videoSize, vw > 0, vh > 0 else { return nil }
        let ww = Double(bounds.width), wh = Double(bounds.height)
        guard ww > 0, wh > 0 else { return nil }
        let winA = ww / wh, vidA = Double(vw) / Double(vh)
        var vpw = ww, vph = wh
        if winA > vidA { vph = wh; vpw = wh * vidA } else { vpw = ww; vph = ww / vidA }
        let vpx = (ww - vpw) / 2, vpy = (wh - vph) / 2
        let px = Double(p.x), py = Double(p.y)
        if px < vpx || px > vpx + vpw || py < vpy || py > vpy + vph { return nil }
        return (Float((px - vpx) / vpw * Double(vw)), Float((py - vpy) / vph * Double(vh)))
    }

    private func loc(_ event: NSEvent) -> NSPoint { convert(event.locationInWindow, from: nil) }

    // ---- Chuột ----
    private func pressed(_ event: NSEvent, _ button: UInt32) {
        guard let client = model?.client, let (dx, dy) = toDevice(loc(event)) else { return }
        mouseButtons |= button
        devX = dx; devY = dy
        client.sendMouseButton(action: RC_ACTION_DOWN, button: button, buttons: mouseButtons, x: dx, y: dy)
        window?.makeFirstResponder(self)
    }
    private func released(_ event: NSEvent, _ button: UInt32) {
        guard let client = model?.client else { return }
        var dx = devX, dy = devY
        if let d = toDevice(loc(event)) { dx = d.0; dy = d.1 }
        mouseButtons &= ~button
        client.sendMouseButton(action: RC_ACTION_UP, button: button, buttons: mouseButtons, x: dx, y: dy)
    }
    private func moved(_ event: NSEvent) {
        guard let client = model?.client, let (dx, dy) = toDevice(loc(event)) else { return }
        devX = dx; devY = dy
        if mouseButtons != 0 { client.sendMouseMotion(buttons: mouseButtons, x: dx, y: dy) }
    }

    override func mouseDown(with event: NSEvent) { pressed(event, RC_BUTTON_LEFT) }
    override func mouseUp(with event: NSEvent) { released(event, RC_BUTTON_LEFT) }
    override func mouseDragged(with event: NSEvent) { moved(event) }
    override func mouseMoved(with event: NSEvent) { moved(event) }
    override func rightMouseDown(with event: NSEvent) { pressed(event, RC_BUTTON_RIGHT) }
    override func rightMouseUp(with event: NSEvent) { released(event, RC_BUTTON_RIGHT) }
    override func rightMouseDragged(with event: NSEvent) { moved(event) }
    override func otherMouseDown(with event: NSEvent) {
        if event.buttonNumber == 2 { pressed(event, RC_BUTTON_MIDDLE) }
    }
    override func otherMouseUp(with event: NSEvent) {
        if event.buttonNumber == 2 { released(event, RC_BUTTON_MIDDLE) }
    }
    override func otherMouseDragged(with event: NSEvent) { moved(event) }

    override func scrollWheel(with event: NSEvent) {
        guard let client = model?.client else { return }
        // Chuẩn hoá delta (pixel-precise chia nhỏ) rồi đảo dấu như GTK (-dx, -dy).
        let scale = event.hasPreciseScrollingDeltas ? 0.1 : 1.0
        let h = Float(-event.scrollingDeltaX * scale)
        let v = Float(event.scrollingDeltaY * scale)
        client.sendScroll(x: devX, y: devY, h: h, v: v)
    }

    // ---- Bàn phím ----
    override func keyDown(with event: NSEvent) {
        // Để tổ hợp Cmd cho menu/app (Cmd+Q, Cmd+W…), không nuốt.
        if event.modifierFlags.contains(.command) { super.keyDown(with: event); return }
        guard let client = model?.client else { return }
        switch mapKeyDown(event) {
        case let .key(code, meta):
            client.sendKey(action: RC_ACTION_DOWN, keycode: code, meta: meta, repeatCount: 0)
            keysDown.insert(code)
        case let .text(s):
            client.sendText(s)
        case .ignore:
            break
        }
    }

    override func keyUp(with event: NSEvent) {
        if event.modifierFlags.contains(.command) { super.keyUp(with: event); return }
        guard let client = model?.client, let code = mapKeyUpCode(event), keysDown.contains(code)
        else { return }
        client.sendKey(action: RC_ACTION_UP, keycode: code, meta: androidMeta(event.modifierFlags), repeatCount: 0)
        keysDown.remove(code)
    }
}

/// Nhúng RCMetalView vào SwiftUI; dựng renderer, nối model, rồi start phiên.
struct MetalVideoView: NSViewRepresentable {
    let model: SessionModel

    func makeNSView(context: Context) -> RCMetalView {
        let view = RCMetalView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.isPaused = true                // chỉ vẽ khi có frame mới
        view.enableSetNeedsDisplay = true
        view.autoResizeDrawable = true
        view.model = model
        if let renderer = VideoRenderer(mtkView: view) {
            view.delegate = renderer
            model.renderer = renderer
        } else {
            NSLog("[ui] không khởi tạo được VideoRenderer")
        }
        model.metalView = view
        model.startIfNeeded()
        return view
    }

    func updateNSView(_ nsView: RCMetalView, context: Context) {}
}
