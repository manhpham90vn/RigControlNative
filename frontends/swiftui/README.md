# Front-end macOS (SwiftUI + Metal)

Client macOS của RigControlNative: **Swift/SwiftUI + AppKit**, render bằng **Metal (MTKView)**,
dùng chung `libcore` (C) như front-end GTK. Ngang tính năng với GTK.

## Build & chạy

Xem mục *Build & chạy (macOS)* ở [README gốc](../../README.md). Tóm tắt:

```bash
brew install ffmpeg cmake            # + adb (brew install --cask android-platform-tools)
xcode-select --install               # swiftc + SDK macOS (KHÔNG cần Xcode đầy đủ)
./server/build.sh                    # dex server (JDK + Android SDK)
make mac                             # → build-mac/RigControlNative.app
open build-mac/RigControlNative.app
```

`build-mac.sh` dựng `libcore` qua CMake rồi biên dịch `Sources/*.swift` bằng `swiftc` (SwiftUI và
Metal có sẵn trong SDK của Command Line Tools) và đóng gói `.app` — không phụ thuộc Xcode/xcodebuild.

## Cấu trúc

| File | Vai trò | Tương ứng GTK |
|------|---------|---------------|
| `bridge/module.modulemap`, `bridge/shim.h` | Phơi C API libcore thành module `RCCore` cho Swift | — |
| `Sources/main.swift` | Entry AppKit + menu; env → mở thẳng hoặc chooser | `main.c` |
| `Sources/RCClient.swift` | Wrapper Swift quanh `rc_client` + callback | (C API) |
| `Sources/EnvConfig.swift` | Đọc biến môi trường `RC_*` | `main.c` |
| `Sources/ChooserView.swift` | Chọn thiết bị, dropdown, quét agent | `chooser.c` |
| `Sources/AdbTools.swift` | `adb devices` / `adb connect` | `chooser.c` |
| `Sources/AgentScan.swift` | Client discovery rc-agent (TCP) | `agent.c` |
| `Sources/SessionManager.swift` | Cửa sổ phiên + cấp cổng LAN đa phiên | `session.c` |
| `Sources/SessionModel.swift` | Vòng đời phiên, resize, tiêu đề, FPS | `session.c` |
| `Sources/SessionView.swift` | Layout cửa sổ + navbar thiết bị | `session.c` + `input.c` |
| `Sources/MetalVideoView.swift` | MTKView + chuột/bàn phím | `input.c` |
| `Sources/VideoRenderer.swift` | Metal YUV→RGB (I420/NV12), letterbox | `render.c` |
| `Sources/AndroidKeys.swift` | NSEvent → Android keycode/metastate | `input.c` |

## Ghi chú kỹ thuật

- **Audio**: libcore phát qua **miniaudio** (Core Audio). Không còn phụ thuộc ALSA.
- **Decode**: hiện **software** (đường low-delay của core). Hardware decode **VideoToolbox** để sau —
  CUDA/VAAPI của core tự bị bỏ qua trên mac.
- **Threading**: frame tới trên thread nội bộ của core → copy vào `FrameStore` (có khóa) → marshal
  về main thread bằng `DispatchQueue.main.async` + `MTKView.setNeedsDisplay` (giống `g_idle_add`).
- **Toạ độ**: chuột ánh xạ theo letterbox sang **pixel VIDEO**; core tự scale về pixel thiết bị.
- **Tiếng Việt/ký tự có dấu**: cùng hạn chế như GTK (server inject TEXT qua `KeyCharacterMap`).
