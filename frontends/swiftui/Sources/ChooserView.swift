/*
 * ChooserView.swift — màn chọn thiết bị: liệt kê `adb devices`, dropdown kích thước/bitrate, tùy
 * chọn phiên, và ô "Quét agent" (nhập IP máy chạy rc-agent). Bấm một thiết bị → mở một phiên (đa
 * session). Tái hiện chooser.c của front-end GTK.
 */
import SwiftUI

private let SIZE_VALUES: [Int32] = [0, 1920, 1280, 1024, 800]
private let SIZE_LABELS = ["Full (gốc)", "1920", "1280", "1024", "800"]
private let BITRATE_VALUES: [Int32] = [2, 4, 8, 16, 24, 32, 48].map { $0 * 1_000_000 }
private let BITRATE_LABELS = ["2 Mbps", "4 Mbps", "8 Mbps", "16 Mbps", "24 Mbps", "32 Mbps", "48 Mbps"]

final class ChooserModel: ObservableObject {
    @Published var devices: [AdbDevice] = []
    @Published var agentIP = ""
    @Published var agentStatus = ""
    @Published var scanning = false

    @Published var sizeValue: Int32
    @Published var bitrateValue: Int32
    @Published var control: Bool
    @Published var audio: Bool
    @Published var showFps: Bool

    private var agentDevs: [String: AgentDev] = [:]
    private let base: SessionConfig

    init() {
        base = EnvConfig.base()
        sizeValue = base.maxSize
        bitrateValue = base.bitRate
        control = base.control
        audio = base.audio
        showFps = base.showFps
    }

    func refresh() {
        DispatchQueue.global().async {
            let devs = AdbTools.listDevices()
            DispatchQueue.main.async { self.devices = devs }
        }
    }

    func connLabel(_ serial: String) -> String {
        if let ad = agentDevs[serial] {
            switch ad.kind {
            case "emulator": return "Máy ảo (agent)"
            case "USB": return "USB (agent)"
            default: return "LAN (agent)"
            }
        }
        if serial.contains(":") { return "LAN" }
        if serial.hasPrefix("emulator-") { return "Máy ảo" }
        return "USB"
    }

    func scanAgent() {
        var ip = agentIP.trimmingCharacters(in: .whitespaces)
        guard !ip.isEmpty else { return }
        var port = DEFAULT_DISCOVERY_PORT
        if let colon = ip.lastIndex(of: ":") {
            let p = Int(ip[ip.index(after: colon)...]) ?? 0
            if p > 0 { port = p }
            ip = String(ip[ip.startIndex..<colon])
        }
        scanning = true
        agentStatus = "Đang quét agent…"
        DispatchQueue.global().async {
            let result: String
            var ok: [AgentDev] = []
            do {
                let devs = try AgentScan.scan(ip: ip, port: port)
                for d in devs where AdbTools.connectAndVerify(d.serial) { ok.append(d) }
                result = ok.isEmpty
                    ? "Agent \(ip): \(devs.count) thiết bị nhưng không nối được máy nào (kiểm tra adb / mạng)."
                    : "Agent \(ip): nối được \(ok.count)/\(devs.count) thiết bị — bấm để mở."
            } catch {
                result = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
            }
            DispatchQueue.main.async {
                for d in ok { self.agentDevs[d.serial] = d }
                self.agentStatus = result
                self.scanning = false
                self.refresh()
            }
        }
    }

    private func makeConfig() -> SessionConfig {
        var cfg = base
        cfg.maxSize = sizeValue
        cfg.bitRate = bitrateValue
        cfg.control = control
        cfg.audio = audio
        cfg.showFps = showFps
        cfg.serial = nil
        cfg.tcpAddr = nil
        return cfg
    }

    func open(_ device: AdbDevice) {
        SessionManager.shared.open(base: makeConfig(), serial: device.serial,
                                   agentDev: agentDevs[device.serial])
    }
}

struct ChooserView: View {
    @StateObject private var model = ChooserModel()
    @State private var sizeIndex = 0
    @State private var bitrateIndex = 2

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Picker("Kích thước:", selection: $sizeIndex) {
                ForEach(0..<SIZE_LABELS.count, id: \.self) { Text(SIZE_LABELS[$0]) }
            }
            .onChange(of: sizeIndex) { _, i in model.sizeValue = SIZE_VALUES[i] }

            Picker("Bitrate:", selection: $bitrateIndex) {
                ForEach(0..<BITRATE_LABELS.count, id: \.self) { Text(BITRATE_LABELS[$0]) }
            }
            .onChange(of: bitrateIndex) { _, i in model.bitrateValue = BITRATE_VALUES[i] }

            Text("Tùy chọn").bold()
            Toggle("Điều khiển chuột & bàn phím", isOn: $model.control)
                .help("Bỏ tick để chỉ xem — không gửi chuột/bàn phím tới thiết bị")
            Toggle("Phát âm thanh thiết bị", isOn: $model.audio)
                .help("Stream và phát audio của thiết bị (tốn thêm băng thông)")
            Toggle("Hiện FPS trên tiêu đề cửa sổ", isOn: $model.showFps)

            Text("Bấm một thiết bị để mở (mở được nhiều máy cùng lúc).")
                .foregroundStyle(.secondary)

            devicesList

            Button("Làm mới") { model.refresh() }

            agentRow
            if !model.agentStatus.isEmpty {
                Text(model.agentStatus).foregroundStyle(.secondary).font(.callout)
            }
        }
        .padding(18)
        .frame(minWidth: 500, minHeight: 640)
        .onAppear { model.refresh() }
    }

    private var devicesList: some View {
        ScrollView {
            VStack(spacing: 0) {
                if model.devices.isEmpty {
                    Text("Không thấy thiết bị. Cắm máy / bật USB debugging, hoặc quét agent.")
                        .foregroundStyle(.secondary).padding()
                }
                ForEach(model.devices) { dev in
                    Button {
                        model.open(dev)
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            HStack {
                                Text(dev.model.isEmpty ? "(không rõ model)" : dev.model).bold()
                                Text("· \(model.connLabel(dev.serial))").foregroundStyle(.secondary)
                            }
                            Text(dev.serial).font(.caption).foregroundStyle(.secondary)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .padding(.vertical, 8).padding(.horizontal, 12)
                    Divider()
                }
            }
        }
        .frame(maxHeight: .infinity)
        .background(RoundedRectangle(cornerRadius: 6).stroke(.quaternary))
    }

    private var agentRow: some View {
        HStack {
            TextField("100.x.y.z (IP máy agent)", text: $model.agentIP)
                .textFieldStyle(.roundedBorder)
                .onSubmit { model.scanAgent() }
                .help("IP máy đang chạy rc-agent (LAN hoặc Tailscale). App hỏi cổng discovery 8888 "
                    + "rồi liệt kê mọi thiết bị; thêm \":cổng\" nếu agent chạy cổng khác.")
            Button("Quét agent") { model.scanAgent() }.disabled(model.scanning)
        }
    }
}
