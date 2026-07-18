#!/usr/bin/env bash
#
# build-mac.sh — dựng libcore (CMake) rồi biên dịch front-end SwiftUI thành RigControlNative.app.
# Không cần Xcode đầy đủ: dùng swiftc của Command Line Tools + SDK macOS (SwiftUI/Metal có sẵn).
#
# Yêu cầu: Xcode Command Line Tools, CMake, Homebrew ffmpeg (brew install ffmpeg), adb.
# Dùng:  frontends/swiftui/build-mac.sh [run]
#
set -euo pipefail

SW="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SW/../.." && pwd)"
BUILD="$ROOT/build-mac"
APP="$BUILD/RigControlNative.app"
ARCH="$(uname -m)"
# Deployment target = phiên bản macOS hiện tại (khớp dylib ffmpeg của Homebrew → hết cảnh báo lệch
# phiên bản khi link). Có thể ép bằng biến MACOS_DEPLOYMENT_TARGET nếu muốn build cho OS cũ hơn.
MACOS_TARGET="${MACOS_DEPLOYMENT_TARGET:-$(sw_vers -productVersion | cut -d. -f1).0}"

echo "==> [1/4] Dựng libcore (CMake, không GTK/agent) — macOS $MACOS_TARGET"
cmake -B "$BUILD" -S "$ROOT" -G "Unix Makefiles" \
    -DRC_BUILD_GTK=OFF -DRC_BUILD_AGENT=OFF -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_TARGET" >/dev/null
cmake --build "$BUILD" --target rccore

# FFmpeg: mặc định lấy từ Homebrew; workflow release ép FFMPEG_PREFIX trỏ tới bản FFmpeg tối
# giản tự build (chỉ decoder cần) để bundle vào .app cho gọn — xem .github/workflows/release.yml.
FFMPEG_PREFIX="${FFMPEG_PREFIX:-$(brew --prefix ffmpeg)}"
echo "==> FFmpeg: $FFMPEG_PREFIX"

echo "==> [2/4] Biên dịch Swift → executable ($ARCH)"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
swiftc \
    -O \
    -target "${ARCH}-apple-macos${MACOS_TARGET}" \
    -o "$APP/Contents/MacOS/RigControlNative" \
    -I "$SW/bridge" \
    -Xcc -I"$ROOT/core/include" \
    "$SW"/Sources/*.swift \
    -L "$BUILD/core" -lrccore \
    -L "$FFMPEG_PREFIX/lib" -lavcodec -lavutil -lswresample \
    -framework CoreFoundation -framework CoreAudio -framework AudioToolbox -framework AudioUnit \
    -framework AppKit -framework Metal -framework MetalKit

echo "==> [3/4] Đóng gói .app"
cp "$SW/Info.plist" "$APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $MACOS_TARGET" \
    "$APP/Contents/Info.plist" 2>/dev/null || true
# Ad-hoc codesign (Metal/JIT một số máy yêu cầu chữ ký hợp lệ).
codesign --force --sign - "$APP" 2>/dev/null || true

echo "==> [4/4] Xong: $APP"
if [[ "${1:-}" == "run" ]]; then
    echo "==> Chạy (cấu hình qua env RC_SERIAL / RC_TCP_ADDR / RC_MAX_SIZE ...)"
    exec "$APP/Contents/MacOS/RigControlNative"
fi
