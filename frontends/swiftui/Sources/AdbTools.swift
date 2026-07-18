/*
 * AdbTools.swift — bọc lệnh adb ở phía chooser (liệt kê thiết bị, adb connect wireless/agent).
 * Core tự lo push + tunnel khi mở phiên; ở đây chỉ cần biết "thiết bị nào đang có + connect được".
 */
import Foundation

struct AdbDevice: Identifiable, Hashable {
    let serial: String
    let model: String
    var id: String { serial }
}

enum AdbTools {
    /// Đường dẫn adb: ưu tiên PATH; fallback vị trí Homebrew hay gặp.
    static let adbPath: String = {
        for p in ["/opt/homebrew/bin/adb", "/usr/local/bin/adb", "/usr/bin/adb"]
        where FileManager.default.isExecutableFile(atPath: p) { return p }
        return "adb" // để launch dựa vào PATH (env qua /usr/bin/env)
    }()

    /// Chạy adb đồng bộ, trả (exitCode, stdout). timeout giây > 0 → kill nếu quá hạn
    /// (`adb connect` tới IP chết treo theo TCP SYN retry). Trả nil nếu spawn lỗi.
    @discardableResult
    static func run(_ args: [String], timeout: TimeInterval = 0) -> (Int32, String)? {
        let proc = Process()
        if adbPath.hasPrefix("/") {
            proc.executableURL = URL(fileURLWithPath: adbPath)
            proc.arguments = args
        } else {
            proc.executableURL = URL(fileURLWithPath: "/usr/bin/env")
            proc.arguments = ["adb"] + args
        }
        let outPipe = Pipe()
        proc.standardOutput = outPipe
        proc.standardError = Pipe()
        do { try proc.run() } catch { return nil }

        if timeout > 0 {
            let deadline = Date().addingTimeInterval(timeout)
            while proc.isRunning && Date() < deadline { usleep(20_000) }
            if proc.isRunning { proc.terminate() }
        }
        let data = outPipe.fileHandleForReading.readDataToEndOfFile()
        proc.waitUntilExit()
        return (proc.terminationStatus, String(data: data, encoding: .utf8) ?? "")
    }

    /// `adb devices -l` → danh sách thiết bị trạng thái "device".
    static func listDevices() -> [AdbDevice] {
        guard let (_, out) = run(["devices", "-l"], timeout: 8) else { return [] }
        var devs: [AdbDevice] = []
        for (i, line) in out.split(separator: "\n", omittingEmptySubsequences: true).enumerated() {
            if i == 0 { continue } // "List of devices attached"
            let cols = line.split(whereSeparator: { $0 == " " || $0 == "\t" })
            guard cols.count >= 2, cols[1] == "device" else { continue }
            let serial = String(cols[0])
            var model = ""
            if let m = line.range(of: "model:") {
                let rest = line[m.upperBound...]
                model = String(rest.prefix(while: { $0 != " " && $0 != "\t" }))
            }
            devs.append(AdbDevice(serial: serial, model: model))
        }
        return devs
    }

    /// adb connect + xác minh get-state (adb connect exit 0 kể cả khi thất bại). TRUE nếu online.
    static func connectAndVerify(_ serial: String) -> Bool {
        _ = run(["connect", serial], timeout: 10)
        guard let (code, _) = run(["-s", serial, "get-state"], timeout: 5) else { return false }
        return code == 0
    }
}
