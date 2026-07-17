# Front-end GTK4 (MVP). Chỉ build khi có GTK4 dev trên máy.

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
pkg_check_modules(EPOXY IMPORTED_TARGET epoxy)

if(NOT GTK4_FOUND OR NOT EPOXY_FOUND)
    message(WARNING
        "Không tìm thấy GTK4 (libgtk-4-dev) hoặc libepoxy (libepoxy-dev) — bỏ qua front-end GTK. "
        "Cài bằng: sudo apt install libgtk-4-dev libepoxy-dev")
    return()
endif()

add_executable(rigcontrol
    main.c
    chooser.c
    session.c
    render.c
    input.c
    agent.c
)

target_link_libraries(rigcontrol
    PRIVATE rccore
    PRIVATE PkgConfig::GTK4
    PRIVATE PkgConfig::EPOXY
)

target_compile_options(rigcontrol PRIVATE -Wall -Wextra)
