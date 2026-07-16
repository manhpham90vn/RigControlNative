# rc-server (Android)

Server chạy trên thiết bị Android: capture màn hình → encode H.264 → gửi qua socket, và nhận
input inject từ desktop. **Không phải APK** — chạy trực tiếp qua `app_process` (như scrcpy),
build bằng `javac` + `d8`, không cần Gradle/AGP.

## Build

Yêu cầu: `javac` (JDK 11+), Android SDK có `platforms/android-*/android.jar` và
`build-tools/*/d8`. Mặc định tìm ở `$ANDROID_HOME`, `$ANDROID_SDK_ROOT`, hoặc `~/Android/Sdk`.

```bash
./build.sh          # → ./rc-server (jar chứa classes.dex)
```

## Chạy thử thủ công (Phase 1)

Chưa cần desktop `libcore` — có thể test bằng `adb forward` + `nc` để hứng byte thô.

USB (localabstract, một socket video cho đơn giản — tắt audio/control):

```bash
adb push rc-server /data/local/tmp/rc-server
# desktop reverse tunnel + listen TRƯỚC khi chạy server
adb reverse localabstract:rigcontrol tcp:27183
nc -l 127.0.0.1 27183 > dump.h264 &    # hứng device_meta + video_packet

adb shell CLASSPATH=/data/local/tmp/rc-server \
    app_process / com.rigcontrol.server.Server \
    max_size=1280 bit_rate=8000000 max_fps=60 codec=h264 audio=false control=false tcp=false
```

Bật thêm audio (`audio=true`) hoặc control (`control=true`) thì phải mở đúng **số socket theo
thứ tự** video → audio → control (mỗi socket một `nc -l` reverse cổng khác nhau).

TCP/LAN (server tự listen, không cần reverse):

```bash
adb shell CLASSPATH=/data/local/tmp/rc-server \
    app_process / com.rigcontrol.server.Server \
    max_size=1280 bit_rate=8000000 audio=false control=false tcp=true port=27183
# rồi từ desktop: nc <device-ip> 27183 > dump.h264
```

Kiểm tra nhanh: `dump.h264` mở đầu bằng `device_meta` (magic `RCN1`) rồi tới các `video_packet`;
loại 78 byte header meta ra là có luồng H.264 Annex-B để `ffplay` thử.

## Cấu trúc

| File | Vai trò |
|------|---------|
| `Server.java` | entry `main`, parse tham số, điều phối |
| `Options.java` | parse `key=value` từ dòng lệnh |
| `Protocol.java` | hằng số giao thức (khớp `docs/PROTOCOL.md` + `core`) |
| `DesktopConnection.java` | mở video/control socket, gửi `device_meta` |
| `ScreenEncoder.java` | SurfaceControl + VirtualDisplay + MediaCodec H.264 |
| `AudioEncoder.java` | AudioRecord `REMOTE_SUBMIX` + MediaCodec Opus |
| `Workarounds.java` | dựng app context giả để AudioRecord chạy được từ app_process |
| `ControlReceiver.java` | đọc control, inject qua InputManager (reflection, Phase 4) |
| `DeviceController.java` | DEVICE_ACTION: screen off/on, panel, xoay (reflection) |
| `wrappers/` | reflection hidden API: `SurfaceControl`, `DisplayManager`, `DisplayInfo`, `InputManager`, `ServiceManager` |

`ControlReceiver` inject chuột→cảm ứng / cuộn / phím / text qua `InputManager` (Android 14+ dùng
`InputManagerGlobal`); `DeviceController` xoay qua `IWindowManager.freezeRotation`, screen power qua
`SurfaceControl.setDisplayPowerMode`, panel qua `IStatusBarService`.
