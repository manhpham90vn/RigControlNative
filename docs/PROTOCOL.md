# RigControlNative — Đặc tả Protocol

Tài liệu này là **nguồn sự thật** cho giao thức giữa `rc-server` (Android) và `libcore` (desktop).
Mọi thay đổi ở server hoặc core phải cập nhật tại đây trước.

Quy ước chung:
- Mọi số nguyên nhiều byte truyền theo **big-endian (network byte order)**.
- Toạ độ điểm ảnh tính theo **kích thước video** (`width`/`height` trong `device_meta` — là kích
  thước encode, có thể nhỏ hơn màn hình thật khi đặt `max_size`). Desktop scale toạ độ cửa sổ →
  video trước khi gửi; server scale tiếp video → pixel màn hình thiết bị khi inject.
- Có tối đa **ba socket** độc lập: *video*, *audio*, *control*. Số lượng và thứ tự thiết lập
  mô tả ở mục Kết nối.

---

## 1. Kết nối & tunnel

### 1.1 USB (mặc định, độ trễ thấp nhất)

Dùng `adb` forward/reverse tới một *localabstract* socket do server mở.

- Server chạy qua `app_process` (xem `docs`/README), lắng nghe localabstract name
  `localabstract:rigcontrol`.
- Desktop dùng **reverse tunnel** (khuyến nghị, giống scrcpy): server *connect ra*, desktop *listen*.
  - Desktop: `adb reverse localabstract:rigcontrol tcp:<port>` rồi listen trên `127.0.0.1:<port>`.
  - Server mở các kết nối tới localabstract theo **thứ tự cố định**: **video → [audio] → [control]**.
    Audio chỉ mở khi `audio=true`; control chỉ mở khi `control=true`.

### Thứ tự stream (áp dụng cho cả USB và TCP)

Luôn theo thứ tự: **video trước, rồi audio (nếu bật), rồi control (nếu bật)**. Hai bên phải
thống nhất `audio`/`control` (desktop truyền cờ này làm tham số khi khởi động server) để
accept/connect đúng số socket và đúng thứ tự.

### 1.2 TCP / LAN (Wi-Fi nội bộ)

- Server listen trực tiếp trên `0.0.0.0:<port>` (mặc định `27183`; server chạy với `tcp=true port=<port>`).
- Desktop connect tới `tcp:<device-ip>:<port>`, mở **video trước, control sau** (cùng thứ tự như USB).
- Deploy: nếu client biết adb serial (kể cả wireless adb `ip:5555`), client tự `adb push` + chạy
  server (`tcp=true`) rồi connect trực tiếp — stream **không** đi qua adb tunnel. Không có serial
  → giả định server đã chạy sẵn.
- Lấy IP thiết bị: người dùng nhập tay, hoặc dùng `adb tcpip` + `adb shell ip route` để hỗ trợ.

**Xác thực (token)** — cổng LAN mở cho cả mạng nên cần hàng rào chống máy lạ chiếm stream /
inject input:

- Khi client tự deploy, nó sinh **token ngẫu nhiên 32 ký tự hex** và truyền cho server qua
  tham số `token=<hex>` (đi qua adb — kênh tin cậy).
- Server chạy với `token=...` yêu cầu **mỗi kết nối TCP** gửi đúng 32 byte ASCII token này
  **trước mọi dữ liệu khác**; sai token hoặc im lặng quá 5 giây → server đóng kết nối đó và
  accept tiếp (client hợp lệ không bị chặn bởi kẻ đến trước).
- Server chạy thủ công **không có** `token=` thì bỏ qua bước này (như cũ) — chỉ nên dùng trong
  mạng tin cậy.
- USB (adb tunnel) không dùng token: tunnel localabstract đã giới hạn trong máy + thiết bị.

Trong cả hai chế độ, **thứ tự stream không đổi**: video là kết nối/luồng đầu tiên, control là thứ hai.

---

## 2. Handshake

Ngay sau khi video socket mở, server gửi **device meta** một lần:

```
device_meta:
  magic       : uint32   = 0x52434E31   ("RCN1")
  version     : uint16   = 1
  codec_id    : uint32   (xem bảng codec)
  width       : uint16   độ rộng màn hình thiết bị (px)
  height      : uint16   độ cao màn hình thiết bị (px)
  device_name : [64] byte, UTF-8, NUL-padded
```

Bảng codec (`codec_id`, 4 ký tự ASCII đóng gói big-endian):

| codec_id (hex) | ASCII | Ý nghĩa           |
|----------------|-------|-------------------|
| `0x68323634`   | h264  | H.264 (MVP)       |
| `0x68323635`   | h265  | H.265 (phase sau) |
| `0x00617631`   | \0av1 | AV1 (phase sau)   |

Control socket **không** có handshake riêng — dùng chung thông tin từ device_meta.

---

## 3. Luồng Video

Sau `device_meta`, video socket phát liên tục các **packet** theo khung:

```
video_packet:
  pts_flags : uint64   bit 63 = CONFIG (packet cấu hình, vd SPS/PPS)
                       bit 62 = KEYFRAME
                       bit 61..0 = PTS (micro-giây); với CONFIG packet PTS bỏ qua
  len       : uint32   độ dài payload (byte)
  payload   : [len] byte  dữ liệu H.264 (Annex-B NAL units)
```

Ghi chú:
- Packet đầu tiên thường là **CONFIG** (SPS/PPS) — core nạp vào decoder trước khi giải mã.
- **Không B-frame**, không reorder: PTS đơn điệu tăng, core render ngay khi decode xong.
- Định dạng bit-flag trùng cách scrcpy đóng gói để dễ đối chiếu/tham khảo.

### 3.1 Xoay màn hình thiết bị (rotation)

Khi thiết bị xoay, độ phân giải luồng thay đổi. Server **không gửi lại `device_meta`**; thay
vào đó nó phát một **CONFIG packet mới** (SPS/PPS mới) rồi một **KEYFRAME** với kích thước mới.
Decoder phía core tự cập nhật, và mỗi `rc_frame` giao cho UI đã mang `width`/`height` hiện tại,
nên front-end chỉ cần đọc kích thước theo từng frame để bố trí lại khung hình. Không cần message
riêng cho việc "theo dõi xoay".

---

## 3b. Luồng Audio (chỉ khi `audio=true`)

Ngay sau khi audio socket mở, server gửi **audio meta** một lần:

```
audio_meta:
  magic       : uint32   = 0x52434E41   ("RCNA")
  codec_id    : uint32   (xem bảng audio codec; 0 = audio KHÔNG khả dụng)
  sample_rate : uint32   (vd 48000)
  channels    : uint8    (vd 2)
```

Nếu `codec_id == 0`: thiết bị không hỗ trợ capture audio (vd Android < 11 hoặc bị chặn) —
core bỏ qua audio, tiếp tục video/control bình thường. Socket vẫn được mở để giữ đúng thứ tự.

Bảng audio codec:

| codec_id (hex) | ASCII  | Ý nghĩa                    |
|----------------|--------|----------------------------|
| `0x00000000`   | —      | audio không khả dụng       |
| `0x6F707573`   | opus   | Opus (mặc định)            |
| `0x00616163`   | \0aac  | AAC (phase sau)            |
| `0x00726177`   | \0raw  | PCM 16-bit LE (phase sau)  |

Sau `audio_meta`, audio socket phát các packet **cùng khung với video** (`audio_packet` ≡
`video_packet`): `[pts_flags: u64][len: u32][payload]`. Cờ `CONFIG` dùng cho gói cấu hình codec
(vd Opus identification/AAC ASC). Core giải mã và phát ra loa desktop; xem README về backend phát.

---

## 4. Luồng Control (desktop → server)

Control socket mang các **event** từ desktop tới thiết bị. Mỗi message bắt đầu bằng 1 byte `type`:

| type | tên            | mô tả                              |
|------|----------------|------------------------------------|
| 0    | `MOUSE_MOTION` | di chuyển/kéo con trỏ              |
| 1    | `MOUSE_BUTTON` | nhấn/thả nút chuột                 |
| 2    | `SCROLL`       | cuộn                               |
| 3    | `KEY`          | phím (down/up); cũng dùng cho nút BACK/HOME/POWER/VOLUME |
| 4    | `TEXT`         | nhập chuỗi UTF-8 (IME/clipboard)   |
| 5    | `DEVICE_ACTION`| hành động đặc biệt (tắt/bật màn hình, panel, xoay) |

Toạ độ `x,y` là **float32** theo pixel video (`device_meta.width/height`); server chịu trách
nhiệm scale về pixel màn hình thiết bị trước khi inject. `buttons` là bitmask trạng thái nút.

### 4.1 MOUSE_MOTION (type=0)
```
buttons : uint32   bitmask nút đang giữ (xem bảng button)
x       : float32
y       : float32
```

### 4.2 MOUSE_BUTTON (type=1)
```
action  : uint8    0 = UP, 1 = DOWN
button  : uint32   nút vừa đổi trạng thái (một bit)
buttons : uint32   bitmask tổng sau thay đổi
x       : float32
y       : float32
```

Bảng button (bitmask):

| bit  | nút          |
|------|--------------|
| 0x01 | trái (LEFT)  |
| 0x02 | phải (RIGHT) |
| 0x04 | giữa (MIDDLE)|

Phía Android map thành `MotionEvent` nguồn `SOURCE_MOUSE` (hoặc touch tương ứng).

### 4.3 SCROLL (type=2)
```
x        : float32   vị trí con trỏ khi cuộn
y        : float32
hscroll  : float32   cuộn ngang  (dương = phải)
vscroll  : float32   cuộn dọc    (dương = lên)
```

### 4.4 KEY (type=3)
```
action    : uint8    0 = UP, 1 = DOWN
keycode   : uint32   Android KeyEvent keycode (KEYCODE_*)
metastate : uint32   Android meta state (SHIFT/CTRL/ALT... bitmask)
repeat    : uint32   số lần lặp (0 nếu không auto-repeat)
```

Desktop chịu trách nhiệm map keycode nền tảng (GDK/Win/macOS) → Android `KEYCODE_*`.

**Nút điều hướng phần cứng** dùng chung message `KEY` này với keycode Android chuẩn (không cần
message riêng):

| nút        | Android keycode      | giá trị |
|------------|----------------------|---------|
| BACK       | `KEYCODE_BACK`       | 4       |
| HOME       | `KEYCODE_HOME`       | 3       |
| APP_SWITCH | `KEYCODE_APP_SWITCH` | 187     |
| MENU       | `KEYCODE_MENU`       | 82      |
| POWER      | `KEYCODE_POWER`      | 26      |
| VOLUME_UP  | `KEYCODE_VOLUME_UP`  | 24      |
| VOLUME_DOWN| `KEYCODE_VOLUME_DOWN`| 25      |
| VOLUME_MUTE| `KEYCODE_VOLUME_MUTE`| 164     |

### 4.5 TEXT (type=4)
```
len   : uint32   độ dài byte của chuỗi UTF-8
text  : [len] byte
```

### 4.6 DEVICE_ACTION (type=5)

Hành động đặc biệt không phải phím (dùng cho các nút trên UI desktop).

```
action : uint8   (xem bảng)
```

| action | tên                        | mô tả                                              |
|--------|----------------------------|----------------------------------------------------|
| 0      | `SCREEN_OFF`               | tắt màn hình thiết bị nhưng vẫn mirror (tiết kiệm pin) |
| 1      | `SCREEN_ON`                | bật lại màn hình thiết bị                          |
| 2      | `EXPAND_NOTIFICATION_PANEL`| mở thanh thông báo                                 |
| 3      | `EXPAND_SETTINGS_PANEL`    | mở bảng cài đặt nhanh                              |
| 4      | `COLLAPSE_PANELS`          | đóng các panel                                     |
| 5      | `ROTATE`                   | xoay/đổi hướng màn hình thiết bị (toggle)          |

Phía Android: `SCREEN_OFF/ON` qua `SurfaceControl.setDisplayPowerMode`; panel qua
`StatusBarManager`; `ROTATE` qua `WindowManager.freezeRotation` (reflection, hidden API).

---

## 5. Đóng kết nối

- Bên nào đóng socket trước thì bên kia coi như phiên kết thúc và dọn dẹp.
- Server tự thoát khi video socket đóng.
- Desktop gửi (tùy chọn) một control `KEY` UP cho mọi phím đang giữ trước khi đóng để tránh
  "kẹt phím" trên thiết bị.

---

## 6. Ghi chú tương thích

- `version` trong `device_meta` cho phép hai bên từ chối nếu lệch phiên bản lớn.
- Các trường/loại message mới **chỉ được thêm vào cuối** với `type` mới; không đổi nghĩa type cũ.
- Toàn bộ giá trị hằng (magic, codec_id, button bits, control type) phải khớp giữa
  `core/src/*.c`, `core/include/rc/rc_client.h` và `server/.../*.java`.
