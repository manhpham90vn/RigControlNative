cmake_minimum_required(VERSION 3.20)
project(RigControlNative C)

# ---- Tuỳ chọn ----
# GTK chỉ có trên Linux; mặc định TẮT trên macOS (front-end mac là SwiftUI, build riêng bằng
# toolchain Apple — xem frontends/swiftui).
if(APPLE)
    option(RC_BUILD_GTK "Build front-end GTK4 (Ubuntu)" OFF)
else()
    option(RC_BUILD_GTK "Build front-end GTK4 (Ubuntu)" ON)
endif()
option(RC_BUILD_AGENT "Build rc-agent (CLI cấu hình máy cắm thiết bị/emulator)" ON)
# libcore cần FFmpeg + miniaudio (audio backend cross-platform) → build được trên cả Linux lẫn
# macOS. Front-end GTK cần core nên luôn bật kèm.
option(RC_BUILD_CORE "Build libcore (cần FFmpeg; audio qua miniaudio)" ON)
if(RC_BUILD_GTK)
    set(RC_BUILD_CORE ON CACHE BOOL "" FORCE)
endif()

# ---- Mặc định build ----
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Core dùng chung ----
if(RC_BUILD_CORE)
    add_subdirectory(core)
endif()

# ---- Front-end (thay đổi theo nền tảng) ----
if(RC_BUILD_GTK)
    add_subdirectory(frontends/gtk)
endif()

# rc-agent: chạy trên MÁY KHÁC đang cắm thiết bị/chạy emulator (macOS/Linux) — tự bật
# adb tcpip / mở NAT port rồi in địa chỉ ip:port để nhập vào app. Không phụ thuộc core.
if(RC_BUILD_AGENT)
    add_subdirectory(frontends/agent)
endif()

# frontends/swiftui (macOS) build bằng toolchain Apple — link vào libcore.a mà CMake dựng ở đây.
# Xem frontends/swiftui/build-mac.sh và frontends/swiftui/README.md.
# frontends/winui (Windows) build bằng toolchain riêng (phase sau).
