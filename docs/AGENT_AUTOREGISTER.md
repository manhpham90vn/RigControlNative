# Thiết kế: bind máy thật + auto-register

Bản phân rã công việc để cài đặt `docs/AGENT_PROTOCOL.md`. Đặc tả đó là **hợp đồng**; tài liệu này
là **kế hoạch sửa code**, xoá được sau khi làm xong.

Công việc chia làm ba tầng độc lập nhau, nối bằng hợp đồng ở §1. **Phần GTK (§4) tự chứa** — sửa
được mà không cần đọc code agent.

## 0. Vì sao

Mục tiêu: chạy `rc-agent` trên máy cắm thiết bị; ở app nhập IP Tailscale của máy đó → hiện ra
**tất cả** máy thật lẫn máy ảo, bấm vào là mở phiên, **mọi máy** đều đi được đường LAN trực tiếp.

Ba chỗ hỏng hiện tại:

1. **Máy thật không dùng được qua Tailscale.** `setup_usb_phone()` (`frontends/agent/main.c:422`)
   không relay điện thoại USB — nó đọc IP Wi-Fi *của điện thoại* rồi in ra để người dùng tự nhập.
   Điện thoại không nằm trong tailnet nên IP đó vô nghĩa với máy remote.
2. **Không có auto-register.** Agent không có cổng discovery; app chỉ có ô `adb connect ip:port`
   thủ công (`frontends/gtk/chooser.c:189`), phải chép tay từng địa chỉ từ terminal của agent.
3. **Chỉ máy đầu tiên được stream nhanh.** `main.c:634` truyền `no_stream` cho mọi máy từ thứ hai
   trở đi; các máy sau fallback adb tunnel.

## 1. Hợp đồng giữa ba tầng

```
  ┌─ agent (§3) ──────────┐      ┌─ GTK (§4) ─────────┐      ┌─ core (§2) ────────┐
  │ discovery :8888       │─────▶│ agent_scan()       │      │                    │
  │   stream_base mỗi máy │      │ registry AgentDev  │─────▶│ tcp_addr           │
  │                       │      │   chọn k theo máy  │      │   → connect tới    │
  │ relay public          │◀─────│ tcp_addr =         │      │     agent          │
  │   stream_base + k     │      │   ip:stream_base+k │      │ tcp_device_port    │
  │     ↓ adb forward     │      │ tcp_device_port =  │─────▶│   → server listen  │
  │   device 27183 + k    │◀─────│   27183 + k        │      │     trong máy      │
  └───────────────────────┘      └────────────────────┘      └────────────────────┘
```

Một câu tóm tắt: **cổng public khác cổng trong thiết bị**, và app là chỗ duy nhất biết cả hai.

## 2. `core` — tách cổng listen khỏi cổng connect

Đây là thay đổi nhỏ nhất nhưng mở khoá toàn bộ phần còn lại.

`deploy_tcp()` (`core/src/server_deploy.c:193`) đang dùng **một** số port cho hai vai trò:

```c
rc_status r = parse_addr(c->cfg.tcp_addr, host, sizeof host, &port);
...
r = rc_adb_run_server(c->cfg.serial, &c->cfg, NULL, port, ...);  /* :212 server listen TRONG máy */
...
c->video_fd = connect_retry(c, host, port, deployed ? 3000 : 1000);  /* :223 connect TỚI agent */
```

Relay nhiều máy cần hai số khác nhau (§1).

**Sửa:**

1. `core/include/rc/rc_client.h` — thêm vào `rc_config`, ngay sau `tcp_addr`:
   ```c
   int tcp_device_port; /* cổng rc-server listen TRONG thiết bị; 0 = dùng port của tcp_addr.
                         * Khác tcp_addr khi đi qua rc-agent: agent relay cổng public của
                         * tcp_addr về đúng cổng này (xem docs/AGENT_PROTOCOL.md §2.2). */
   ```
2. `core/src/server_deploy.c` trong `deploy_tcp()`:
   ```c
   int dev_port = c->cfg.tcp_device_port > 0 ? c->cfg.tcp_device_port : port;
   ```
   → truyền `dev_port` vào `rc_adb_run_server` (`:212`). **`connect_retry` giữ nguyên `port`**
   (`:223`, `:226`, `:227`).

`tcp_device_port = 0` → `dev_port == port` → hành vi cũ y hệt. Nhánh env `RC_TCP_ADDR` và mọi
caller cũ không phải đổi gì.

## 3. `frontends/agent/main.c` — bind mọi thiết bị + discovery + rescan

### 3.1 Bảng slot

Thay biến `nemu` đếm tay (`:631`) bằng bảng slot (`docs/AGENT_PROTOCOL.md` §0.1):

```c
#define MAX_SLOTS 8            /* adb 15553-15560, stream 27183-27214 */
#define DEFAULT_DISCOVERY_PORT 8888

typedef struct {
    char serial[128], model[96];
    const char *kind;   /* "emulator" | "USB" | "wireless" */
    int idx, adb_port, stream_base;
    int present;        /* còn thấy trong `adb devices` không → có liệt kê ở discovery không */
    int ready;          /* forward + listener đã dựng xong chưa */
} Slot;
```

Slot gán theo serial, **giữ suốt đời process, không tái dụng**. Nhờ vậy không bao giờ phải gỡ
listener đang chạy — nguồn race lớn nhất khi có rescan nền. Vượt `MAX_SLOTS` → log rõ, không im
lặng bỏ máy.

### 3.2 `setup_device()` — gộp hai đường thành một

Bỏ `setup_usb_phone()` (`:422`), đổi tên `setup_emulator()` (`:516`) thành `setup_device()`. Logic
emulator hiện tại **đã đúng cho cả USB**, giữ nguyên vòng:

> forward → `verify_guest_adb` → hỏng thì `adb tcpip` + sleep 2s → forward lại → verify lại

`adb tcpip 5555` restart adbd làm USB rớt 1–2s rồi vào lại; vòng trên chính là chỗ xử lý việc đó,
**đừng làm đứt nó khi gộp hàm**.

Ba nhánh:

| kind | với tới adbd bằng | relay |
|---|---|---|
| emulator, USB | `adb forward tcp:(adb_port+10000) tcp:5555` | `adb_port` → `127.0.0.1:(adb_port+10000)` |
| wireless | không cần | `adb_port` → `<ip>:<port>` lấy từ serial |

Nhánh wireless cần đích **không phải localhost** → thêm `char dst_host[64]` vào `Listener` và
`Conn`; `conn_thread` (`:307`) và `stream_open_upstream` (`:252`) đang hardcode `"127.0.0.1"`.

Stream, **cho mọi máy** (không còn `idx > 0` → `no_stream`):

```c
for (int k = 0; k < STREAM_COUNT; k++) {
    adb_forward(serial, slot->stream_base + k + LOC_OFFSET, STREAM_BASE + k);
    add_listener(slot->stream_base + k, slot->stream_base + k + LOC_OFFSET, group_cho_k);
}
```

### 3.3 Group: toàn cục → một cho mỗi cổng stream

`g_stream_group` (`:476`) hiện là **một** group cho mọi cổng stream, khiến chúng serialize với
nhau. Đổi thành **một `Group` cho mỗi listener stream**.

Lý do việc serialize tồn tại (xem comment `:206`): khi phải reconnect + replay token, hai kết nối
cùng vào guest sẽ lệch kênh vì server accept theo thứ tự video→audio→control. Nhưng thứ tự đó chỉ
có nghĩa **trong cùng một cổng** — mỗi phiên là một server socket riêng. Hai cổng khác nhau không
thể lệch kênh của nhau, nên group toàn cục vừa thừa vừa làm chậm khi nhiều máy chạy song song.

### 3.4 Discovery

Thêm loại listener mới (`Listener.kind = LISTENER_DISCOVERY`), `add_listener(8888, ...)` trên mọi
bind IP. Trong vòng accept (`:654`): loại này → sinh đáp ứng **dưới lock** rồi `write` + `close`
ngay tại chỗ, không cần thread — đáp ứng vài trăm byte, luôn vừa socket buffer nên `write` không
block.

Chỉ liệt kê slot có `present && ready`.

### 3.5 Rescan

Thread nền, lặp mỗi ~3s: `list_devices()` (`:340`) → serial mới thì cấp slot + `setup_device()`;
serial biến mất thì `present = 0`. Không đụng tới listener đã tạo. `adb devices` lỗi tạm thời → bỏ
qua vòng đó, đừng xoá sạch bảng.

### 3.6 Khoá

Một mutex bảo vệ bảng slot + `g_ls`/`g_nls`. Vòng accept đã dựng lại mảng `pollfd` mỗi vòng
(`:649`) nên chỉ cần lock lúc snapshot fd và lúc dispatch. Mảng listener **chỉ lớn thêm, không bao
giờ co lại** (hệ quả của §3.1) → không có use-after-free.

### 3.7 Linh tinh

- **Bind loopback**: `host_ips()` (`:402`) đang bỏ `lo` qua `iface_skipped()`. Append `127.0.0.1`
  vào `g_bind_ips` cho đúng đặc tả §0. Vẫn tuyệt đối **không** bind `0.0.0.0`.
- **Vòng đời**: bỏ `if (g_nls == 0) return 0;` (`:642`) và nhánh thoát sớm khi `ndev == 0`
  (`:609`) — agent giờ luôn phải sống để giữ cổng discovery, kể cả khi chưa cắm máy nào.
- `--no-stream` → mọi slot có `stream_base = 0`.
- Cờ mới `--port` cho cổng discovery.

## 4. `frontends/gtk` — auto-register (phần tự chứa)

> Đọc `docs/AGENT_PROTOCOL.md` §1 và §2.2 trước. Không cần đọc code agent: agent chỉ là một cổng
> TCP trả về text theo đúng §1.2, và mọi cổng nó nói tới đều đã nằm trong đáp ứng đó.

### 4.1 `agent.c` (file mới) + `CMakeLists.txt`

Tách module theo đúng kiểu một-file-một-nhiệm-vụ đã ghi ở đầu `rcgtk.h`. Thêm vào
`frontends/gtk/CMakeLists.txt`.

```c
typedef struct {
    char serial[128], model[96], kind[16];
    int adb_port, stream_base;
} AgentDev;

/* TCP tới ip:port, KHÔNG gửi gì, đọc tới EOF, parse theo AGENT_PROTOCOL §1.2.
 * Trả FALSE + set err nếu banner sai hoặc version lạ. */
gboolean agent_scan(const char *ip, int port, GPtrArray **out, GError **err);
```

Bắt buộc theo §3 của đặc tả:
- dòng đầu không khớp `RCAGENT ` → **từ chối** (chống nhập nhầm cổng dịch vụ khác);
- `version` lạ → **từ chối, không đoán**;
- dòng parse lỗi → **bỏ dòng đó**, không bỏ cả đáp ứng;
- trường thừa chưa hiểu → bỏ qua, không coi là lỗi.

Registry trong `App` (`rcgtk.h:69`): `GHashTable *agent_devs`, khoá `"<ip>:<adb_port>"` (đúng
serial mà `adb devices` sẽ báo) → `AgentDev*`.

### 4.2 `chooser.c` — thay ô Wi-Fi bằng quét agent

- Bỏ `wifi_add_thread` (`:189`), `wifi_add` (`:212`), `on_wifi_activate`/`on_wifi_clicked`.
  **Giữ** helper `adb_run` (`:135`) — vẫn cần, và nó đã xử lý đúng vụ timeout/SIGKILL.
- Thread quét: `agent_scan()` → với **mỗi** máy trả về: `adb connect <ip>:<adb_port>` rồi xác minh
  bằng `get-state`. Xác minh là bắt buộc: `adb connect` **exit 0 kể cả khi thất bại** (bẫy này đã
  ghi ở comment `:187`). → nạp registry → `populate_devices`.
- **Không push `rc-server` lúc quét.** Core tự push trong `deploy_tcp`
  (`core/src/server_deploy.c:206`) khi mở phiên; push N máy qua tailnet lúc quét chỉ làm chậm mà
  không thêm bảo đảm gì. (Khác hành vi `wifi_add_thread` cũ — có chủ đích.)
- Entry: placeholder `100.x.y.z (IP agent)`, nút "Quét agent". Không có `:` → cổng 8888.
- `conn_label` (`:29`): tra registry trước → `"Máy ảo (agent)"` / `"USB (agent)"` /
  `"LAN (agent)"`; không có trong registry → giữ nguyên nhánh cũ (USB cắm thẳng vào máy này).
- `on_row_activated` (`:105`): tra registry:
  - `stream_base > 0` → LAN trực tiếp (§4.3);
  - `stream_base == 0` → truyền `tcp_addr = NULL` → **đi thẳng adb tunnel**, không phí vài giây
    retry rồi mới fallback;
  - không có trong registry → giữ nguyên nhánh cũ.

### 4.3 `session.c` — cấp cổng stream theo thiết bị

`alloc_lan_port()` (`:16`) đang cấp **toàn cục** từ 27183 — đúng khi chỉ một máy được relay
stream, sai khi mỗi máy có dải riêng.

Thay bằng: chọn `k` nhỏ nhất trong `0..STREAM_COUNT-1` mà **chưa phiên đang sống nào của cùng
serial** đang dùng (bỏ qua phiên `torn` — server trên máy đã nhận SIGTERM, cổng đã giải phóng;
lý do này giữ nguyên từ comment `:12`). Rồi:

```c
s->cfg.tcp_addr        = "<ip-agent>:<stream_base + k>";  /* client connect tới */
s->cfg.tcp_device_port = RC_LAN_BASE_PORT + k;            /* server listen trong máy */
```

Thêm `int stream_k` vào `Session` (`rcgtk.h:28`) để so khi cấp. Hết 4 slot `k` → không cấp được →
`tcp_addr = NULL` → adb tunnel.

Nhánh env `RC_TCP_ADDR` có port sẵn (`:148`) **giữ nguyên**: `tcp_device_port = 0` → core dùng
port của `tcp_addr` cho cả hai vai → hành vi cũ.

## 5. Verify

1. `make agent` (macOS tự tắt GTK/core); GTK build trên Linux.
2. Cắm **một máy thật qua USB** + `./start-emulators.sh` 1–2 máy ảo →
   `./build/frontends/agent/rc-agent`.
3. **Kiểm tra giao thức trước khi động tới UI** — `nc <ip-tailscale> 8888`: banner `RCAGENT 1`,
   **một dòng mỗi máy** (cả USB lẫn emulator), 5 trường TAB, `stream_base` tăng bội số 4 từ 27183.
4. **Hot-plug**: rút điện thoại → `nc` lại, dòng đó biến mất; cắm lại → hiện lại **đúng cổng cũ**
   (§0.1). Cắm máy thứ hai lúc agent đang chạy → xuất hiện trong ~3s, không restart.
5. Máy khác trong tailnet: GTK → nhập IP Tailscale → Quét → danh sách có cả máy thật lẫn máy ảo,
   nhãn `(agent)` đúng loại.
6. Mở phiên **máy thật** trước (đây là thứ đang hỏng): tiêu đề cửa sổ phải là **"LAN trực tiếp"**.
   Ghi "adb tunnel" = relay stream chưa đúng.
7. Mở **đồng thời** 2–3 máy: **mọi** cửa sổ báo "LAN trực tiếp" — đây chính là điều bản cũ không
   làm được.
8. Mở 2 phiên **cùng một máy** → `k` khác nhau (server listen 27183 và 27184 trong máy), không
   BindException.
9. Ctrl-C agent → `adb forward --list` trên máy agent phải sạch (`cleanup_forwards` `:382`).

## 6. Rủi ro

- Cổng discovery + cổng adb **không xác thực** (đặc tả §4). Bind thêm loopback không nới rộng phơi
  bày, nhưng phải giữ nguyên tắc không bind `0.0.0.0`.
- `MAX_SLOTS = 8` là trần mới, phải log khi vượt thay vì im lặng bỏ máy.
- `adb tcpip` restart adbd → USB rớt 1–2s. Vòng retry ở §3.2 đã xử lý; rủi ro nằm ở việc làm đứt
  nó khi gộp hàm.
