# RigControlNative — Makefile tiện dụng bao quanh CMake + build.sh + clang tools.
# Dùng: make help

BUILD_DIR    ?= build
# Ưu tiên Ninja nếu có; nếu không (thường trên Mac chỉ có Xcode CLT) → Unix Makefiles.
GENERATOR    ?= $(shell command -v ninja >/dev/null 2>&1 && echo Ninja || echo "Unix Makefiles")
BUILD_TYPE   ?= Release
CMAKE_FLAGS  ?= -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

# macOS: không có GTK4 → front-end là SwiftUI (build riêng bằng frontends/swiftui/build-mac.sh,
# xem target `mac`). Các target CMake chung trên mac mặc định chỉ build rc-agent (không cần
# ffmpeg); libcore vẫn build được trên mac (audio qua miniaudio) nhưng để `make agent` khỏi đòi
# ffmpeg, để core OFF ở đây — target `mac` tự bật core khi dựng app.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CMAKE_FLAGS += -DRC_BUILD_GTK=OFF -DRC_BUILD_CORE=OFF
endif

# Nguồn để format/lint
C_SOURCES    := $(shell find core frontends -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null)
JAVA_SOURCES := $(shell find server/src -type f -name '*.java' 2>/dev/null)
CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= clang-tidy

.DEFAULT_GOAL := build

# ---- Build ----

.PHONY: all
all: server build ## Build server (dex) + core + front-end

.PHONY: configure
configure: ## Cấu hình CMake (sinh compile_commands.json)
	cmake -B $(BUILD_DIR) -G "$(GENERATOR)" $(CMAKE_FLAGS)

.PHONY: build
build: configure ## Build core + front-end (C)
	cmake --build $(BUILD_DIR)

.PHONY: core
core: configure ## Chỉ build libcore
	cmake --build $(BUILD_DIR) --target rccore

.PHONY: gtk
gtk: configure ## Chỉ build front-end GTK (nếu có libgtk-4-dev)
	cmake --build $(BUILD_DIR) --target rigcontrol

.PHONY: agent
agent: configure ## Build rc-agent (CLI chạy trên máy cắm thiết bị/emulator)
	cmake --build $(BUILD_DIR) --target rc-agent

.PHONY: mac
mac: ## (macOS) Dựng libcore + app SwiftUI → build-mac/RigControlNative.app
	./frontends/swiftui/build-mac.sh

.PHONY: run-mac
run-mac: ## (macOS) Build rồi chạy app SwiftUI (RC_SERIAL=<serial> để mở thẳng)
	@RC_SERIAL="$(SERIAL)" ./frontends/swiftui/build-mac.sh run

.PHONY: server
server: ## Build Android rc-server (javac + d8)
	./server/build.sh

.PHONY: run
run: gtk ## Chạy app GTK (nhiều máy → chọn trên UI; hoặc make run SERIAL=<serial>)
	@serial="$(SERIAL)"; [ -z "$$serial" ] && serial="$$RC_SERIAL"; \
	RC_SERIAL="$$serial" ./$(BUILD_DIR)/frontends/gtk/rigcontrol

.PHONY: devices
devices: ## Liệt kê thiết bị adb
	@adb devices -l

.PHONY: tcpip
tcpip: ## Bật adb TCP/IP trên thiết bị USB (PORT=5555) rồi in lệnh connect
	@port="$(PORT)"; [ -z "$$port" ] && port=5555; \
	adb $(if $(SERIAL),-s $(SERIAL)) tcpip $$port || exit 1; \
	sleep 1; \
	ip=$$(adb $(if $(SERIAL),-s $(SERIAL)) shell ip route 2>/dev/null | awk '/wlan/ {print $$9; exit}'); \
	if [ -n "$$ip" ]; then echo "→ make connect IP=$$ip:$$port"; \
	else echo "Không đọc được IP Wi-Fi; xem trong Cài đặt > Wi-Fi rồi: make connect IP=<ip>:$$port"; fi

.PHONY: connect
connect: ## adb connect IP=<ip[:port]> (mặc định port 5555)
	@[ -n "$(IP)" ] || { echo "Dùng: make connect IP=192.168.1.x[:5555]"; exit 1; }
	@case "$(IP)" in *:*) adb connect "$(IP)";; *) adb connect "$(IP):5555";; esac

.PHONY: disconnect
disconnect: ## adb disconnect [IP=<ip[:port]>] (bỏ IP = ngắt tất cả)
	@if [ -n "$(IP)" ]; then adb disconnect "$(IP)"; else adb disconnect; fi

# ---- Format ----

.PHONY: format
format: ## Định dạng lại toàn bộ C + Java (clang-format -i)
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { echo "Thiếu clang-format: sudo apt install clang-format"; exit 1; }
	@echo "==> clang-format C ($(words $(C_SOURCES)) file)"
	@$(if $(strip $(C_SOURCES)),$(CLANG_FORMAT) -i $(C_SOURCES),true)
	@echo "==> clang-format Java ($(words $(JAVA_SOURCES)) file)"
	@$(if $(strip $(JAVA_SOURCES)),$(CLANG_FORMAT) -i $(JAVA_SOURCES),true)

.PHONY: format-check
format-check: ## Kiểm tra định dạng (không sửa; fail nếu lệch)
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { echo "Thiếu clang-format: sudo apt install clang-format"; exit 1; }
	@$(CLANG_FORMAT) --dry-run --Werror $(C_SOURCES) $(JAVA_SOURCES) && echo "Định dạng OK"

# ---- Lint ----

.PHONY: lint
lint: configure ## Lint C bằng clang-tidy; fallback build -Werror nếu thiếu
	@if command -v $(CLANG_TIDY) >/dev/null 2>&1; then \
		echo "==> clang-tidy"; \
		$(CLANG_TIDY) -p $(BUILD_DIR) $(filter %.c,$(C_SOURCES)); \
	else \
		echo "Thiếu clang-tidy (sudo apt install clang-tidy) — fallback build -Werror"; \
		$(MAKE) lint-strict; \
	fi

.PHONY: lint-strict
lint-strict: ## Build core với -Werror để bắt cảnh báo
	cmake -B $(BUILD_DIR)-strict -G "$(GENERATOR)" $(CMAKE_FLAGS) \
		-DCMAKE_C_FLAGS="-Wall -Wextra -Werror"
	cmake --build $(BUILD_DIR)-strict --target rccore

# ---- Dọn dẹp ----

.PHONY: clean
clean: ## Xoá thư mục build (C + server + app mac)
	rm -rf $(BUILD_DIR) $(BUILD_DIR)-strict build-mac server/build server/rc-server

.PHONY: deps
deps: ## In gợi ý cài dependency
	@echo "Ubuntu:"
	@echo "  sudo apt install build-essential cmake ninja-build pkg-config \\"
	@echo "       libavcodec-dev libavutil-dev libswresample-dev \\"
	@echo "       libgtk-4-dev libepoxy-dev clang-format clang-tidy"
	@echo "  (audio qua miniaudio: dlopen ALSA/PulseAudio lúc chạy — không cần libasound2-dev để build)"
	@echo "  Android SDK: platforms/android-*/android.jar + build-tools/*/d8 (ANDROID_HOME)"

.PHONY: help
help: ## Liệt kê target
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'
