# V4L2-Camera-App

多线程 V4L2 摄像头采集库：零拷贝 mmap 采集、有界环形缓冲、可插拔输出（sink）、录像与性能统计，支持交叉编译到 aarch64。

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

- V4L2 缓冲立刻 QBUF 还回驱动，继续接下一帧（快，不拖累采集）；
- frame_queue 里那份拷贝，消费线程可以慢慢取。

如果没有这层拷贝、让消费线程直接读 V4L2 缓冲，消费线程一慢，驱动就等不到空闲缓冲，整个采集卡死或狂丢帧。**frame_queue 这层「拷贝 + 队列」就是用一点内存，换「采集和消费各自独立运行」的自由。**

### 关于「零拷贝」的说法

- **内核 → 用户态**：mmap 之后摄像头 DMA 直接写进 `cap->buffers[i]`，这段内存用户可直接读，没有传统「内核拷一份给用户态」的拷贝——这就是「零拷贝」指的那一步。
- **用户态内部**：`fq_push` 的 `memcpy` 是**一次**用户态拷贝，是主动加的（为了解耦），不影响「采集路径零拷贝」这个卖点。

严格说：**采集路径零拷贝 + 之后一次可控的用户态拷贝。**

### 一句话总结

> **`cap->buffers[]` 是摄像头直接写的「原片」，QBUF/DQBUF 是向驱动「借/还」原片的手续；`cap->fq` 是我们自己复印出来的「照片队列」，复印完立刻把原片还回去。**


### 两个线程怎么「配合」跑（时间线）
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

  - 共享同一个 cap：因为传的是指针，主线程和新线程看到的是同一块 malloc 
  出来的结构体。这也是之前一直强调数据竞争的原因——cap
  里的字段，谁在读、谁在写，要分清楚。
  - pthread_join 是「等它结束」：主线程 join
  时，如果新线程还在跑，主线程就阻塞等着；新线程 return NULL（或 break 后
  return）那一刻，join 返回。return NULL 里的 NULL
  就是这个线程的「退出状态」，pthread_join 第二个参数可以接住它（这里填 NULL
  表示不关心）。

## sink 的完整流转：vtable 分发 + ctx 私有数据

sink 是「运行期多选」的可插拔接口（null/sdl/fb），靠「公共骨架 + 函数指针表（vtable）+ 私有数据（ctx）」实现。以 sink_null 为例，跟踪一帧的完整流转。

### 三个角色

| 角色 | 是什么 | 在哪 |
|---|---|---|
| `frame_sink` | 公共骨架：`ops`（指向一张操作表）+ `ctx`（指向私有数据） | `sink.h` |
| `sink_ops` | vtable：每个实现填一张（`render` / `destroy` 两个函数指针） | `sink.h` |
| `ctx` | 各实现自己的私有数据（null 存帧计数，sdl 存窗口句柄，fb 存文件描述符） | 各实现 `.c` |

### 一帧的完整流转

```
sink_null_create()
  分配 null_ctx（存帧计数器）
  s->ctx = ctx                      ① 把私有数据塞进骨架
  s->ops = &null_ops                ② 绑定这张 vtable

sink_render(s, rgb, size)           ③ 公共入口（inline 在 sink.h）
  if (!s || !rgb || !size) return -1   参数校验
  return s->ops->render(s->ctx, ...)   查 vtable，把 ctx 传给实现

null_render(void *ctx, ...)         ④ 具体实现（sink_null.c）
  ((struct null_ctx*)ctx)->frames++     void* 还原成 null_ctx*，计数

sink_destroy(s)
  s->ops->destroy(s->ctx)           ⑤ 先释放私有数据
  free(s)                           ⑥ 再释放公共骨架
```

### 为什么 ctx 是 void *

vtable 的函数签名是通用的（`render(void *ctx, ...)`），不能写死成 `struct null_ctx*` 或 `struct sdl_ctx*`。所以私有数据被「类型擦除」成 `void*` 传进去，实现内部再还原成具体类型。这就是 C 里回调 / 插件 / 线程通用的「上下文指针（context / user data / cookie）」套路——和 `capture_thread(void *arg)` 里的 `arg` 是同一个东西。

一句话：**工厂把私有数据塞进 `ctx`，公共入口查 vtable 把 `ctx` 传过去，实现里把它还原出来用。**