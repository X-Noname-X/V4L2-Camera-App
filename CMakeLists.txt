cmake_minimum_required(VERSION 3.10)
project(camera C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_package(JPEG  REQUIRED)   # MJPEG 解码（decoder.c）+ 存 JPEG（apps/save_jpg.c）
find_package(SDL2  REQUIRED)   # 本地预览窗口（apps/preview.c）

# 库：采集 + 队列 + 解码。别人链接这个就够（比如 LCD 上的显示程序）。
add_library(camera STATIC
    src/frame_queue.c
    src/decoder.c
    src/capture.c
)
target_include_directories(camera PUBLIC include)
target_link_libraries(camera PUBLIC Threads::Threads JPEG::JPEG)

# 演示程序：采集 → 队列 → 解码 → 预览（+ 可选录像）
add_executable(camera_app
    apps/main.c
    apps/preview.c
)
target_include_directories(camera_app PRIVATE apps ${SDL2_INCLUDE_DIRS})
target_link_libraries(camera_app PRIVATE camera ${SDL2_LIBRARIES})

# 集成测试：真设备，需要一台摄像头。它验的是只有真硬件才能覆盖的东西——
# 驱动协商、mmap、以及 cap_stop 能不能把阻塞在 DQBUF 的线程唤醒（写错就是永久挂死）。
enable_testing()
add_executable(test_capture tests/integration/test_capture.c)
target_link_libraries(test_capture PRIVATE camera)
add_test(NAME capture COMMAND test_capture)
set_tests_properties(capture PROPERTIES TIMEOUT 60)
