# RigControlNative

Ứng dụng **screen mirroring độ trễ cực thấp** để chơi game, mirror màn hình **Android** sang
desktop (giống [scrcpy](https://github.com/Genymobile/scrcpy)) nhưng với **GUI native trên
từng nền tảng**: Ubuntu/GTK, Windows/WinUI 3 (C++), macOS/SwiftUI.

Phần **core viết bằng C thuần** (`libcore`) được **dùng chung cho cả 3 front-end** — chỉ phần
render + input + hardware-decode backend là viết riêng cho mỗi nền tảng.

## Đặc điểm

- Mirror màn hình Android, độ trễ thấp (ưu tiên tối ưu cho gaming).
- **Stream audio** thiết bị về desktop (Opus, giải mã bằng FFmpeg, phát qua miniaudio).
- Transport: **USB** (qua adb, độ trễ thấp nhất) + **LAN/Wi-Fi** nội bộ.
- Điều khiển ngược bằng **chuột + bàn phím** từ desktop.
- **Nút điều khiển thiết bị**: BACK / HOME / APP_SWITCH / MENU / POWER / VOLUME, cùng các
  hành động đặc biệt (tắt–bật màn hình thiết bị, mở notification/settings panel, **xoay màn hình**).
- **Tự theo dõi xoay màn hình** thiết bị (khung hình tự thích ứng theo độ phân giải mới).
- Codec video **H.264** (hardware decode có sẵn mọi nền tảng); H.265/AV1 để phase sau.
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

- Tối đa **ba socket** mở theo thứ tự cố định: **video → [audio] → [control]**.
- **Video**: `device_meta` → (tùy chọn) config packet (SPS/PPS) → các frame
  `[pts_flags: u64][len: u32][payload H.264]`. Không reorder, không B-frame. Khi thiết bị xoay,
  server gửi config+keyframe mới với kích thước mới (client tự thích ứng theo từng frame).
- **Audio**: `audio_meta` (codec/sample_rate/channels; codec=0 nếu không khả dụng) → packet cùng
  khung với video (Opus mặc định).
- **Control**: message byte đầu là type — `MOUSE_MOTION/BUTTON`, `SCROLL`, `KEY` (gồm cả nút
  BACK/HOME/POWER/VOLUME), `TEXT`, `DEVICE_ACTION` (screen off/on, panel, rotate).
- USB: các socket qua `adb reverse` localabstract. LAN: server listen TCP, client connect.

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
| 0 | Scaffolding: repo, CMake, docs, protocol, xác định deps |
| 1 | Android `rc-server`: capture + encode **video (H.264) + audio (Opus)** + socket (USB/TCP) |
| 2 | `libcore`: adb/deploy + demux + FFmpeg decode + C API |
| 3 | GTK front-end: render frame → mirror chạy end-to-end |
| 4 | Control: chuột + bàn phím + nút thiết bị + DEVICE_ACTION (rotate...) |
| 5 | Audio phía desktop: decode Opus (FFmpeg) + phát miniaudio (core) |
| 6 | LAN transport + hardware decode (VAAPI zero-copy) |
| 7 | Port Windows (WinUI 3) & macOS (SwiftUI), dùng chung `libcore` |

## Build & chạy (MVP — Ubuntu/GTK)

Yêu cầu: `libavcodec-dev libavutil-dev libswresample-dev libasound2-dev`, `libgtk-4-dev libepoxy-dev` + EGL/GL, `adb` (platform-tools), JDK 11+,
Android SDK (`platforms/android-*/android.jar` + `build-tools/*/d8`) để build server. **Không cần
NDK** (server là Java thuần). Thiết bị Android bật USB debugging.

Cách nhanh nhất là dùng **Makefile** (bao quanh CMake + `build.sh` + clang tools):

```bash
make deps      # in gợi ý cài dependency
make all       # build rc-server (dex) + libcore + app GTK
make run       # chạy app GTK
make format    # định dạng C + Java (clang-format)
make lint      # clang-tidy (fallback build -Werror nếu chưa cài)
make clean     # xoá build
make help      # xem toàn bộ target
```

Hoặc thủ công:

```bash
# 1. Build Android server (javac + d8, không cần Gradle)
cd server && ./build.sh   # → server/rc-server (jar chứa classes.dex)

# 2. Build core + GTK app
cmake -B build && cmake --build build

# 3. Cắm Android qua USB rồi chạy app GTK
adb devices               # xác nhận thiết bị
./build/frontends/gtk/rigcontrol
```

### Định dạng & lint

- `.clang-format` áp cho cả C (LLVM, 4-space) và Java (Google, 4-space).
- `.clang-tidy` cho C của libcore (cần `compile_commands.json` — CMake tự sinh).
- Cài công cụ: `sudo apt install clang-format clang-tidy`.

### Kiểm thử

1. **Video**: màn hình Android hiển thị trong cửa sổ, mượt.
2. **Độ trễ**: mở đồng hồ mili-giây trên điện thoại, chụp chung để đo lệch (mục tiêu vài chục ms qua USB).
3. **Input**: rê chuột/click và gõ phím trên cửa sổ → thao tác xảy ra trên thiết bị.

## Trạng thái

🚧 Đang phát triển — **Phase 5**: mirror video + **audio** + điều khiển đầy đủ qua cửa sổ GTK.

- **Phase 1** — Android `rc-server`: capture + encode video (H.264) và audio (Opus), mở socket
  USB (localabstract) / TCP (LAN), gửi `device_meta`/`audio_meta`. Audio dùng `REMOTE_SUBMIX`
  (cần Android 11+); thiết bị không hỗ trợ tự báo `ACODEC_ID_NONE`, video vẫn chạy.
- **Phase 2** — `libcore`: `rc_client_start` tự `adb push` + reverse tunnel + chạy server rồi
  demux → **FFmpeg H.264 decode** → giao `rc_frame` (I420) qua callback. Audio hiện chỉ *drain*
  để không nghẽn server; decode/phát ở Phase 5.
- **Phase 3** — front-end GTK4: `GtkGLArea` + shader YUV(I420)→RGB, frame marshal về UI thread,
  letterbox giữ tỉ lệ. Đã kiểm chứng render pixel thật từ thiết bị. **Bộ chọn thiết bị trên UI**:
  nhiều máy → hiện danh sách (model + serial) để chọn, một máy → tự kết nối; có nút *Làm mới*.
  Cấu hình qua biến môi trường (`RC_SERIAL`, `RC_TCP_ADDR`, `RC_MAX_SIZE`, `RC_BIT_RATE`,
  `RC_MAX_FPS`, `RC_AUDIO`, `RC_CONTROL`, `RC_SERVER_PATH`); đặt `RC_SERIAL`/`RC_TCP_ADDR` sẽ
  bỏ qua bộ chọn và kết nối thẳng.

- **Phase 4** — điều khiển ngược: server inject qua `InputManager`/`InputManagerGlobal`
  (reflection, Android 14+). Chuột → cảm ứng (nhấn/kéo/thả), cuộn, bàn phím (phím thường → TEXT,
  phím điều hướng/sửa → KEY), nút thiết bị **BACK/HOME/RECENT/POWER/VOLUME** trên thanh công cụ,
  và **DEVICE_ACTION** (xoay qua `IWindowManager.freezeRotation`, screen off/on, panel thông báo).
  Đã kiểm chứng end-to-end: HOME đổi foreground, vuốt mở app drawer, ROTATE đổi `user_rotation`.
- **Phase 5** — audio playback ở desktop: `libcore` giải mã **Opus** (FFmpeg) → `libswresample`
  (S16 interleaved) → phát qua **ALSA** (blocking, tự pace real-time). OpusHead tự tổng hợp từ
  `audio_meta`. Thiết bị không có audio / không mở được ALSA → tự bỏ qua, video vẫn chạy. Đã
  kiểm chứng phát đúng 48000 mẫu/s ra sound card song song với video.

> Backend audio hiện dùng **ALSA** (Ubuntu MVP); chuyển sang **miniaudio** (cross-platform) khi
> port Windows/macOS — `rc_audio` đã trừu tượng hoá sẵn.

Còn lại: LAN transport + hardware decode zero-copy (Phase 6), port Windows/macOS (Phase 7).

## License

Apache-2.0 (dự kiến).
