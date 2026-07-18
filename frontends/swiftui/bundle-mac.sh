#!/usr/bin/env bash
#
# bundle-mac.sh — biến RigControlNative.app (do build-mac.sh dựng) thành app SELF-CONTAINED:
#   1. Bundle mọi dylib FFmpeg (và closure của nó) vào Contents/Frameworks, đổi install-name
#      sang @rpath + thêm rpath @executable_path/../Frameworks → không phụ thuộc Homebrew.
#   2. Copy rc-server (dex) vào Contents/Resources — libcore tìm ở "../Resources/rc-server"
#      cạnh binary (core/src/server_deploy.c) nên app double-click deploy được server.
#   3. Ad-hoc codesign lại (install_name_tool làm hỏng chữ ký cũ).
#
# Dùng:  bundle-mac.sh [APP_PATH] [RC_SERVER_PATH]
# Mặc định: build-mac/RigControlNative.app  và  server/rc-server (tương đối gốc repo).
#
# Bundle nhỏ nhất khi build-mac.sh trỏ FFMPEG_PREFIX tới FFmpeg tối giản (chỉ ~4 dylib);
# với Homebrew ffmpeg full thì closure lớn hơn (x264/x265/openssl...) nhưng vẫn chạy.
# bash 3.2 tương thích (runner macOS mặc định /bin/bash 3.2 — không dùng associative array).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="${1:-$ROOT/build-mac/RigControlNative.app}"
SERVER="${2:-$ROOT/server/rc-server}"
EXE="$APP/Contents/MacOS/RigControlNative"
FW="$APP/Contents/Frameworks"
RES="$APP/Contents/Resources"

[ -x "$EXE" ] || { echo "Không thấy executable: $EXE (chạy 'make mac' trước)"; exit 1; }
[ -f "$SERVER" ] || { echo "Không thấy rc-server: $SERVER (chạy 'make server' trước)"; exit 1; }
mkdir -p "$FW" "$RES"

echo "==> [1/3] Bundle FFmpeg + relink @rpath"
seen=" "        # danh sách basename đã bundle (space-delimited, thay cho associative array)
queue="$EXE"    # hàng đợi Mach-O cần quét, phân tách bằng '|'
while [ -n "$queue" ]; do
    bin="${queue%%|*}"
    if [ "$queue" = "$bin" ]; then queue=""; else queue="${queue#*|}"; fi
    [ -n "$bin" ] || continue
    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        case "$dep" in
            /usr/lib/*|/System/*|@rpath/*|@loader_path/*|@executable_path/*) continue ;;
        esac
        base="$(basename "$dep")"
        case "$seen" in
            *" $base "*) : ;;   # đã bundle rồi
            *)
                seen="$seen$base "
                cp -L "$dep" "$FW/$base"; chmod u+w "$FW/$base"
                install_name_tool -id "@rpath/$base" "$FW/$base"
                queue="$queue|$FW/$base"
                ;;
        esac
        install_name_tool -change "$dep" "@rpath/$base" "$bin"
    done < <(otool -L "$bin" | tail -n +2 | awk '{print $1}')
done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXE" 2>/dev/null || true
echo "    bundled $(ls "$FW" | wc -l | tr -d ' ') dylib"

# Không được còn dependency tuyệt đối ngoài hệ thống (chỉ cho phép /usr/lib, /System và
# @rpath/@loader_path/@executable_path). LC_RPATH thừa thì vô hại nên chỉ soi LC_LOAD_DYLIB.
bad=0
for f in "$FW"/*.dylib "$EXE"; do
    leftover="$(otool -L "$f" | tail -n +2 | awk '{print $1}' \
        | grep -E '^/' | grep -vE '^(/usr/lib/|/System/)' || true)"
    if [ -n "$leftover" ]; then
        echo "    LỖI: $(basename "$f") còn dependency chưa relink:"; echo "$leftover" | sed 's/^/      /'
        bad=1
    fi
done
[ "$bad" = 0 ] || exit 1

echo "==> [2/3] Copy rc-server vào Contents/Resources"
cp "$SERVER" "$RES/rc-server"

echo "==> [3/3] Ad-hoc codesign"
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"
echo "==> Self-contained: $APP"
