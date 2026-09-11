cmake_minimum_required(VERSION 3.10)
project(camera C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_package(JPEG QUIET)   # libjpeg(-turbo)：可选，用于 MJPEG 解码
find_package(SDL2 QUIET)   # SDL2：可选，用于窗口显示

# 核心库：只放硬件无关的模块。
# capture.c 不进这里：它依赖 /dev/video0，无硬件环境下无法构建/测试。
add_library(camera STATIC
    src/frame_queue.c
    src/decoder.c
    src/recorder.c
    src/stats.c
    src/sinks/sink_null.c
)
target_include_directories(camera PUBLIC include)
target_link_libraries(camera PUBLIC Threads::Threads)

if(JPEG_FOUND)
    target_compile_definitions(camera PRIVATE HAVE_LIBJPEG)
    target_link_libraries(camera PUBLIC JPEG::JPEG)
    message(STATUS "找到 libjpeg，启用 MJPEG 解码")
else()
    message(STATUS "未找到 libjpeg，MJPEG 解码不可用（YUYV 仍可用）")
endif()

if(SDL2_FOUND)
    # sink_sdl.c 只在找到 SDL2 时加入。注意 SDL2 2.0.10 的 CMake 包只提供
    # SDL2_LIBRARIES / SDL2_INCLUDE_DIRS 两个变量，没有 SDL2::SDL2 这个 imported
    # target，所以这里用变量。HAVE_SDL2 必须是 PUBLIC——apps/main.c 要靠它
    # 决定用哪个 sink。
    target_sources(camera PRIVATE src/sinks/sink_sdl.c)
    target_compile_definitions(camera PUBLIC HAVE_SDL2)
    target_include_directories(camera PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(camera PUBLIC ${SDL2_LIBRARIES})
    message(STATUS "找到 SDL2，启用窗口显示")
else()
    message(STATUS "未找到 SDL2，sink_sdl 不可用（camera_app 退回 sink_null）")
endif()

# 单元测试
enable_testing()
add_executable(test_frame_queue tests/unit/test_frame_queue.c)
target_link_libraries(test_frame_queue PRIVATE camera)

add_executable(test_decoder tests/unit/test_decoder.c)
target_link_libraries(test_decoder PRIVATE camera)

add_executable(test_sink tests/unit/test_sink.c)
target_link_libraries(test_sink PRIVATE camera)

add_executable(test_recorder tests/unit/test_recorder.c)
target_link_libraries(test_recorder PRIVATE camera)

add_executable(test_stats tests/unit/test_stats.c)
target_link_libraries(test_stats PRIVATE camera)

# 集成测试：真设备（需要一台摄像头）。这条会真的打开 /dev/video0 跑完整 V4L2
# 路径，所以没有摄像头时它是失败而不是跳过——这是刻意的，它测的正是
# 驱动协商、mmap、DQBUF/STREAMOFF 这些只有真硬件才能覆盖的地方。
add_executable(test_capture
    tests/integration/test_capture.c
    src/capture.c
)
target_link_libraries(test_capture PRIVATE camera)

add_test(NAME frame_queue COMMAND test_frame_queue)
add_test(NAME decoder COMMAND test_decoder)
add_test(NAME sink COMMAND test_sink)
add_test(NAME recorder COMMAND test_recorder)
add_test(NAME stats COMMAND test_stats)
add_test(NAME capture COMMAND test_capture)

# cap 相关测试要开关摄像头、等帧、等线程 join，给足超时；
# 真挂死了（比如 cap_stop 没能唤醒 DQBUF）也能被 CTest 判为失败而不是卡住套件。
set_tests_properties(capture PROPERTIES TIMEOUT 60)

# 可执行程序：真采集（V4L2）+ null 显示，需要一台摄像头。
add_executable(camera_app
    apps/main.c
    src/capture.c
)
target_link_libraries(camera_app PRIVATE camera)
