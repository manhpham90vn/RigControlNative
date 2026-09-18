#!/usr/bin/env bash
#
# Khởi động Android emulator (local) + expose cổng adb ra ĐÚNG IP Tailscale (không ra LAN).
# Chạy lại được nhiều lần (tự dọn emulator cũ trước khi start).
#
#   ./start-emulators.sh           # start tất cả + mở forward Tailscale
#   ./start-emulators.sh stop      # dừng tất cả (emulator + forward)
#   ./start-emulators.sh status    # xem trạng thái 1 lần
#   ./start-emulators.sh expose    # chỉ dựng lại forward Tailscale (vd sau khi tailscale up lại)
#   ./start-emulators.sh unexpose  # đóng forward, emulator vẫn chạy (chỉ còn localhost)
#   ./start-emulators.sh authorize # nạp lại adb key vào máy ảo đang chạy
#   ./start-emulators.sh monitor   # theo dõi host + máy ảo realtime (Ctrl-C để thoát)
#
# Vì sao cần socat: emulator LUÔN bind cổng console/adb vào 127.0.0.1 và không có cờ đổi địa chỉ
# bind. Muốn máy khác trong tailnet `adb connect` được thì phải có một listener trên IP Tailscale
# nối ngược về loopback. Bind đích danh IP 100.x (không phải 0.0.0.0) để LAN không chạm tới adb.
#
set -euo pipefail

# ─── Cấu hình ────────────────────────────────────────────────────────────────
SDK="$HOME/Library/Android/sdk"
EMULATOR="$SDK/emulator/emulator"
ADB="$SDK/platform-tools/adb"
LOG_DIR="$HOME/.emulator-logs"

# "AVD:console_port:cores:mem_MB"   -> ADB port = console+1
EMULATORS=(
  "Small_Tablet:5552:1:1536"     # 1 core / 1.5 GB
  "Small_Tablet_2:5554:1:1536"   # 1 core / 1.5 GB
  "Small_Tablet_3:5556:1:1536"   # 1 core / 1.5 GB
  "Small_Tablet_4:5558:1:1536"   # 1 core / 1.5 GB
  "Small_Tablet_5:5560:1:1536"   # 1 core / 1.5 GB
)

GPU="host"          # máy có màn hình -> "host"; server headless -> "swiftshader_indirect"
EXTRA_FLAGS="-no-snapshot-load -no-boot-anim"

# Public key của các máy remote muốn adb connect (mỗi key 1 dòng, giống ~/.android/adbkey.pub).
# Key của chính host này luôn được tự thêm vào.
AUTH_KEYS_FILE="$HOME/authorized_adb_keys"

# Forward Tailscale: mỗi emulator một socat bind vào IP 100.x.
# Cổng public = console_port + TS_PORT_OFFSET  (5552 -> 15552, 5554 -> 15554), trỏ về adb port
# loopback tương ứng (console+1). Dùng dải riêng cho dễ nhớ và khỏi lẫn với cổng adb local.
# Đặt TS_IP=... để ép IP cụ thể (máy nhiều tailnet / test).
TS_PORT_OFFSET=10000
FWD_RETRY=5                       # giây chờ trước khi supervisor dựng lại socat đã chết
FWD_PID_DIR="$LOG_DIR/forwards"
# ─────────────────────────────────────────────────────────────────────────────

mkdir -p "$LOG_DIR" "$FWD_PID_DIR"

# ─── Timeout helper (macOS không có sẵn `timeout`) ───────────────────────────
# run_to SECS cmd... : chạy cmd trong nền, tự kill nếu vượt quá SECS giây.
# Trả về 124 khi bị timeout (giống lệnh `timeout`), ngược lại trả mã của cmd.
run_to() {
  local secs="$1"; shift
  "$@" &
  local cmd_pid=$!
  ( sleep "$secs"; kill -TERM "$cmd_pid" 2>/dev/null
    sleep 2;      kill -KILL "$cmd_pid" 2>/dev/null ) >/dev/null 2>&1 &
  local watch_pid=$!
  local rc=0
  wait "$cmd_pid" 2>/dev/null || rc=$?
  # cmd xong sớm -> dọn watchdog.
  kill "$watch_pid" 2>/dev/null || true
  wait "$watch_pid" 2>/dev/null || true
  # bị SIGTERM(143)/SIGKILL(137) do watchdog -> quy về 124 (timeout).
  [[ $rc -eq 143 || $rc -eq 137 ]] && rc=124
  return "$rc"
}

ADB_T=20   # timeout (giây) mặc định cho mỗi lệnh adb dễ treo

# ─── Tailscale ───────────────────────────────────────────────────────────────
# IP Tailscale v4 của máy này. Ưu tiên CLI; nếu không có thì quét interface tìm địa chỉ trong dải
# CGNAT 100.64.0.0/10 (dải Tailscale cấp phát). In ra stdout, rỗng nếu không tìm thấy.
tailscale_ip() {
  if [[ -n "${TS_IP:-}" ]]; then echo "$TS_IP"; return 0; fi

  local bin
  for bin in /usr/local/bin/tailscale /opt/homebrew/bin/tailscale \
             "/Applications/Tailscale.app/Contents/MacOS/Tailscale"; do
    if [[ -x "$bin" ]]; then
      local ip; ip="$("$bin" ip -4 2>/dev/null | head -1 | tr -d '[:space:]')"
      [[ -n "$ip" ]] && { echo "$ip"; return 0; }
      break
    fi
  done

  ifconfig 2>/dev/null | awk '
    $1 == "inet" && $2 ~ /^100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\./ { print $2; exit }'
}

# Đóng mọi forward đang chạy (theo pid file). Kill supervisor -> nó tự kill socat con;
# thêm một nhát pkill theo cổng để quét cả socat mồ côi (supervisor từng bị SIGKILL).
unexpose() {
  local found=0 f pid port
  for f in "$FWD_PID_DIR"/*.pid; do
    [[ -e "$f" ]] || continue
    port="$(basename "$f" .pid)"
    pid="$(cat "$f" 2>/dev/null || true)"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      echo "⏹  Đóng forward cổng $port (pid $pid)"
      found=1
    fi
    rm -f "$f"
    pkill -f "TCP4-LISTEN:$port,bind=" 2>/dev/null || true
  done
  (( found == 0 )) && echo "   (không có forward Tailscale nào đang chạy)"
  return 0
}

# Vòng giữ forward sống cho MỘT emulator. socat chết vì bất kỳ lý do gì (crash, tailscale down,
# IP tailnet đổi, cổng bị chiếm) -> ngủ FWD_RETRY giây rồi dựng lại, và tra lại IP mỗi vòng nên
# tailscale up/down xong là tự lành, không cần chạy lại script.
fwd_supervise() {
  local avd="$1" pub="$2" loc="$3"
  local child="" ip=""
  trap '[[ -n "$child" ]] && kill "$child" 2>/dev/null; exit 0' TERM INT
  while true; do
    ip="$(tailscale_ip)"
    if [[ -z "$ip" ]]; then
      echo "$(date '+%F %H:%M:%S') $avd: chưa có IP Tailscale, thử lại sau ${FWD_RETRY}s."
      sleep "$FWD_RETRY"; continue
    fi
    echo "$(date '+%F %H:%M:%S') $avd: listen $ip:$pub → 127.0.0.1:$loc"
    socat "TCP4-LISTEN:$pub,bind=$ip,reuseaddr,fork" "TCP4:127.0.0.1:$loc" &
    child=$!
    wait "$child" 2>/dev/null || true
    child=""
    echo "$(date '+%F %H:%M:%S') $avd: socat thoát — dựng lại sau ${FWD_RETRY}s."
    sleep "$FWD_RETRY"
  done
}

# Dựng forward TS_IP:<console+OFFSET> -> 127.0.0.1:<console+1> cho từng emulator, mỗi cái một
# supervisor tự hồi phục. Pid ghi ra FWD_PID_DIR là pid supervisor, không phải pid socat.
expose() {
  if ! command -v socat >/dev/null 2>&1; then
    echo "⚠️  Thiếu socat -> không expose được. Cài: brew install socat"
    return 0
  fi
  local ip; ip="$(tailscale_ip)"
  [[ -z "$ip" ]] && echo "⚠️  Chưa thấy IP Tailscale (tailscale down?) — supervisor vẫn chạy và sẽ tự bind khi có mạng."

  unexpose >/dev/null
  echo "🌐 Expose qua Tailscale (${ip:-chờ IP}):"
  for e in "${EMULATORS[@]}"; do
    IFS=: read -r avd port _ _ <<<"$e"
    local adb_port=$((port + 1))
    local pub_port=$((port + TS_PORT_OFFSET))
    # trap '' HUP: sống tiếp sau khi script thoát / đóng terminal.
    ( trap '' HUP; fwd_supervise "$avd" "$pub_port" "$adb_port" ) \
      >>"$LOG_DIR/forward-$pub_port.log" 2>&1 &
    local pid=$!
    disown
    echo "$pid" >"$FWD_PID_DIR/$pub_port.pid"
    sleep 0.4
    if lsof -nP -iTCP:"$pub_port" -sTCP:LISTEN >/dev/null 2>&1; then
      printf "   ✓ %-26s %s:%s → 127.0.0.1:%s (supervisor %s)\n" "$avd" "$ip" "$pub_port" "$adb_port" "$pid"
    elif kill -0 "$pid" 2>/dev/null; then
      printf "   … %-26s chưa bind được :%s — supervisor %s đang thử lại (%s)\n" \
        "$avd" "$pub_port" "$pid" "$LOG_DIR/forward-$pub_port.log"
    else
      echo "   ✗ $avd: supervisor chết ngay, xem $LOG_DIR/forward-$pub_port.log"
    fi
  done
  echo
  echo "   Máy remote trong tailnet:"
  for e in "${EMULATORS[@]}"; do
    IFS=: read -r avd port _ _ <<<"$e"
    echo "     adb connect ${ip:-<ip-tailscale>}:$((port + TS_PORT_OFFSET))   # $avd"
  done
}

stop_all() {
  unexpose
  for e in "${EMULATORS[@]}"; do
    IFS=: read -r avd port _ _ <<<"$e"
    echo "⏹  Dừng emulator $avd (emulator-$port)..."
    run_to 10 "$ADB" -s "emulator-$port" emu kill 2>/dev/null || true
  done
}

status() {
  echo "=== Emulator đang chạy ==="
  ps aux | grep -E "qemu-system" | grep -v grep || echo "(không có)"
  echo "=== adb devices ==="
  "$ADB" devices
  echo "=== Forward Tailscale ==="
  local ip; ip="$(tailscale_ip)"
  echo "IP Tailscale: ${ip:-(không có)}"
  local f pid port shown=0
  for f in "$FWD_PID_DIR"/*.pid; do
    [[ -e "$f" ]] || continue
    port="$(basename "$f" .pid)"; pid="$(cat "$f" 2>/dev/null || true)"
    local dst="127.0.0.1:$((port - TS_PORT_OFFSET + 1))"
    if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
      echo "  :$port → $dst  ✗ supervisor chết (pid file mồ côi)"
    elif lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
      echo "  ${ip:-?}:$port → $dst  ✓ (supervisor $pid)"
    else
      echo "  :$port → $dst  … chưa bind, supervisor $pid đang thử lại"
    fi
    shown=1
  done
  (( shown == 0 )) && echo "  (không có)"
  return 0
}

monitor() {
  local interval="${1:-2}"
  local ncpu; ncpu="$(sysctl -n hw.ncpu)"
  trap 'echo; echo "↩  Thoát monitor."; exit 0' INT
  while true; do
    clear
    printf "📊 EMULATOR MONITOR  —  %s  (refresh %ss, Ctrl-C để thoát)\n" "$(date '+%H:%M:%S')" "$interval"
    printf '%.0s─' {1..70}; echo

    # ── Host ──
    local load mem_free swap
    load="$(uptime | sed 's/.*load averages*: //')"
    mem_free="$(memory_pressure 2>/dev/null | awk -F': ' '/free percentage/{print $2}')"
    swap="$(sysctl -n vm.swapusage | sed 's/total = //')"
    printf "HOST (%s core)\n" "$ncpu"
    printf "  Load avg   : %s   (≈%d%% nếu chia /%s core)\n" \
      "$load" "$(echo "$load $ncpu" | awk '{printf "%d", $1/$NF*100}')" "$ncpu"
    printf "  RAM free   : %s\n" "${mem_free:-n/a}"
    printf "  Swap       : %s\n" "$swap"
    printf '%.0s─' {1..70}; echo

    # ── Per emulator ──
    printf "%-26s %4s  %7s  %8s  %6s\n" "MÁY ẢO" "CORE" "CPU%" "RSS(MB)" "BOOT"
    for e in "${EMULATORS[@]}"; do
      IFS=: read -r avd port cores _ <<<"$e"
      local row pid cpu rss boot
      row="$(ps aux | grep -E "qemu-system.* -port $port" | grep -v grep | head -1)"
      if [[ -z "$row" ]]; then
        printf "%-26s %4s  %7s  %8s  %6s\n" "$avd" "$cores" "—" "—" "OFF"
        continue
      fi
      pid="$(echo "$row"  | awk '{print $2}')"
      cpu="$(echo "$row"  | awk '{print $3}')"
      rss="$(echo "$row"  | awk '{printf "%.0f", $6/1024}')"
      boot="$("$ADB" -s "emulator-$port" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')"
      [[ "$boot" == "1" ]] && boot="✓" || boot="…"
      printf "%-26s %4s  %6s%%  %8s  %6s   (pid %s)\n" "$avd" "$cores" "$cpu" "$rss" "$boot" "$pid"
    done
    sleep "$interval"
  done
}

# Nạp sẵn public key vào emulator để ADB không hỏi "Allow USB debugging?" nữa.
# Key nằm ở /data/misc/adb/adb_keys (persistent qua reboot vì thuộc userdata).
authorize() {
  # Gộp key của host + file key remote (nếu có) thành 1 file tạm.
  local tmp; tmp="$(mktemp)"
  cat "$HOME/.android/adbkey.pub" >"$tmp" 2>/dev/null || true
  [[ -f "$AUTH_KEYS_FILE" ]] && cat "$AUTH_KEYS_FILE" >>"$tmp"
  # Bỏ dòng trống / trùng lặp.
  sort -u "$tmp" | grep -v '^[[:space:]]*$' >"$tmp.clean" && mv "$tmp.clean" "$tmp"

  if [[ ! -s "$tmp" ]]; then
    echo "⚠️  Không tìm thấy public key nào để nạp (thiếu ~/.android/adbkey.pub và $AUTH_KEYS_FILE)."
    rm -f "$tmp"; return
  fi

  for e in "${EMULATORS[@]}"; do
    IFS=: read -r avd port _ _ <<<"$e"
    local dev="emulator-$port"
    # Ảnh đã tắt xác thực adb (ro.adb.secure=0, hay gặp ở ảnh userdebug/AOSP):
    # mọi máy connect tự do, không cần popup/key -> bỏ qua hẳn, khỏi authen.
    local secure
    secure="$(run_to 10 "$ADB" -s "$dev" shell getprop ro.adb.secure 2>/dev/null | tr -d '\r ')"
    if [[ "$secure" == "0" ]]; then
      echo "🔓 $avd: ro.adb.secure=0 -> adb không cần xác thực, bỏ qua nạp key."
      continue
    fi
    echo "🔑 Nạp $(grep -c . "$tmp") key vào $avd ($dev)..."
    if ! run_to "$ADB_T" "$ADB" -s "$dev" root >/dev/null 2>&1; then
      echo "   ⚠️  '$avd' không phản hồi 'adb root' (offline/kẹt, hoặc ảnh Google Play). Bỏ qua."
      continue
    fi
    run_to "$ADB_T" "$ADB" -s "$dev" wait-for-device 2>/dev/null || true
    run_to "$ADB_T" "$ADB" -s "$dev" shell 'mkdir -p /data/misc/adb' 2>/dev/null || true
    if run_to 30 "$ADB" -s "$dev" push "$tmp" /data/misc/adb/adb_keys >/dev/null 2>&1; then
      run_to "$ADB_T" "$ADB" -s "$dev" shell 'chown shell:shell /data/misc/adb/adb_keys; chmod 640 /data/misc/adb/adb_keys; restorecon /data/misc/adb/adb_keys 2>/dev/null; setprop ctl.restart adbd' 2>/dev/null || true
      echo "   ✓ đã nạp, restart adbd."
    else
      echo "   ⚠️  push adb_keys thất bại (device offline?)."
    fi
    run_to "$ADB_T" "$ADB" -s "$dev" unroot >/dev/null 2>&1 || true
  done
  rm -f "$tmp"
}

start_one() {
  local avd="$1" port="$2" cores="$3" mem="$4"
  local adb_port=$((port + 1))

  # Emulator đã chạy chưa?
  if pgrep -f "qemu-system.* -port $port" >/dev/null; then
    echo "✓  $avd (port $port) đã chạy, bỏ qua start."
  else
    echo "▶  Start $avd  | $cores core / ${mem}MB | console=$port adb=$adb_port"
    nohup "$EMULATOR" -avd "$avd" \
      -cores "$cores" -memory "$mem" \
      -gpu "$GPU" $EXTRA_FLAGS -port "$port" \
      >"$LOG_DIR/$avd.log" 2>&1 &
    disown
  fi
}

wait_boot() {
  for e in "${EMULATORS[@]}"; do
    IFS=: read -r avd port _ _ <<<"$e"
    echo -n "⏳ Chờ $avd boot... "
    run_to "$ADB_T" "$ADB" -s "emulator-$port" wait-for-device 2>/dev/null || true
    local tries=0 ok=0
    while (( tries < 90 )); do   # tối đa ~3 phút (90 x 2s)
      if [[ "$(run_to 10 "$ADB" -s "emulator-$port" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == "1" ]]; then
        ok=1; break
      fi
      tries=$((tries + 1)); sleep 2
    done
    (( ok == 1 )) && echo "✓ done" || echo "✗ timeout (chưa boot xong sau ~3 phút)"
  done
}

case "${1:-start}" in
  stop)      stop_all ;;
  status)    status ;;
  monitor)   monitor "${2:-2}" ;;
  authorize) authorize ;;
  expose)    expose ;;
  unexpose)  unexpose ;;
  start)
    "$ADB" start-server >/dev/null 2>&1 || true
    for e in "${EMULATORS[@]}"; do
      IFS=: read -r avd port cores mem <<<"$e"
      start_one "$avd" "$port" "$cores" "$mem"
    done
    wait_boot
    echo
    authorize
    echo
    expose
    echo
    status
    echo
    echo "👉 Cổng adb đã mở trên IP Tailscale (chỉ tailnet, không ra LAN)."
    ;;
  *) echo "Dùng: $0 [start|stop|status|expose|unexpose|authorize|monitor [giây]]"; exit 1 ;;
esac
