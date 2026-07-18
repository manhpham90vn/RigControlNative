# libcore — thư viện C dùng chung cho mọi front-end (GTK / WinUI 3 / SwiftUI).

# ---- FFmpeg ----
# Ưu tiên pkg-config (Linux, và macOS nếu có). macOS thường không cài pkg-config nhưng vẫn có
# .pc của Homebrew — fallback tìm thủ công theo prefix Homebrew để khỏi bắt cài thêm pkg-config.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavutil libswresample)
endif()

if(FFMPEG_FOUND)
    set(RC_FFMPEG_TARGET PkgConfig::FFMPEG)
else()
    # Không có pkg-config: tìm include + lib trực tiếp (Homebrew: brew --prefix ffmpeg).
    if(APPLE AND NOT DEFINED FFMPEG_ROOT)
        execute_process(COMMAND brew --prefix ffmpeg
            OUTPUT_VARIABLE FFMPEG_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
        HINTS ${FFMPEG_ROOT} PATH_SUFFIXES include)
    find_library(AVCODEC_LIB avcodec HINTS ${FFMPEG_ROOT} PATH_SUFFIXES lib)
    find_library(AVUTIL_LIB avutil HINTS ${FFMPEG_ROOT} PATH_SUFFIXES lib)
    find_library(SWRESAMPLE_LIB swresample HINTS ${FFMPEG_ROOT} PATH_SUFFIXES lib)
    if(NOT FFMPEG_INCLUDE_DIR OR NOT AVCODEC_LIB OR NOT AVUTIL_LIB OR NOT SWRESAMPLE_LIB)
        message(FATAL_ERROR "Không tìm thấy FFmpeg (libavcodec/libavutil/libswresample). "
            "Cài qua Homebrew (brew install ffmpeg) hoặc apt (libavcodec-dev ...).")
    endif()
    add_library(rc_ffmpeg INTERFACE)
    target_include_directories(rc_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(rc_ffmpeg INTERFACE ${AVCODEC_LIB} ${AVUTIL_LIB} ${SWRESAMPLE_LIB})
    set(RC_FFMPEG_TARGET rc_ffmpeg)
endif()

find_package(Threads REQUIRED)

add_library(rccore
    src/client.c
    src/adb.c
    src/net.c
    src/server_deploy.c
    src/demuxer.c
    src/decoder.c
    src/audio.c
    src/miniaudio_impl.c
    src/control_msg.c
)

target_include_directories(rccore
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_SOURCE_DIR}/third_party/miniaudio
)

target_link_libraries(rccore
    PUBLIC  Threads::Threads
    PRIVATE ${RC_FFMPEG_TARGET}
)

# ---- Backend audio (miniaudio) — framework/lib theo nền tảng ----
if(APPLE)
    target_link_libraries(rccore PRIVATE
        "-framework CoreFoundation" "-framework CoreAudio"
        "-framework AudioToolbox" "-framework AudioUnit")
elseif(UNIX)
    # miniaudio nạp ALSA/PulseAudio qua dlopen lúc chạy → chỉ cần libdl + libm + pthread.
    target_link_libraries(rccore PRIVATE ${CMAKE_DL_LIBS} m)
endif()

target_compile_features(rccore PUBLIC c_std_11)
target_compile_options(rccore PRIVATE -Wall -Wextra)

set_target_properties(rccore PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
)
