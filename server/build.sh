#!/usr/bin/env bash
#
# Build rc-server (Java thuần -> dex) không cần Gradle/AGP.
# Sinh ra ./rc-server (jar chứa classes.dex) chạy trên thiết bị qua app_process:
#   adb push rc-server /data/local/tmp/rc-server
#   adb shell CLASSPATH=/data/local/tmp/rc-server \
#       app_process / com.rigcontrol.server.Server <key=value...>
#
set -euo pipefail
cd "$(dirname "$0")"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"

# Chọn android.jar mới nhất
ANDROID_JAR="$(ls -d "$SDK"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -1 || true)"
[ -n "$ANDROID_JAR" ] || { echo "Không tìm thấy android.jar trong $SDK/platforms"; exit 1; }

# Chọn d8 mới nhất
D8="$(ls -d "$SDK"/build-tools/*/d8 2>/dev/null | sort -V | tail -1 || true)"
[ -n "$D8" ] || { echo "Không tìm thấy d8 trong $SDK/build-tools"; exit 1; }

echo "android.jar: $ANDROID_JAR"
echo "d8:          $D8"

OUT=build
rm -rf "$OUT"
mkdir -p "$OUT/classes"

echo "==> javac"
find src/main/java -name '*.java' > "$OUT/sources.txt"
# JDK 9+ không cho -bootclasspath đi với -target; dùng android.jar làm classpath.
# android.* lấy từ android.jar; java.* lấy từ JDK (đủ để biên dịch, dex hoá ở bước sau).
javac -source 11 -target 11 -nowarn \
      -classpath "$ANDROID_JAR" \
      -d "$OUT/classes" \
      @"$OUT/sources.txt"

echo "==> d8 -> classes.dex"
CLASS_FILES=$(find "$OUT/classes" -name '*.class')
"$D8" --min-api 24 --output "$OUT" $CLASS_FILES

echo "==> đóng gói rc-server"
(cd "$OUT" && jar cf ../rc-server classes.dex)

echo "Xong: $(pwd)/rc-server"
