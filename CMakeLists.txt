cmake_minimum_required(VERSION 3.10)
project(camera C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_package(JPEG QUIET)   # libjpeg(-turbo)：可选，用于 MJPEG 解码

# 核心库：只放硬件无关的模块。
# capture.c 刻意不进这里（方案 B）：真采集依赖 /dev/video0，
# 测试用 tests/unit/mock_capture.c 顶替同一套接口，二者不能同链接。
add_library(camera STATIC
    src/frame_queue.c
    src/decoder.c
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

# 单元测试
enable_testing()
add_executable(test_frame_queue tests/unit/test_frame_queue.c)
target_link_libraries(test_frame_queue PRIVATE camera)

add_executable(test_decoder tests/unit/test_decoder.c)
target_link_libraries(test_decoder PRIVATE camera)

# 集成测试：mock_capture 顶替真采集，验证 采集→队列 链路（无硬件）
add_executable(test_capture
    tests/integration/test_capture.c
    tests/unit/mock_capture.c
)
target_link_libraries(test_capture PRIVATE camera)

add_executable(test_sink tests/unit/test_sink.c)
target_link_libraries(test_sink PRIVATE camera)

add_test(NAME frame_queue COMMAND test_frame_queue)
add_test(NAME decoder COMMAND test_decoder)
add_test(NAME capture COMMAND test_capture)
add_test(NAME sink COMMAND test_sink)

# 可执行程序：demo 版用 mock 采集 + null 显示，无硬件依赖。
# 之后接真摄像头：把 tests/unit/mock_capture.c 换成 src/capture.c 即可。
add_executable(camera_app
    apps/main.c
    tests/unit/mock_capture.c
)
target_link_libraries(camera_app PRIVATE camera)
