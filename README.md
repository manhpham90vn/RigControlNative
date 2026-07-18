# RigControlNative

Ứng dụng **screen mirroring độ trễ cực thấp** cho **Android** → desktop (giống
[scrcpy](https://github.com/Genymobile/scrcpy)) nhưng với **GUI native trên từng nền tảng**:
Ubuntu/GTK (MVP hiện tại), Windows/WinUI 3 và macOS/SwiftUI (phase sau).

Phần **core viết bằng C thuần** (`libcore`) dùng chung cho mọi front-end — chỉ render + input
+ hardware-decode backend là viết riêng cho mỗi nền tảng.

## Đặc điểm

- Mirror màn hình Android độ trễ thấp (tối ưu cho gaming), **H.264 hardware decode**
  (NVDEC / VAAPI, tự fallback software).
- **Stream audio** thiết bị về desktop (Opus, Android 11+).
- Điều khiển ngược bằng **chuột + bàn phím**, kèm thanh nút
  BACK / HOME / RECENT / MENU / POWER / VOLUME / xoay màn hình / tắt–bật màn hình.
- Transport: **USB** (adb tunnel) và **LAN trực tiếp** (TCP, có token bảo vệ) — core tự chọn
  đường tốt nhất và tự fallback.
- **rc-agent**: điều khiển thiết bị cắm ở **máy khác** (LAN hoặc **Tailscale**) — app tự phát
  hiện agent, không phải chép địa chỉ tay.
- Đo hiệu năng ngay trên tiêu đề cửa sổ: **FPS**, trễ pipeline (**pipe**), trễ tích luỹ
  (**lag**), backend decode, đường stream đang dùng.
- Nhiều phiên cùng lúc (nhiều thiết bị, hoặc nhiều cửa sổ cùng một thiết bị).

## Cài đặt (từ bản release)

Tải từ [Releases](https://github.com/manhpham90vn/RigControlNative/releases):

### Desktop Ubuntu (app chính)

Yêu cầu: **Ubuntu 24.04+** (GTK4 ≥ 4.10), `adb`, thiết bị Android 7+ đã bật **USB debugging**
(audio cần Android 11+, control đã kiểm chứng trên Android 14).

```bash
sudo apt install libgtk-4-1 libepoxy0 libasound2t64 adb
tar xzf rigcontrol-v*-linux-x86_64.tar.gz && cd rigcontrol-v*/
./rigcontrol          # chạy từ thư mục giải nén — app tìm server/rc-server cạnh binary
```

FFmpeg đã bundle sẵn trong `lib/` của tar (không phụ thuộc FFmpeg hệ thống — soname
libavcodec đổi theo từng đời distro nên không dùng lib distro được).

> Distro khác / phiên bản thư viện lệch → [build từ source](#build-từ-source) (~5 phút).

### macOS / Linux khác (máy cắm thiết bị, chạy rc-agent)

```bash
brew install --cask android-platform-tools   # adb, nếu chưa có
tar xzf rc-agent-v*-macos-universal.tar.gz && cd rc-agent-v*/
./rc-agent            # giữ chạy liên tục; Ctrl-C dọn adb forward
```

Binary macOS là universal (Apple Silicon + Intel). Trên Linux, `rc-agent` nằm sẵn trong gói
desktop.

## Sử dụng

### Thiết bị cắm USB vào chính máy desktop

Cắm cáp, xác nhận `adb devices` thấy máy, chạy `./rigcontrol` → bấm thiết bị trong danh sách.
Xong.

### Thiết bị qua Wi-Fi (wireless adb)

```bash
make tcpip                     # bật adb TCP trên máy đang cắm USB, in sẵn lệnh connect
make connect IP=192.168.1.50   # hoặc: adb connect 192.168.1.50:5555
```

Thiết bị hiện trong danh sách với nhãn **LAN** — app sẽ thử **LAN trực tiếp** (stream thẳng
TCP, không vòng qua adb) và tự fallback adb tunnel nếu cổng không thông.

### Thiết bị cắm ở máy khác (rc-agent) — LAN hoặc Tailscale

Trên máy đang cắm thiết bị / chạy emulator (Mac hoặc Linux):

```bash
./rc-agent             # cờ: --port --tcpip-port --adb-base --no-stream
```

Trên app desktop: nhập **IP của máy agent** (IP LAN hoặc IP Tailscale `100.x.y.z`) vào ô dưới
cùng rồi bấm **Quét agent** — app hỏi cổng discovery **8888** và liệt kê **mọi thiết bị** máy
đó đang có (USB, wireless, emulator). Bấm để mở phiên như bình thường.

Agent là **trạm chung chuyển**: mọi thiết bị được expose lại dưới địa chỉ máy agent, mỗi máy
một cổng adb (15553, 15554…) và một dải 4 cổng stream riêng (27183+). IP thật của điện thoại
không bao giờ xuất hiện — máy ở xa qua tailnet vẫn điều khiển được điện thoại cắm USB dù điện
thoại không nằm trong tailnet. Agent quét lại `adb devices` mỗi 3 giây nên cắm thêm máy giữa
chừng vẫn thấy.

Từng quét (hoặc còn `adb connect` từ trước) thì lần chạy sau **không cần quét lại**: app tự dò
cổng discovery trên host của các thiết bị wireless lạ và tự đăng ký mapping.

> **Firewall trên máy agent** phải mở các cổng: `8888` (discovery), `15553+` (adb, mỗi thiết
> bị một cổng), `27183+` (stream, mỗi thiết bị 4 cổng) — trên đúng interface bạn dùng (LAN
> và/hoặc `tailscale0`). Agent phải khởi động **sau** khi Tailscale đã lên (nó bind IP theo
> interface có mặt lúc khởi động). Giao thức chi tiết:
> [`docs/AGENT_PROTOCOL.md`](docs/AGENT_PROTOCOL.md).

### Tùy chọn trên màn hình chọn thiết bị

| Tùy chọn | Ý nghĩa |
|---|---|
| **Kích thước** | Giới hạn cạnh dài của video (Full/1920/1280/…). Muốn chữ nét hơn → tăng kích thước, không phải tăng bitrate. Emulator nên để 1920 (encoder software). |
| **Bitrate** | Mức **trần** cho encoder (mặc định 8 Mbps). Nội dung tĩnh dùng rất ít; chỉ thấy khác khi chuyển động mạnh (game, video, cuộn nhanh). |
| **Điều khiển chuột & bàn phím** | Bỏ tick = chỉ xem (không mở kênh control). |
| **Phát âm thanh thiết bị** | Stream + phát audio (tốn thêm băng thông; cần Android 11+). |
| **Hiện FPS trên tiêu đề** | Bật dòng chỉ số hiệu năng (bên dưới). |

### Đọc chỉ số trên tiêu đề cửa sổ

```
RigControlNative — 192.168.1.4:15553 (LAN trực tiếp) · 60 FPS · pipe 6ms · lag +2ms · GPU NVIDIA (NVDEC)
```

- **(LAN trực tiếp / LAN qua adb / USB / adb (máy ảo))** — đường stream thật sau khi kết nối.
- **FPS** — khung hình nhận được mỗi giây (màn hình tĩnh → 0 là bình thường).
- **pipe** — trễ pipeline desktop: packet vào decoder → frame lên GPU. Vài ms là khoẻ.
- **lag** — trễ **tích luỹ** so với frame tốt nhất của phiên: bình thường +0–5ms; leo lên
  hàng trăm ms nghĩa là mạng/encoder đang nghẽn (hình tụt sau thực tế).
- Cuối cùng là backend decode (NVDEC / VAAPI / CPU).

### Biến môi trường (chạy không cần UI chọn máy)

| Biến | Ý nghĩa |
|---|---|
| `RC_SERIAL` | adb serial; dạng `ip:port` → tự `adb connect`. Đặt là bỏ qua màn hình chọn. |
| `RC_TCP_ADDR` | `ip[:port]` → connect TCP thẳng (server đã chạy sẵn). |
| `RC_MAX_SIZE` / `RC_BIT_RATE` / `RC_MAX_FPS` | Giới hạn cạnh dài / bitrate bps / fps (mặc định 0/8000000/60). |
| `RC_AUDIO` / `RC_CONTROL` / `RC_SHOW_FPS` | 0/1 (mặc định 0/1/1). |
| `RC_HWDEC` | `off` → ép software decode. |
| `RC_SERVER_PATH` | Đường dẫn `rc-server` (mặc định `server/rc-server` tương đối thư mục chạy). |

### Xử lý sự cố

Chạy app **từ terminal** — log ba tầng đổ về cùng chỗ: `[core]` (desktop), `[rc-server]`
(trong thiết bị, theo đường adb), và console của `rc-agent` (trên máy agent, có timestamp).

| Triệu chứng / thông báo | Nguyên nhân thường gặp |
|---|---|
| `LAN trực tiếp không thông ở bước "connect video"` → adb tunnel | Cổng stream bị firewall chặn trên máy agent, hoặc thiết bị sau NAT. |
| `...không thông ở bước "đọc device_meta"` → adb tunnel | Connect tới được relay nhưng đường vào thiết bị chết (adb forward đứt — xem console agent, dòng `BỎ CUỘC`). |
| `cổng LAN đã bị dịch vụ khác chiếm` → adb tunnel | Có dịch vụ khác listen sẵn ở cổng đó (thường là relay của một rc-agent mà app chưa biết — quét agent là hết). |
| Quét agent không thấy gì | Agent chưa chạy / khởi động trước khi Tailscale lên / sai IP-cổng. Thử `nc -vz <ip> 8888`. |
| Qua Tailscale lúc nhanh lúc chậm | `tailscale ping <ip>` — thấy "via DERP" là đang bị relay, không phải kết nối trực tiếp. |
| `lag` leo cao dần | Băng thông không đủ cho bitrate đã chọn → giảm bitrate hoặc kích thước. |
| Hình mờ dù tăng bitrate | Nút cổ chai là kích thước (đang downscale) — tăng **Kích thước**, không phải bitrate. |

## Build từ source

Yêu cầu (Ubuntu): `build-essential cmake ninja-build pkg-config libavcodec-dev libavutil-dev
libswresample-dev libasound2-dev libgtk-4-dev libepoxy-dev`, `adb`, JDK 11+ và Android SDK
(`platforms/android-*/android.jar` + `build-tools/*/d8`, trỏ qua `ANDROID_HOME`). **Không cần
NDK/Gradle** (server là Java thuần, build bằng `javac + d8`).

```bash
make deps      # in gợi ý cài dependency
make all       # build rc-server (dex) + libcore + app GTK + rc-agent
make run       # chạy app GTK (hoặc make run SERIAL=<serial>)
make format    # clang-format C + Java     |  make lint: clang-tidy
make help      # xem toàn bộ target
```

Build chỉ rc-agent trên macOS (chỉ cần Xcode Command Line Tools, không cần GTK/FFmpeg):

```bash
make agent     # tự thêm -DRC_BUILD_GTK=OFF -DRC_BUILD_CORE=OFF trên Darwin
```

## Phát hành (CI/CD)

GitHub Actions ([`.github/workflows/`](.github/workflows)):

- **`ci.yml`** — mỗi push/PR: build đủ bộ trên Ubuntu 24.04 (server + core + GTK + agent) và
  rc-agent trên macOS; kiểm tra `clang-format` (chỉ cảnh báo).
- **`release.yml`** — push tag `v*` là ra bản phát hành:

```bash
git tag v0.1.0 && git push origin v0.1.0
```

→ tự build + đóng gói `rigcontrol-v0.1.0-linux-x86_64.tar.gz` (app + rc-agent + rc-server) và
`rc-agent-v0.1.0-macos-universal.tar.gz`, tạo GitHub Release kèm release notes.

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

Ba tầng, ranh giới rõ ràng qua **C API** (`core/include/rc/rc_client.h`):

1. **rc-server** (Android, Java qua `app_process`): capture + encode H.264/Opus + inject input.
2. **libcore** (C): adb/deploy, demux, FFmpeg decode (hw → sw fallback), audio ALSA, protocol.
3. **Front-end native** (GTK4 trước): chỉ render frame (GL, shader YUV→RGB) + bắt input.

```
RigControlNative/
<<<<<<< HEAD
  CMakeLists.txt                # top-level: build core + gtk frontend
  README.md
  docs/PROTOCOL.md              # đặc tả protocol video + control server↔core (nguồn sự thật)
  docs/AGENT_PROTOCOL.md        # đặc tả giao thức discovery + relay của rc-agent (nguồn sự thật)
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
    agent/                      # rc-agent: CLI chạy trên máy cắm thiết bị/emulator (mac/Linux)
    swiftui/                    # front-end macOS (Swift/SwiftUI + AppKit, MTKView + Metal)
                                #   build-mac.sh (swiftc, không cần Xcode đầy đủ), bridge/ (module
                                #   map C API), Sources/*.swift
    winui/                      # phase sau (C++/WinUI 3, SwapChainPanel + D3D11)
  third_party/
    miniaudio/                  # backend audio cross-platform (single-header, vendored)
=======
  server/          # rc-server Android (Java thuần, build javac + d8 → dex)
  core/            # libcore C: client.c, adb.c, net.c, server_deploy.c, demuxer.c,
                   #            decoder.c, audio.c, control_msg.c
  frontends/gtk/   # app GTK4 (chooser, session, render GL, input)
  frontends/agent/ # rc-agent: discovery + relay, C thuần POSIX (Linux/macOS)
  docs/PROTOCOL.md        # protocol video/audio/control (nguồn sự thật)
  docs/AGENT_PROTOCOL.md  # protocol discovery + relay của rc-agent
>>>>>>> 42bc9b07321123ecba6dc23a23ca3e7232ddb1f8
```

**Điểm nhấn low-latency**: encoder CBR + không B-frame + IDR theo yêu cầu; decoder
`LOW_DELAY` 1 thread, decode ngay trên thread đọc socket (không hàng đợi); render giữ đúng
**một** frame chờ (frame mới đè frame cũ); `TCP_NODELAY` hai đầu. Chi tiết protocol trong
[`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Trạng thái & lộ trình

🚧 **Phase 6** — LAN transport trực tiếp + hardware decode. Đã xong Phase 1–5: server
capture/encode video + audio; libcore demux/decode/C API; GTK render; control chuột/bàn
phím/nút thiết bị; audio Opus→ALSA; LAN trực tiếp có token + hw decode (NVDEC/VAAPI, fallback
sw); rc-agent discovery/relay đa thiết bị + tự phát hiện; đo trễ pipe/lag trên tiêu đề.

<<<<<<< HEAD
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

**Threading**: 1 thread nhận network + demux + decode (H.264 low-delay, decode ngay trên
thread đọc socket — không hàng đợi trung gian); frame đẩy về UI thread qua callback (GTK
marshal bằng `g_idle_add`). Audio chạy thread riêng.

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

Yêu cầu: `libavcodec-dev libavutil-dev libswresample-dev`, `libgtk-4-dev libepoxy-dev` + EGL/GL, `adb` (platform-tools), JDK 11+,
(audio qua **miniaudio** — dlopen ALSA/PulseAudio lúc chạy, không cần `libasound2-dev` để build),
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

### rc-agent — điều khiển thiết bị cắm ở MÁY KHÁC (LAN hoặc tailnet)

Khi điện thoại cắm cáp (hoặc emulator chạy) trên một máy khác — ví dụ một máy Mac — chạy
`rc-agent` trên máy đó, rồi ở app **chỉ cần nhập IP của máy agent** (LAN hoặc Tailscale) và bấm
"Quét agent": app tự hỏi cổng discovery **8888** và liệt kê ra **mọi thiết bị** máy đó đang có,
cả máy thật lẫn máy ảo. Không phải chép tay địa chỉ nào.

Agent là **trạm chung chuyển**: mọi thiết bị được expose lại dưới địa chỉ của *máy agent*, mỗi
máy một cổng adb riêng (15553, 15554…). IP của điện thoại không bao giờ xuất hiện — nhờ vậy máy
ở xa qua tailnet điều khiển được điện thoại cắm USB, dù điện thoại không hề nằm trong tailnet.

- **Điện thoại USB / emulator**: tự `adb tcpip 5555` + `adb forward` rồi relay TCP.
- **Máy đã wireless adb**: relay thẳng tới địa chỉ trong serial.
- Relay bind riêng vào từng IP **LAN + Tailscale + loopback**, không mở `0.0.0.0`. Mỗi thiết bị
  còn có dải cổng stream riêng (kèm replay token + tuần tự hoá kênh) để phiên chạy "LAN trực
  tiếp"; không được thì core tự fallback adb tunnel. Relay là C thuần trong agent — không cần
  `socat`.
- Agent **quét lại định kỳ**: cắm thêm điện thoại lúc agent đang chạy vẫn thấy, không phải khởi
  động lại (khởi động lại = đứt mọi phiên).

Agent phải **chạy liên tục** để giữ relay sống (Ctrl-C dọn `adb forward`). Giao thức đầy đủ:
[`docs/AGENT_PROTOCOL.md`](docs/AGENT_PROTOCOL.md).

Build trên macOS (chỉ cần Xcode Command Line Tools + adb, không cần GTK/FFmpeg/ALSA):

```bash
make agent                                # trên macOS tự tắt GTK + core, tự chọn generator
./build/frontends/agent/rc-agent          # cờ: --port --tcpip-port --adb-base --no-stream
```

Trên macOS, `make agent` tự thêm `-DRC_BUILD_GTK=OFF -DRC_BUILD_CORE=OFF` (libcore cần ALSA
vốn Linux-only) và tự chọn generator `Ninja` nếu có, không thì `Unix Makefiles`. Chuẩn bị:

```bash
xcode-select --install                              # clang + make
brew install --cask android-platform-tools          # adb (nếu chưa có)
# brew install ninja                                 # tuỳ chọn, cho build nhanh hơn
```

Hoặc gọi thẳng CMake không qua Makefile:

```bash
cmake -B build -DRC_BUILD_GTK=OFF -DRC_BUILD_CORE=OFF && cmake --build build --target rc-agent
```

Trên Linux: `make agent` (build kèm cả libcore).

## Build & chạy (macOS — front-end SwiftUI)

Front-end macOS viết bằng **Swift/SwiftUI + AppKit**, render bằng **Metal (MTKView)**, dùng chung
`libcore` (C) như GTK. Ngang tính năng với GTK: chọn thiết bị, quét rc-agent, wireless adb, LAN
trực tiếp, video + **audio** (miniaudio → Core Audio), điều khiển chuột/bàn phím + nút thiết bị.

Yêu cầu: **Xcode Command Line Tools** (không cần Xcode đầy đủ — build bằng `swiftc` + SDK macOS,
vốn đã có SwiftUI/Metal), **CMake**, **Homebrew ffmpeg** (`brew install ffmpeg`), **adb**. Build
`server/rc-server` như GTK (cần JDK + Android SDK) để core tự push khi mở phiên.

```bash
brew install ffmpeg cmake            # adb: brew install --cask android-platform-tools
xcode-select --install               # clang/swiftc + SDK

./server/build.sh                    # dex server (một lần; hoặc make server)
make mac                             # dựng libcore + app → build-mac/RigControlNative.app
open build-mac/RigControlNative.app  # hoặc: make run-mac SERIAL=<serial>
```

`make mac` gọi `frontends/swiftui/build-mac.sh`: dựng `libcore` qua CMake (tự bật core, tắt GTK),
biên dịch toàn bộ `Sources/*.swift` với `swiftc` (module map `bridge/` phơi C API), rồi đóng gói
`.app` (ad-hoc codesign). Cấu hình qua **cùng bộ biến môi trường như GTK** (`RC_SERIAL`,
`RC_TCP_ADDR`, `RC_MAX_SIZE`, `RC_BIT_RATE`, `RC_MAX_FPS`, `RC_AUDIO`, `RC_CONTROL`, `RC_SHOW_FPS`,
`RC_HWDEC`, `RC_SERVER_PATH`); đặt `RC_SERIAL`/`RC_TCP_ADDR` → mở thẳng một phiên, bỏ qua bộ chọn.

> libcore không có generator riêng cho mac: `swiftc` link thẳng `build-mac/core/librccore.a`
> (static) + FFmpeg (dylib Homebrew, absolute install-name) + framework Core Audio/AppKit/Metal.
> Hardware decode (VideoToolbox) chưa có — mac dùng **software decode** (CUDA/VAAPI của core tự bỏ
> qua); đủ mượt cho H.264 nhờ đường low-delay.

### Định dạng & lint

- `.clang-format` áp cho cả C (LLVM, 4-space) và Java (Google, 4-space).
- `.clang-tidy` cho C của libcore (cần `compile_commands.json` — CMake tự sinh).
- Cài công cụ: `sudo apt install clang-format clang-tidy`.

### Kiểm thử

1. **Video**: màn hình Android hiển thị trong cửa sổ, mượt.
2. **Độ trễ**: mở đồng hồ mili-giây trên điện thoại, chụp chung để đo lệch (mục tiêu vài chục ms qua USB).
3. **Input**: rê chuột/click và gõ phím trên cửa sổ → thao tác xảy ra trên thiết bị.

## Trạng thái

🚧 Đang phát triển — **Phase 7**: port macOS (front-end SwiftUI + Metal, audio miniaudio).

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
  `RC_MAX_FPS`, `RC_AUDIO`, `RC_CONTROL`, `RC_HWDEC`, `RC_SERVER_PATH`); đặt `RC_SERIAL`/`RC_TCP_ADDR` sẽ
  bỏ qua bộ chọn và kết nối thẳng. **Wireless adb**: `RC_SERIAL` dạng `ip:port` (hoặc nhập
  vào ô *Kết nối Wi-Fi* trên bộ chọn) → core tự `adb connect` rồi deploy như USB; bật bằng
  `make tcpip` (máy cắm USB) rồi `make connect IP=<ip>` / `make run SERIAL=<ip>:5555`.

- **Phase 4** — điều khiển ngược: server inject qua `InputManager`/`InputManagerGlobal`
  (reflection, Android 14+). Chuột → cảm ứng (nhấn/kéo/thả), cuộn, bàn phím (phím thường → TEXT,
  phím điều hướng/sửa → KEY, tổ hợp **Ctrl/Alt+phím → KEY kèm metastate**), nút thiết bị
  **BACK/HOME/RECENT/MENU/POWER/VOLUME** trên thanh công cụ, và **DEVICE_ACTION** (xoay qua
  `IWindowManager.freezeRotation`, screen off/on, panel thông báo — đều có nút trên thanh công
  cụ). Đã kiểm chứng end-to-end: HOME đổi foreground, vuốt mở app drawer, ROTATE đổi
  `user_rotation`.
  > Hạn chế đã biết: gõ **tiếng Việt/ký tự có dấu** chưa hoạt động — server inject TEXT qua
  > `KeyCharacterMap` (chỉ map được ký tự có trên bàn phím ảo, giống scrcpy chế độ thường);
  > cần UHID/IME injection ở phase sau.
- **Phase 5** — audio playback ở desktop: `libcore` giải mã **Opus** (FFmpeg) → `libswresample`
  (S16 interleaved) → phát qua **ALSA** (blocking, tự pace real-time; buffer ~60ms cho gaming).
  OpusHead tự tổng hợp từ `audio_meta`. Thiết bị không có audio / không mở được ALSA → tự bỏ
  qua, video vẫn chạy. Đã kiểm chứng phát đúng 48000 mẫu/s ra sound card song song với video.

> Backend audio dùng **miniaudio** (single-header, cross-platform): Core Audio trên macOS, WASAPI
> trên Windows, ALSA/PulseAudio trên Linux — nạp backend theo nền tảng lúc chạy. Trước đây là ALSA
> thuần (Linux-only); nhờ miniaudio libcore build + phát audio được trên cả ba nền tảng mà không
> đụng `client.c`. Ghi PCM vào ring buffer là **blocking khi đầy** nên vẫn pace server qua
> backpressure TCP như `snd_pcm_writei` cũ. Header vendor tại `third_party/miniaudio/`.

- **Phase 6** — LAN transport trực tiếp: transport TCP tự deploy server (`tcp=true`) qua adb rồi
  stream thẳng `ip:27183` không đi vòng adb tunnel (UI: tick *LAN trực tiếp* ở ô Wi-Fi; env:
  `RC_SERIAL` + `RC_TCP_ADDR`). Cổng LAN được bảo vệ bằng **token ngẫu nhiên** (client sinh,
  truyền cho server qua adb; mỗi kết nối TCP phải gửi token trước — xem PROTOCOL §1.2).
  Hardware decode bật mặc định, thử lần lượt **CUDA/NVDEC** (NVIDIA) → **VAAPI** (Intel/AMD) →
  software: decode trên GPU → hwdownload NV12 (render đã hỗ trợ NV12 qua texture RG8); decode
  hw lỗi giữa chừng thì core tự dựng lại decoder software và phát tiếp. Ép software bằng
  `RC_HWDEC=off`. Zero-copy dmabuf → GL để phase sau.

- **Phase 7 (đang làm)** — port **macOS**: front-end **SwiftUI + AppKit**, render **Metal**
  (MTKView, shader YUV→RGB I420/NV12, letterbox), input chuột/bàn phím → Android keycode/TEXT +
  navbar thiết bị, chooser + quét rc-agent + wireless adb + LAN trực tiếp — dùng chung `libcore`
  qua C API (module map). Audio backend chuyển từ ALSA sang **miniaudio** (Core Audio trên mac,
  cross-platform) nên libcore build + phát audio được trên cả ba nền tảng. Đã kiểm chứng
  end-to-end với emulator: deploy server → decode H.264 (software) → phát Opus qua Core Audio.
  Còn lại của mac: hardware decode VideoToolbox, đóng gói/ký chính thức.

Còn lại: zero-copy dmabuf (VAAPI → EGLImage), hardware decode VideoToolbox (mac), port Windows
(WinUI 3).
=======
Còn lại: zero-copy GPU (dmabuf/CUDA-GL interop), gõ tiếng Việt (cần UHID/IME injection),
port Windows (WinUI 3) và macOS (SwiftUI) dùng chung libcore — Phase 7.
>>>>>>> 42bc9b07321123ecba6dc23a23ca3e7232ddb1f8

## License

Apache-2.0 (dự kiến).
