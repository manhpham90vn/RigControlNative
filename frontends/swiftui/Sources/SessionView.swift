/*
 * SessionView.swift — nội dung cửa sổ một phiên: thanh nút điều khiển thiết bị (nếu bật control)
 * + vùng video Metal. Tái hiện navbar của input.c.
 */
import RCCore
import SwiftUI

struct SessionView: View {
    @ObservedObject var model: SessionModel

    var body: some View {
        VStack(spacing: 0) {
            if model.config.control {
                NavBar(model: model)
            }
            MetalVideoView(model: model)
                .background(Color.black)
        }
        .background(Color.black)
    }
}

private struct NavBar: View {
    let model: SessionModel

    var body: some View {
        HStack(spacing: 6) {
            key("◀", "Back", RC_AKEYCODE_BACK)
            key("●", "Home", RC_AKEYCODE_HOME)
            key("▢", "Ứng dụng gần đây", RC_AKEYCODE_APP_SWITCH)
            key("☰", "Menu", RC_AKEYCODE_MENU)
            key("⏻", "Power", RC_AKEYCODE_POWER)
            key("🔉", "Giảm âm lượng", RC_AKEYCODE_VOLUME_DOWN)
            key("🔊", "Tăng âm lượng", RC_AKEYCODE_VOLUME_UP)
            Divider().frame(height: 20)
            dev("⟳", "Xoay màn hình", RC_DEVICE_ROTATE)
            dev("🔔", "Mở thanh thông báo", RC_DEVICE_EXPAND_NOTIF)
            dev("🌙", "Tắt màn hình thiết bị (vẫn mirror)", RC_DEVICE_SCREEN_OFF)
            dev("☀", "Bật lại màn hình thiết bị", RC_DEVICE_SCREEN_ON)
            Spacer()
        }
        .padding(6)
        .buttonStyle(.bordered)
    }

    private func key(_ label: String, _ tip: String, _ code: Int32) -> some View {
        Button(label) { model.clickButton(UInt32(code)) }.help(tip)
    }
    private func dev(_ label: String, _ tip: String, _ action: rc_device_action) -> some View {
        Button(label) { model.sendDeviceAction(action) }.help(tip)
    }
}
