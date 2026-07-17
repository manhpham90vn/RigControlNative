# RigControlNative — Đặc tả giao thức Agent

Tài liệu này là **nguồn sự thật** cho giao thức giữa `rc-agent` (máy đang cắm thiết bị) và app
desktop. Mọi thay đổi ở `frontends/agent/main.c` hoặc phía app phải cập nhật tại đây trước.

Không liên quan tới `docs/PROTOCOL.md` — tài liệu đó đặc tả stream giữa `rc-server` (Android) và
`libcore`. Giao thức ở đây chỉ lo việc **tìm ra thiết bị nào đang có và nối vào bằng cổng nào**;
sau khi nối xong thì mọi thứ chạy đúng theo `docs/PROTOCOL.md`, agent không tham gia.

## 0. Mô hình

`rc-agent` là **trạm chung chuyển**. Mọi thiết bị nó thấy qua adb (emulator, điện thoại USB, máy
đã wireless) đều được expose lại dưới **địa chỉ của máy chạy agent**, mỗi thiết bị một cổng adb
riêng. Hệ quả quan trọng:

- IP của thiết bị **không bao giờ** xuất hiện trong giao thức và client không cần biết. Client chỉ
  nói chuyện với agent. Nhờ vậy máy ở xa qua tailnet điều khiển được điện thoại cắm USB — điện
  thoại không nằm trong tailnet, IP LAN của nó vô nghĩa với máy remote.
- Agent phải **chạy liên tục** để giữ relay sống, với **mọi** loại thiết bị — không có ngoại lệ
  nào cho phép agent thoát sau khi cấu hình xong. Đóng agent = mất mọi phiên đang chạy.
- Relay bind riêng vào từng IP LAN + Tailscale + loopback của máy agent, **không** bind `0.0.0.0`
  (tránh lỡ mở ra interface public).

### 0.1 Slot

Mỗi thiết bị được gán một **slot** — số nguyên `i` từ 0 tới `MAX_SLOTS-1` (**hiện tại
`MAX_SLOTS = 8`**). Slot quyết định toàn bộ cổng của thiết bị đó (§2).

Slot gán **theo serial** và **giữ suốt đời tiến trình agent, không tái dụng**: rút thiết bị ra
thì slot của nó không được cấp cho máy khác, và cắm lại đúng serial đó thì nhận lại đúng slot cũ.
Đổi lại, agent chỉ phục vụ được `MAX_SLOTS` serial **khác nhau** cho tới lần khởi động lại; vượt
trần thì agent log rõ và bỏ qua thiết bị mới, không im lặng.

Đây là đánh đổi có chủ đích: nhờ slot không bao giờ đổi chủ, agent **không bao giờ phải gỡ
listener đang chạy** — thứ vốn là nguồn race lớn nhất khi có rescan nền (§0.2).

### 0.2 Rescan

Agent quét lại `adb devices` **định kỳ (~3s)** trên một thread nền: serial mới → cấp slot + dựng
relay; serial biến mất → đánh dấu vắng mặt và **thôi liệt kê ở discovery**, nhưng listener + slot
giữ nguyên. Nhờ vậy cắm thêm điện thoại lúc agent đang chạy vẫn dùng được ngay, không phải khởi
động lại agent (khởi động lại = đứt mọi phiên).

## 1. Cổng discovery

| | |
|---|---|
| Giao vận | TCP |
| Cổng mặc định | **8888** (đổi bằng `--port`) |
| Bind | mỗi IP LAN/Tailscale/loopback một listener |
| Mã hoá | UTF-8, kết dòng `\n` (LF) |

Đây là cổng **duy nhất** người dùng cần nhập vào app. Từ nó app suy ra toàn bộ phần còn lại.

### 1.1 Khuôn khổ

Một lần hỏi–đáp trên một kết nối, không có request:

1. Client mở TCP tới `<ip-agent>:8888`.
2. Client **không gửi gì**.
3. Agent ghi ngay toàn bộ đáp ứng rồi **đóng socket**.
4. Client đọc tới EOF, parse, xong.

Client phải đọc tới EOF chứ không dừng ở dòng đầu — số dòng không được báo trước. Đáp ứng luôn
nhỏ (vài trăm byte), agent ghi một lần nên client không cần lo phân mảnh ngoài việc đọc tới EOF.

### 1.2 Đáp ứng

Dòng đầu là banner nhận dạng + phiên bản:

```
RCAGENT <version>
```

`<version>` là số nguyên thập phân, **hiện tại là `1`**. Sau đó là **0 hoặc nhiều** dòng thiết bị,
mỗi dòng 5 trường **phân tách bằng TAB** (`\t`):

```
<port>\t<kind>\t<serial>\t<model>\t<stream_base>
```

| Trường | Kiểu | Nghĩa |
|---|---|---|
| `port` | số nguyên | cổng adb của thiết bị này **trên máy agent** |
| `kind` | `emulator` \| `USB` \| `wireless` | thiết bị đấu nối với agent kiểu gì — chỉ để hiển thị |
| `serial` | chuỗi | serial thiết bị theo adb **của agent** — chỉ để hiển thị/phân biệt |
| `model` | chuỗi | model theo `adb devices -l`, hoặc `-` nếu adb không báo |
| `stream_base` | số nguyên | cổng public đầu dải stream của thiết bị này; **`0` = không relay stream** |

Chỉ thiết bị **đang có mặt** mới được liệt kê — máy đã rút ra biến mất khỏi đáp ứng dù slot của
nó vẫn còn (§0.1).

Ví dụ thật:

```
RCAGENT 1
15553	emulator	emulator-5552	Android_SDK_built_for_arm64	27183
15554	emulator	emulator-5554	Android_SDK_built_for_arm64	27187
15555	USB	39061FDJH00TXP	Pixel_7	27191
```

Không trường nào chứa TAB hay LF: `port`/`stream_base` là số, `kind` thuộc tập đóng, còn
`serial`/`model` do adb sinh (`model:` trong `adb devices -l` không có khoảng trắng). Không có
escaping — nếu về sau cần trường tự do thì phải lên version.

### 1.3 Client dùng đáp ứng thế nào

Với mỗi dòng thiết bị, địa chỉ adb là **`<ip-agent>` ghép với `<port>` của dòng đó** — trong đó
`<ip-agent>` là chính IP client vừa dùng để nối cổng discovery, *không phải* `serial`:

```
adb connect <ip-agent>:<port>
```

Thiết bị sau đó hiện ra trong `adb devices` với serial `<ip-agent>:<port>` và dùng được y như một
thiết bị wireless bình thường — push, shell, deploy `rc-server`. `serial` trong đáp ứng **chỉ để
hiển thị**, không được đem đi `adb connect`.

`stream_base` quyết định đường stream của phiên (§2.2):

- `stream_base > 0` → client mở phiên LAN trực tiếp: chọn `k` trong `0..STREAM_COUNT-1`, bảo
  `rc-server` listen cổng **`27183 + k`** (trong thiết bị) rồi connect tới
  **`<ip-agent>:<stream_base + k>`**. Hai số này **khác nhau** — xem §2.2.
- `stream_base == 0` → client đi thẳng adb tunnel, **không** thử LAN trực tiếp (thử cũng chỉ tốn
  vài giây retry rồi mới fallback).

## 2. Cổng relay

Client không cần tự suy ra các cổng này (cổng adb đã có trong đáp ứng discovery), nhưng chúng là
một phần hợp đồng vì agent và app phải khớp nhau.

Toàn bộ cổng của thiết bị suy ra từ **slot** `i` của nó (§0.1):

| | Công thức | Với `MAX_SLOTS = 8` |
|---|---|---|
| cổng adb public | `--adb-base` + `i`, mặc định `15553 + i` | 15553–15560 |
| dải stream public | `STREAM_BASE + i*STREAM_COUNT + k`, `k = 0..3` | 27183–27214 |
| cổng local trung gian | cổng public + `LOC_OFFSET` (10000) | 25553–25560, 37183–37214 |

`STREAM_BASE = 27183`, `STREAM_COUNT = 4`.

### 2.1 Cổng adb — một cổng mỗi thiết bị

Agent relay `<ip-agent>:<port>` về adbd của thiết bị. **Mọi loại thiết bị đều được relay** — cách
với tới adbd là khác nhau, còn phía client thì không phân biệt:

- **emulator / USB**: `adb forward` kéo adbd trong thiết bị ra `127.0.0.1:<port+10000>` rồi relay
  tới đó. adbd không nghe TCP khi ở chế độ USB nên agent chạy `adb tcpip 5555` (đổi bằng
  `--tcpip-port`) trước; cổng đó chỉ dùng qua `adb forward` trên localhost của agent, không phải để
  cả LAN nối vào.
- **wireless**: relay thẳng tới IP:port trong serial, không cần forward.

Cổng gắn với **slot**, mà slot gắn với **serial** (§0.1) — nên khác với bản đặc tả trước, cắm/rút
rồi cắm lại **không** làm cổng đổi chỗ. Dù vậy client vẫn **phải** hỏi lại discovery mỗi lần và
**không** được nhớ cổng giữa các lần chạy: slot chỉ ổn định trong một đời tiến trình agent, khởi
động lại agent là cấp lại từ đầu.

### 2.2 Dải cổng stream — mỗi thiết bị một dải

Mỗi thiết bị có dải stream **riêng** (`STREAM_COUNT = 4` phiên LAN trực tiếp đồng thời mỗi máy),
nên **mọi** thiết bị đều đi được đường "LAN trực tiếp", không chỉ thiết bị đầu tiên.

Mấu chốt: **cổng public khác cổng trong thiết bị**. Mỗi thiết bị là một namespace port riêng, nên
cổng *trong* thiết bị luôn là `27183 + k` với mọi máy; chỉ cổng *public* mới phải tách nhau ra để
agent biết kết nối nào thuộc máy nào:

```
public 27183 + i*4 + k   →  adb forward  →  device port 27183 + k
```

Vì vậy phía app, một phiên LAN trực tiếp cần **hai** số cổng khác nhau:

| | Giá trị | Dùng ở đâu |
|---|---|---|
| cổng server listen | `27183 + k` | tham số `port=` truyền cho `rc-server` qua adb |
| cổng client connect | `stream_base + k` | địa chỉ TCP client nối tới trên máy agent |

Trước đây hai số này luôn bằng nhau nên `rc_config.tcp_addr` gánh cả hai vai; giờ phải tách bằng
`rc_config.tcp_device_port` (§3).

Client chọn `k` **theo từng thiết bị**, không phải toàn cục: `k` nhỏ nhất chưa bị phiên đang sống
nào của cùng thiết bị chiếm. Hết 4 slot `k` → phiên đó đi adb tunnel.

Muốn bỏ hẳn relay stream (mọi thiết bị đi adb tunnel): `--no-stream` — khi đó agent báo
`stream_base = 0` cho mọi dòng.

## 3. Ghi chú tương thích

- Banner `RCAGENT <version>` phải là thứ đầu tiên trên dây. Client **phải** từ chối nếu dòng đầu
  không khớp `RCAGENT ` — bảo vệ khỏi việc người dùng nhập nhầm cổng của dịch vụ khác.
- Client **phải** từ chối `version` nó không biết thay vì đoán. Version tăng khi đổi khuôn khổ hoặc
  đổi nghĩa trường sẵn có.
- Trường mới **chỉ được thêm vào cuối dòng**, không đổi nghĩa trường cũ. Client phải bỏ qua trường
  thừa nó không hiểu — thêm trường ở cuối không cần lên version.
- Dòng không parse được thì **bỏ qua dòng đó**, không bỏ cả đáp ứng.
- Giá trị hằng phải khớp giữa `frontends/agent/agent.h` (`DEFAULT_DISCOVERY_PORT`,
  `DEFAULT_ADB_BASE`, `STREAM_BASE`, `STREAM_COUNT`, `MAX_SLOTS`) và phía app (`RC_LAN_BASE_PORT`
  + `STREAM_COUNT` trong `frontends/gtk/session.c`).
- `STREAM_BASE` phía agent phải khớp cổng mà app truyền vào `rc_config.tcp_device_port`: agent
  forward cổng public về đúng `STREAM_BASE + k` trong thiết bị, nên nếu app bảo server listen một
  cổng khác thì relay trỏ vào chỗ không ai nghe.

## 4. Bảo mật

Cổng discovery và cổng adb **không xác thực**: ai với tới được thì điều khiển được thiết bị. Agent
giới hạn phơi bày bằng cách chỉ bind IP LAN + Tailscale + loopback thay vì `0.0.0.0`, nhưng trong
LAN thì vẫn là mở cho cả mạng. Chỉ chạy agent trên mạng tin được; muốn chặt hơn thì để tailnet lo
(ACL Tailscale) và đừng chạy trên Wi-Fi công cộng.

Riêng dải cổng stream có token: core sinh token ngẫu nhiên, truyền cho server qua adb (kênh tin
cậy), mỗi kết nối TCP phải gửi token trước — xem `docs/PROTOCOL.md` §1.2. Token đó bảo vệ stream,
**không** bảo vệ cổng adb.
