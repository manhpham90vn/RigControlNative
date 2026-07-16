# libcore — thư viện C dùng chung cho mọi front-end (GTK / WinUI 3 / SwiftUI).

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavutil
    libswresample
)
# Backend phát audio (Ubuntu MVP): ALSA. Cross-platform (miniaudio) để phase sau.
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
find_package(Threads REQUIRED)

add_library(rccore
    src/client.c
    src/adb.c
    src/net.c
    src/server_deploy.c
    src/demuxer.c
    src/decoder.c
    src/audio.c
    src/control_msg.c
)

target_include_directories(rccore
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(rccore
    PUBLIC  Threads::Threads
    PRIVATE PkgConfig::FFMPEG
    PRIVATE PkgConfig::ALSA
)

target_compile_features(rccore PUBLIC c_std_11)
target_compile_options(rccore PRIVATE -Wall -Wextra)

set_target_properties(rccore PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
)
