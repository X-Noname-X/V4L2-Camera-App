cmake_minimum_required(VERSION 3.10)
project(camera C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

# 核心库（目前只有 frame_queue，后续模块继续往里加源文件）
add_library(camera STATIC
    src/frame_queue.c
)
target_include_directories(camera PUBLIC include)
target_link_libraries(camera PUBLIC Threads::Threads)

# 单元测试
enable_testing()
add_executable(test_frame_queue tests/unit/test_frame_queue.c)
target_link_libraries(test_frame_queue PRIVATE camera)

add_test(NAME frame_queue COMMAND test_frame_queue)
