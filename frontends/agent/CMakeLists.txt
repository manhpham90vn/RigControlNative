# rc-agent — CLI chạy trên máy cắm thiết bị (macOS/Linux), C thuần POSIX, chỉ cần adb.
add_executable(rc-agent
    main.c
    util.c
    net.c
    adb.c
    discovery.c
)
target_compile_options(rc-agent PRIVATE -Wall -Wextra)
find_package(Threads REQUIRED)
target_link_libraries(rc-agent PRIVATE Threads::Threads)
