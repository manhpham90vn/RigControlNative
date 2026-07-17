#!/usr/bin/env bash
#
# Khởi động Android emulator (local). Expose ra LAN/Tailscale bằng rc-agent, không dùng socat.
# Chạy lại được nhiều lần (tự dọn emulator cũ trước khi start).
#
#   ./start-emulators.sh           # start tất cả (tự nạp adb key, khỏi popup Allow)
#   ./start-emulators.sh stop      # dừng tất cả (emulator)
#   ./start-emulators.sh status    # xem trạng thái 1 lần
#   ./start-emulators.sh authorize # nạp lại adb key vào máy ảo đang chạy
#   ./start-emulators.sh monitor   # theo dõi host + máy ảo realtime (Ctrl-C để thoát)
#
set -euo pipefail

# ─── Cấu hình ────────────────────────────────────────────────────────────────
SDK="$HOME/Library/Android/sdk"
EMULATOR="$SDK/emulator/emulator"
ADB="$SDK/platform-tools/adb"
LOG_DIR="$HOME/.emulator-logs"

# "AVD:console_port:cores:mem_MB"   -> ADB port = console+1
EMULATORS=(
  "Resizable_Experimental:5552:4:6144"       # 4 core / 6 GB
  "Resizable_Experimental_2:5554:3:5120"     # 3 core / 5 GB
)

GPU="host"          # máy có màn hình -> "host"; server headless -> "swiftshader_indirect"
EXTRA_FLAGS="-no-snapshot-load -no-boot-anim"

# Public key của các máy remote muốn adb connect (mỗi key 1 dòng, giống ~/.android/adbkey.pub).
# Key của chính host này luôn được tự thêm vào.
AUTH_KEYS_FILE="$HOME/authorized_adb_keys"
# ─────────────────────────────────────────────────────────────────────────────

mkdir -p "$LOG_DIR"

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

stop_all() {
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
    status
    echo
    echo "👉 Emulator đang chạy local. Để máy remote adb connect / dùng app,"
    echo "   chạy rc-agent trên máy này (tự forward + mở port LAN/Tailscale):"
    echo "     ./build/frontends/agent/rc-agent"
    ;;
  *) echo "Dùng: $0 [start|stop|status|authorize|monitor [giây]]"; exit 1 ;;
esac
