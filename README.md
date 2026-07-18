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
sudo apt install libgtk-4-1 libepoxy0 libavcodec-extra libasound2t64 adb
tar xzf rigcontrol-v*-linux-x86_64.tar.gz && cd rigcontrol-v*/
./rigcontrol          # chạy từ thư mục giải nén — app tìm server/rc-server cạnh binary
```

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
  server/          # rc-server Android (Java thuần, build javac + d8 → dex)
  core/            # libcore C: client.c, adb.c, net.c, server_deploy.c, demuxer.c,
                   #            decoder.c, audio.c, control_msg.c
  frontends/gtk/   # app GTK4 (chooser, session, render GL, input)
  frontends/agent/ # rc-agent: discovery + relay, C thuần POSIX (Linux/macOS)
  docs/PROTOCOL.md        # protocol video/audio/control (nguồn sự thật)
  docs/AGENT_PROTOCOL.md  # protocol discovery + relay của rc-agent
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

Còn lại: zero-copy GPU (dmabuf/CUDA-GL interop), gõ tiếng Việt (cần UHID/IME injection),
port Windows (WinUI 3) và macOS (SwiftUI) dùng chung libcore — Phase 7.

## License

Apache-2.0 (dự kiến).
