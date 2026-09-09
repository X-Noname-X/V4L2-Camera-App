cmake_minimum_required(VERSION 3.10)
project(camera C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_package(JPEG QUIET)   # libjpeg(-turbo)：可选，用于 MJPEG 解码

# 核心库
add_library(camera STATIC
    src/frame_queue.c
    src/decoder.c
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

add_test(NAME frame_queue COMMAND test_frame_queue)
add_test(NAME decoder COMMAND test_decoder)
