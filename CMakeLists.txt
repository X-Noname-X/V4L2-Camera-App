cmake_minimum_required(VERSION 3.10)
project(camera C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

# 本仓库对外只提供一样东西：libcamera.a + include/camera 下的三个头文件。
# apps/ 和 tests/ 是本仓库自己开发时用的，不是库的一部分——别的项目把它当
# 依赖时用 -DBUILD_APPS=OFF 关掉，免得被迫去装 SDL2、也免得编出用不上的可执行文件。
option(BUILD_APPS "编 apps/ 下的演示程序和 tests/ 下的集成测试" ON)

find_package(Threads REQUIRED)

# MJPEG 解码（decoder.c）用的 JCS_RGB565 是 libjpeg-turbo 的扩展，
# 发行版的 libjpeg-dev 装的都是它；换成 IJG 原版会编不过。
#
# 交叉编译时别走这里——find_package 会找到宿主机的 x86 版，链进去在板子上跑不起来。
# 改成显式指定：
#   -DJPEG_INCLUDE_DIR=<前缀>/include -DJPEG_LIBRARY=<前缀>/lib/libjpeg.a
find_package(JPEG REQUIRED)

# 库：采集 + 队列 + 解码。别人链接这个就够（比如 LCD 上的显示程序）。
add_library(camera STATIC
    src/frame_queue.c
    src/decoder.c
    src/capture.c
)
target_include_directories(camera PUBLIC include)
target_link_libraries(camera PUBLIC Threads::Threads JPEG::JPEG)

# 消费方要的就是这两样：静态库和头文件。源码留在本仓库，不跟着搬进别人的树里。
install(TARGETS camera ARCHIVE DESTINATION lib)
install(DIRECTORY include/camera DESTINATION include)

# 库到此为止。下面都是本仓库自用的，关掉 BUILD_APPS 就都不编。
if(NOT BUILD_APPS)
    return()
endif()

find_package(SDL2 REQUIRED)   # 本地预览窗口（apps/preview.c）

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
