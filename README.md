# RigControlNative

Ứng dụng **screen mirroring độ trễ cực thấp** để chơi game, mirror màn hình **Android** sang
desktop (giống [scrcpy](https://github.com/Genymobile/scrcpy)) nhưng với **GUI native trên
từng nền tảng**: Ubuntu/GTK, Windows/WinUI 3 (C++), macOS/SwiftUI.

Phần **core viết bằng C thuần** (`libcore`) được **dùng chung cho cả 3 front-end** — chỉ phần
render + input + hardware-decode backend là viết riêng cho mỗi nền tảng.

## Đặc điểm

- Mirror màn hình Android, độ trễ thấp (ưu tiên tối ưu cho gaming).
- Transport: **USB** (qua adb, độ trễ thấp nhất) + **LAN/Wi-Fi** nội bộ.
- Điều khiển ngược bằng **chuột + bàn phím** từ desktop.
- Codec **H.264** (hardware decode có sẵn mọi nền tảng); H.265/AV1 để phase sau.
- Core C dùng chung, tái sử dụng cho mọi client.

## Kiến trúc

```
┌─────────────────────┐        video socket (H.264 stream)      ┌──────────────────────────┐
│   Android device    │  ───────────────────────────────────▶  │   Desktop (Ubuntu MVP)   │
│                     │                                          │                          │
│  rc-server (Java)   │        control socket (input events)     │  libcore (C)             │
│  - SurfaceControl   │  ◀───────────────────────────────────   │   adb/net → demux →      │
│  - VirtualDisplay   │                                          │   FFmpeg decode → API    │
│  - MediaCodec H.264 │        (adb forward/reverse tunnel qua   │       │                  │
│  - InputManager     │         USB, hoặc TCP trực tiếp qua LAN) │       ▼                  │
│    inject events    │                                          │  GTK4 front-end          │
└─────────────────────┘                                          │  GL render + input       │
                                                                 └──────────────────────────┘
```

Ba tầng, ranh giới rõ ràng qua **C API**:

1. **rc-server** (Android, Java): capture màn hình + encode H.264 + nhận input inject.
2. **libcore** (C, shared lib): quản lý thiết bị, demux, decode, protocol; phơi ra C API thuần.
3. **Front-end native** (GTK trước): chỉ lo render frame + bắt input, gọi vào libcore.

## Cấu trúc repo

```
RigControlNative/
  CMakeLists.txt                # top-level: build core + gtk frontend
  README.md
  docs/PROTOCOL.md              # đặc tả protocol video + control (nguồn sự thật)
  server/                       # Android server (Java, build bằng Gradle + android.jar)
    src/main/java/com/rigcontrol/server/
      Server.java               # entry qua app_process, mở socket, điều phối
      ScreenEncoder.java        # SurfaceControl display + VirtualDisplay + MediaCodec H.264
      ControlReceiver.java      # đọc control socket, inject qua InputManager (reflection)
      DesktopConnection.java    # quản lý video/control socket + tunnel
      wrappers/                 # reflection wrapper cho hidden API
  core/
    include/rc/rc_client.h      # C API công khai
    src/
      client.c                  # orchestrator + impl C API, quản lý thread
      adb.c                     # spawn adb: push server, forward/reverse, tcpip
      net.c                     # socket TCP, tunnel
      server_deploy.c           # push + chạy rc-server qua app_process
      demuxer.c                 # tách khung gói: [PTS:8][len:4][payload]
      decoder.c                 # FFmpeg libavcodec H.264 (+ hwaccel VAAPI sau)
      control_msg.c             # serialize mouse/keyboard → control protocol
  frontends/
    gtk/                        # MVP (GTK4 + GtkGLArea)
    winui/                      # phase sau (C++/WinUI 3, SwapChainPanel + D3D11)
    swiftui/                    # phase sau (Swift/SwiftUI, MTKView + Metal)
```

## C API (bản phác — `core/include/rc/rc_client.h`)

```c
typedef struct rc_client rc_client;

typedef struct {
    const char *serial;        // adb serial, NULL = thiết bị mặc định
    enum { RC_TRANSPORT_USB, RC_TRANSPORT_TCP } transport;
    const char *tcp_addr;      // dùng khi TCP (LAN)
    int   max_size;            // giới hạn cạnh dài (0 = full)
    int   bit_rate;            // bps
    int   max_fps;
    enum { RC_CODEC_H264 } codec;
} rc_config;

typedef struct {
    int width, height;
    int format;                // vd NV12/I420
    uint8_t *data[4];
    int linesize[4];
    int64_t pts_us;
} rc_frame;

typedef void (*rc_frame_cb)(const rc_frame *frame, void *user);
typedef void (*rc_status_cb)(int code, const char *msg, void *user);

rc_client *rc_client_create(const rc_config *cfg);
int  rc_client_start(rc_client *c);
void rc_client_set_frame_callback(rc_client *c, rc_frame_cb cb, void *user);
void rc_client_set_status_callback(rc_client *c, rc_status_cb cb, void *user);

int  rc_client_send_mouse(rc_client *c, int action, int buttons, float x, float y);
int  rc_client_send_scroll(rc_client *c, float x, float y, float hscroll, float vscroll);
int  rc_client_send_key(rc_client *c, int action, int keycode, int metastate);
int  rc_client_send_text(rc_client *c, const char *utf8);

void rc_client_stop(rc_client *c);
void rc_client_destroy(rc_client *c);
```

**Threading**: 1 thread nhận network + demux, 1 thread decode; frame đẩy về UI thread qua
callback (GTK marshal bằng `g_idle_add`).

## Protocol (chi tiết trong `docs/PROTOCOL.md`)

- **Video socket**: 1 byte codec-id → (tùy chọn) config packet (SPS/PPS) → các frame
  `[PTS: int64 BE][len: uint32 BE][payload H.264]`. Không reorder, không B-frame.
- **Control socket**: message nhỏ gọn, byte đầu là type — `MOUSE`, `SCROLL`, `KEY`, `TEXT`.
- USB: hai socket qua `adb reverse/forward` localabstract. LAN: server listen TCP, client connect.

## Tối ưu độ trễ thấp

- **Encoder (Android)**: tắt B-frame, bitrate-mode realtime, I-frame interval lớn (chỉ IDR khi
  cần), priority realtime.
- **Decoder (FFmpeg)**: `AV_CODEC_FLAG_LOW_DELAY`, không frame reordering, render ngay khi decode xong.
- **USB**: stream thẳng, không jitter buffer. **LAN**: TCP cho MVP.
- **Render**: MVP upload CPU frame (NV12) lên GL texture + shader YUV→RGB; phase sau zero-copy
  VAAPI → dmabuf → EGL.

## Lộ trình (milestones)

| Phase | Nội dung |
|-------|----------|
| 0 | Scaffolding: repo, CMake, docs, xác định deps |
| 1 | Android `rc-server` (video-only): capture + encode + socket |
| 2 | `libcore`: adb/deploy + demux + FFmpeg decode + C API |
| 3 | GTK front-end: render frame → mirror chạy end-to-end |
| 4 | Control: chuột + bàn phím (inject qua InputManager) |
| 5 | LAN transport + hardware decode (VAAPI zero-copy) |
| 6 | Port Windows (WinUI 3) & macOS (SwiftUI), dùng chung `libcore` |

## Build & chạy (MVP — Ubuntu/GTK)

Yêu cầu: `libavcodec-dev libavutil-dev`, GTK4 + EGL/GL, `adb` (platform-tools), Android
SDK/NDK + `android.jar` (để build server). Thiết bị Android bật USB debugging.

```bash
# 1. Build Android server
cd server && ./gradlew   # → rc-server (dex)

# 2. Build core + GTK app
cmake -B build && cmake --build build

# 3. Cắm Android qua USB rồi chạy app GTK
adb devices               # xác nhận thiết bị
./build/frontends/gtk/rigcontrol
```

### Kiểm thử

1. **Video**: màn hình Android hiển thị trong cửa sổ, mượt.
2. **Độ trễ**: mở đồng hồ mili-giây trên điện thoại, chụp chung để đo lệch (mục tiêu vài chục ms qua USB).
3. **Input**: rê chuột/click và gõ phím trên cửa sổ → thao tác xảy ra trên thiết bị.

## Trạng thái

🚧 Đang phát triển — Phase 0 (scaffolding).

## License

Apache-2.0 (dự kiến).
