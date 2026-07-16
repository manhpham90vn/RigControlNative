cmake_minimum_required(VERSION 3.20)
project(RigControlNative C)

# ---- Tuỳ chọn ----
option(RC_BUILD_GTK "Build front-end GTK4 (Ubuntu)" ON)

# ---- Mặc định build ----
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Core dùng chung ----
add_subdirectory(core)

# ---- Front-end (thay đổi theo nền tảng) ----
if(RC_BUILD_GTK)
    add_subdirectory(frontends/gtk)
endif()

# frontends/winui và frontends/swiftui build bằng toolchain riêng của nền tảng (Phase 6).
