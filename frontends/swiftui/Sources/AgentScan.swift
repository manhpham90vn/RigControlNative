/*
 * AgentScan.swift — client cổng discovery của rc-agent: mở TCP tới ip:port, KHÔNG gửi gì, đọc tới
 * EOF, parse theo docs/AGENT_PROTOCOL.md §1.2 thành danh sách AgentDev. Tái hiện agent.c của GTK.
 */
import Darwin
import Foundation

let RC_AGENT_PROTOCOL_VERSION = 1
let DEFAULT_DISCOVERY_PORT = 8888

struct AgentDev: Hashable {
    var host: String      // IP máy agent
    var serial: String    // "<host>:<adb_port>" — dùng cho mọi thao tác adb
    var model: String
    var kind: String      // "emulator" | "USB" | "wireless"
    var adbPort: Int
    var streamBase: Int   // cổng public đầu dải stream; 0 = không relay stream (đi adb tunnel)
}

enum AgentScanError: Error, LocalizedError {
    case connect(String, Int)
    case noReply(String, Int)
    case badBanner(String, Int)
    case version(Int)

    var errorDescription: String? {
        switch self {
        case let .connect(ip, port):
            return "Không kết nối được \(ip):\(port) (agent chưa chạy? sai IP/cổng?)"
        case let .noReply(ip, port):
            return "\(ip):\(port) không trả lời (cổng của dịch vụ khác?)"
        case let .badBanner(ip, port):
            return "\(ip):\(port) không phải rc-agent (banner sai) — kiểm tra lại cổng."
        case let .version(v):
            return "Agent phiên bản \(v), app chỉ hiểu \(RC_AGENT_PROTOCOL_VERSION) — cập nhật cho khớp."
        }
    }
}

enum AgentScan {
    /// Connect non-blocking có timeout; trả fd hoặc -1.
    private static func connectTimeout(_ ip: String, _ port: Int, _ timeoutMs: Int32) -> Int32 {
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        var res: UnsafeMutablePointer<addrinfo>?
        if getaddrinfo(ip, String(port), &hints, &res) != 0 { return -1 }
        defer { freeaddrinfo(res) }

        var ai = res
        while let a = ai {
            let fd = socket(a.pointee.ai_family, a.pointee.ai_socktype, a.pointee.ai_protocol)
            if fd >= 0 {
                let flags = fcntl(fd, F_GETFL, 0)
                _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
                var ok = false
                if connect(fd, a.pointee.ai_addr, a.pointee.ai_addrlen) == 0 {
                    ok = true
                } else if errno == EINPROGRESS {
                    var pfd = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
                    if poll(&pfd, 1, timeoutMs) > 0 {
                        var err: Int32 = 0
                        var sl = socklen_t(MemoryLayout<Int32>.size)
                        if getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &sl) == 0 && err == 0 { ok = true }
                    }
                }
                if ok {
                    _ = fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)
                    return fd
                }
                close(fd)
            }
            ai = a.pointee.ai_next
        }
        return -1
    }

    /// Đọc tới EOF với hạn tổng (agent ghi một phát rồi đóng).
    private static func readToEOF(_ fd: Int32, _ timeoutMs: Int) -> Data? {
        var out = Data()
        let deadline = Date().addingTimeInterval(Double(timeoutMs) / 1000.0)
        var buf = [UInt8](repeating: 0, count: 8192)
        while out.count < 1 << 20 {
            let remain = Int32((deadline.timeIntervalSinceNow) * 1000)
            if remain <= 0 { return out.isEmpty ? nil : out }
            var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            let r = poll(&pfd, 1, remain)
            if r < 0 { if errno == EINTR { continue }; return nil }
            if r == 0 { return out.isEmpty ? nil : out }
            let n = buf.withUnsafeMutableBytes { read(fd, $0.baseAddress, $0.count) }
            if n < 0 { if errno == EINTR { continue }; return nil }
            if n == 0 { break } // EOF
            out.append(contentsOf: buf[0..<n])
        }
        return out
    }

    private static func parseDevLine(_ ip: String, _ line: String) -> AgentDev? {
        let f = line.components(separatedBy: "\t")
        guard f.count >= 5, let adbPort = Int(f[0]), adbPort > 0 else { return nil }
        return AgentDev(host: ip, serial: "\(ip):\(adbPort)", model: f[3], kind: f[1],
                        adbPort: adbPort, streamBase: Int(f[4]) ?? 0)
    }

    /// Quét một agent; ném AgentScanError nếu banner/version sai hoặc không nối được.
    static func scan(ip: String, port: Int) throws -> [AgentDev] {
        let fd = connectTimeout(ip, port, 2000)
        if fd < 0 { throw AgentScanError.connect(ip, port) }
        defer { close(fd) }
        guard let data = readToEOF(fd, 3000), !data.isEmpty,
              let text = String(data: data, encoding: .utf8) else {
            throw AgentScanError.noReply(ip, port)
        }
        var lines = text.components(separatedBy: "\n")
        guard let banner = lines.first, banner.hasPrefix("RCAGENT ") else {
            throw AgentScanError.badBanner(ip, port)
        }
        let version = Int(banner.dropFirst(8).prefix(while: { $0.isNumber })) ?? -1
        guard version == RC_AGENT_PROTOCOL_VERSION else { throw AgentScanError.version(version) }

        lines.removeFirst()
        return lines.compactMap { $0.isEmpty ? nil : parseDevLine(ip, $0) }
    }
}
