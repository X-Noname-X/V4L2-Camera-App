# V4L2-Camera-App

多线程 V4L2 摄像头采集库 + 一个演示程序。零拷贝 mmap 采集、有界环形缓冲解耦采集与消费、MJPEG/YUYV/RGB24 解码。

## 目录结构

```
include/camera/   frame_queue.h  capture.h  decoder.h    ← 库的接口
src/              frame_queue.c  capture.c  decoder.c    ← 库的实现，编成 libcamera.a
apps/             main.c  preview.c                      ← 演示程序
tests/            integration/test_capture.c             ← 真设备集成测试
```

**库和应用分得很开。** `libcamera.a` 只放能被别的程序复用的东西——采集、队列、解码，**一行显示代码都没有**。真正的产品（比如跑在 LCD 上的那个）链接这个库，拿到 RGB24 之后自己决定怎么画到屏幕上。

`apps/preview.c` 只是本仓库「看得见画面」用的调试窗口，不是库的一部分。等 LCD 那边能跑了，它可以整个删掉。

## 编译

依赖：`libjpeg`、`SDL2`、`pthread`

```bash
cmake -S . -B build
cmake --build build
```

## 用法

```
./build/camera_app [选项]

  -d, --device=PATH  采集设备（默认 /dev/video0）
  -n, --frames=N     处理 N 帧后退出；0 = 不限（默认 0）
  -r, --record=PATH  把原始帧录到文件（可与预览同时进行）
  -f, --format=NAME  像素格式：mjpeg / yuyv / rgb24（默认 mjpeg）
  -s, --size=WxH     分辨率（默认 640x480）
  -h, --help         显示本帮助
```

运行时：**空格**存一张 `photo_000N.jpg`（仅 MJPEG），**ESC 或关窗口**退出。

### 几个例子

```bash
./build/camera_app                                  # 默认 MJPEG 640x480，显示到关窗口
./build/camera_app --frames=150                     # 处理 150 帧就退出
./build/camera_app --size=1280x720                  # 720p
./build/camera_app --format=yuyv --size=1280x720    # 同样 720p，但换成 YUYV
./build/camera_app --frames=150 --record=rec.mjpg   # 边显示边录
```

**格式和帧率的关系值得亲手量一下**——这台摄像头的 YUYV 在 720p 下只有 10fps 左右，MJPEG 才有 30fps：

```
$ ./build/camera_app --size=1280x720 --frames=100
完成：100 帧，平均 29.6 fps

$ ./build/camera_app --format=yuyv --size=1280x720 --frames=40
完成：40 帧，平均 9.6 fps
```

### 录像和拍照的文件在哪

**都在进程的工作目录**，路径原样交给 `open()`，不创建目录：

```bash
cd build && ./camera_app --record=rec.mjpg    # → build/rec.mjpg
./build/camera_app --record=/tmp/rec.mjpg     # → /tmp/rec.mjpg
```

照片是 `photo_0001.jpg`、`photo_0002.jpg`……依次往后编号。编号用 `O_EXCL` 打开——文件已存在就失败，所以**换多少次运行也不会覆盖之前拍的照片**。

### 录像怎么播放

录的是**采集到的原始帧**，不解码。MJPEG 下每帧本身就是一张完整的 JPEG，所以录像文件就是一串 JPEG 首尾相接——**没有容器，没有文件头，也没有时间戳**。

所以 **`mpv rec.mjpg` 播不了**，会报 `Failed to recognize file format`——文件没坏，是开头那个 SOI 标记不足以让播放器认定它是 MJPEG。改扩展名也没用（实测 `.mjpeg`、甚至改成 `.mp4` 都一样），它看的是内容不是名字。两种办法：

```bash
mpv --demuxer-lavf-format=mjpeg rec.mjpg                     # 指定 demuxer，直接播
ffmpeg -f mjpeg -framerate 30 -i rec.mjpg -c copy out.mp4    # 装进容器，之后任何播放器都能开
```

`-framerate` 和 `-c copy` 都不能省：

- **`-framerate 30`**：没有时间戳，不指定的话 ffmpeg 按默认的 25fps 装。150 帧本该是 5.0s（30fps），会变成 6.0s，**慢放 20%**
- **`-c copy`**：直接复制 JPEG 数据，不重新编码。不加的话 ffmpeg 会默认转成 H.264——**有损压缩，出来已经不是原始帧了**。实测这个文件 `-c copy` 是 5.19 MB（只多出容器开销），重新编码只剩 1.08 MB；想留住原始画质就必须写 `-c copy`

录像体积随画面内容剧烈变化，**没有固定值**——**同一个 640x480**，测试里对着静止画面录每帧才 21 KB，实拍能到 34.6–37.7 KB（两边都是 150 帧，5.19 MB 和 5.65 MB），**相差将近一倍**。压缩后的大小取决于画面好不好压，跟分辨率没有单调关系。对照同样 150 帧的 YUYV：每帧固定 `640×480×2 = 614400` 字节，合计 92 MB，是它的 15 倍以上。

拍照同理——既然帧本身就是 JPEG，存盘就只是 `fwrite`，**不需要任何编码器**。这也是为什么拍出来的照片必须是 MJPEG 格式：`--format=yuyv` 时按空格只会得到一句提示，不会写出一个打不开的 `.jpg`。

## 测试

```bash
cd build && ctest
```

需要一台真摄像头——它跑的是完整 V4L2 路径，没有硬件时**失败而不是跳过**，这是刻意的。它把 MJPEG（变长帧）和 YUYV（定长帧）各跑一遍，因为这两种帧在队列里走的是不同分支，只测一种的话另一种坏了没人知道。

断言的是「假采集」永远测不到的东西：

- 驱动协商出的格式和尺寸
- 每帧长度不超过驱动缓冲区；MJPEG 的每一帧都带 JPEG 的 SOI 标记且能被解码；YUYV 的每帧长度精确等于 `宽×高×2`
- **`cap_stop` 能否让阻塞在 `DQBUF` 的线程干净退出**——这个机制写错了 `pthread_join` 会永久阻塞，是最难靠肉眼发现的一类 bug

## 核心概念：capture 里的两套「缓冲」和「队列」

`src/capture.c` 里「缓冲」和「队列」**各出现了两套，而且名字还撞了**，很容易混。先分清它们是两件完全不同层次的事。

### 四样东西各是什么

| 名字 | 是谁 | 谁在管 | 生命周期 |
|---|---|---|---|
| `cap->buffers[]` | **V4L2 内核缓冲**：mmap 出来的若干块内存，摄像头 DMA 直接往里面写图像 | 内核驱动 | `cap_create` 映射 → `cap_destroy` munmap |
| `buf`（`struct v4l2_buffer`） | **缓冲描述符**：只告诉你「第几块缓冲、写了多少字节」，本身不是数据 | 内核 | 每次 DQBUF 填充 |
| QBUF / DQBUF 的「队列」 | **驱动的缓冲队列**：管理「哪块空闲待写 / 哪块已写好」 | 内核 | 随流开始/结束 |
| `cap->fq`（`frame_queue`） | **我们自己写的环形队列**：用户态，存拷贝出来的帧数据 | 我们 | `fq_create` → `fq_destroy` |

困惑的根源：**`cap->buffers` 和 `cap->fq` 都叫「存帧的地方」，QBUF 和 `frame_queue` 又都叫「队列」**。

### 数据流

```
摄像头 ──DMA──▶ V4L2内核缓冲(cap->buffers[i])      我们的 frame_queue(cap->fq)
                 │                                    │
                 │  DQBUF：取一块"写好的"               │
                 │  ↓                                 │
                 │  fq_push：memcpy 拷一份 ────────────▶ 环形缓冲
                 │  ↓                                 │
                 │  QBUF：把这块还回驱动复用              │
                 └── 循环 ─────────────────────────────┘
                                                       ↓
                                            fq_pop：消费线程取出
```

一句话：**V4L2 的缓冲是「摄像头直接写」的共享内存，frame_queue 是「拷贝出来」的用户态队列。**

### 两套东西各自的职责

**V4L2 缓冲 + 驱动队列（QBUF/DQBUF）**

- 通常只有 4 块，必须**飞快地循环**：DQBUF 取一块 → 赶紧用完 → QBUF 还回去，让驱动继续写下一帧。还慢了驱动就没有空闲缓冲，只能丢帧。
- 所以它**不能**用来做「缓冲解耦」——它太金贵，是「借你一下，立刻还」。

**frame_queue**

- 就是 `frame_queue.c` 里的环形队列，带 `DROP_OLDEST / DROP_NEWEST` 丢帧策略和条件变量。
- 作用是**解耦**：采集线程和消费线程速度不一致时，在这里缓冲，用丢帧策略消化掉。

### 为什么需要「两套」而不是直接用 V4L2 缓冲

关键在采集线程的这行：

```c
fq_push(cap->fq, cap->buffers[buf.index], buf.bytesused);
```

`fq_push` 内部是 `memcpy`——把 V4L2 缓冲里的数据**复制了一份**进 frame_queue。复制完：

- V4L2 缓冲立刻 QBUF 还回驱动，继续接下一帧（快，不拖累采集）
- frame_queue 里那份拷贝，消费线程可以慢慢取

如果没有这层拷贝、让消费线程直接读 V4L2 缓冲，消费线程一慢，驱动就等不到空闲缓冲，整个采集卡死或狂丢帧。**frame_queue 这层「拷贝 + 队列」就是用一点内存，换「采集和消费各自独立运行」的自由。**

### 关于「零拷贝」的说法

- **内核 → 用户态**：mmap 之后摄像头 DMA 直接写进 `cap->buffers[i]`，这段内存用户可直接读，没有传统「内核拷一份给用户态」的拷贝——这就是「零拷贝」指的那一步
- **用户态内部**：`fq_push` 的 `memcpy` 是**一次**用户态拷贝，是主动加的（为了解耦），不影响「采集路径零拷贝」这个卖点

严格说：**采集路径零拷贝 + 之后一次可控的用户态拷贝。**

### 一句话总结

> **`cap->buffers[]` 是摄像头直接写的「原片」，QBUF/DQBUF 是向驱动「借/还」原片的手续；`cap->fq` 是我们自己复印出来的「照片队列」，复印完立刻把原片还回去。**

## 队列为什么是变长的

`frame_queue` 的每个槽有固定的**容量**，但存的帧可以比容量短：

```c
frame_queue *fq_create(size_t capacity, size_t max_size, fq_policy policy);
int          fq_push(frame_queue *fq, const void *data, size_t size);   // size <= max_size
int          fq_pop(frame_queue *fq, void *out, size_t *size, int timeout_ms);
```

因为 YUYV 和 MJPEG 的帧长规律完全不同：

| | 每帧长度 | 实测 |
|---|---|---|
| YUYV | **定长**，恒等于 `宽×高×2` | `614400..614400` |
| MJPEG | **变长**，取决于画面内容能压多小 | `20908..21054` |

`fq_pop` 的 `*size` 入参是「out 的容量」、出参是「这一帧的实际长度」，所以调用方永远按最大容量准备缓冲，拿到多少用多少。

**槽容量必须 >= `cap_max_frame_size(cap)`**，`cap_start` 会校验这一点。因为这个数来自 `QUERYBUF` 报出来的**驱动缓冲区长度**，是驱动一帧最多能写的上界；一旦队列槽比它小，采集线程里 `fq_push` 会失败而帧被**静默丢掉**——采不到画面，但日志干干净净，非常难查。

注意它只是**上界**：驱动对压缩格式也按「不压缩」预留（这台摄像头 MJPEG 和 YUYV 报的都是 614400），而 MJPEG 实际一帧才 20–38 KB。所以按它设 8 个槽会占 `8 × 614400 ≈ 4.7 MB`，内存是过配的——这是个已知的取舍，不是 bug。

## 两个线程怎么「配合」跑（时间线）

```
  主线程 (cap_start)                        新线程 (capture_thread)
  ────────────────────                     ──────────────────────
  cap->fq = fq;
  pthread_create(..., cap)  ─────▶ 新线程诞生，开始执行 capture_thread(cap)
  cap->running = 1;                       capture *cap = arg;   // 同一个 cap！
  return 0;                              for(;;){
  （继续做别的，比如 fq_pop）              DQBUF 等帧 → fq_push → QBUF
                                         ...
  ... 过一会 ...                          ... 一直循环 ...
  cap_stop():
    STREAMOFF ──────────────────▶         DQBUF 返回错误 → break
    pthread_join(&cap->thread) ◀────────── return NULL（线程退出）
    cap->running = 0;
```

两个关键点：

- **共享同一个 cap**：因为传的是指针，主线程和新线程看到的是同一块 malloc 出来的结构体。这也是之前一直强调数据竞争的原因——cap 里的字段，谁在读、谁在写，要分清楚
- **`pthread_join` 是「等它结束」**：主线程 join 时，如果新线程还在跑，主线程就阻塞等着；新线程 `return NULL`（或 break 后 return）那一刻，join 返回。`return NULL` 里的 NULL 就是这个线程的「退出状态」，`pthread_join` 第二个参数可以接住它（这里填 NULL 表示不关心）

**线程是怎么被叫停的**：`DQBUF` 是阻塞调用，正常情况下会一直卡在那儿等帧。`cap_stop` 先发 `STREAMOFF`，内核会让阻塞中的 `DQBUF` 立刻返回错误，采集线程据此跳出循环——**不是靠读某个标志位轮询**。所以 `cap_start` 里那句 `open()` 特意不加 `O_NONBLOCK`：非阻塞模式下 `DQBUF` 会立刻返回 `EAGAIN`，只能改成忙等轮询，白白烧 CPU。
