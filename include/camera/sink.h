#ifndef SINK_H_
#define SINK_H_

#include <stddef.h>
#include <stdlib.h>  // free, inline 函数用到

/* 帧输出（sink）：消费解码后的 RGB24 帧。每个实现填一张 ops 表即可。 */

// 每个sink 实现要提供的操作集（vtable）
struct sink_ops{
    int (*render)(void *ctx, const void *rgb, size_t size);
    void (*destroy)(void *ctx);
};

// sink 实例：公共骨架 + 各实现自己的私有数据
typedef struct frame_sink {
    const struct sink_ops *ops; // 这个 sink 用哪套操作
    void *ctx;                  // 指向「这个 sink 自己的私有数据」 ctx是context（上下文）的缩写
} frame_sink;

// 工厂函数：创建各类 sink，失败返回 NULL
frame_sink *sink_null_create(void);
frame_sink *sink_sdl_create(unsigned width, unsigned height);

// 分发：查 ops 表转发到具体实现。inline 放头文件，省掉独立的 sink.c
static inline int sink_render(frame_sink *s, const void *rgb, size_t size)
{
    if (!s || !rgb || !size) return -1;
    return s->ops->render(s->ctx, rgb, size);
}

static inline void sink_destroy(frame_sink *s)
{
    if (!s) return;
    s->ops->destroy(s->ctx);   // 先释放各实现的私有数据
    free(s);                   // 再释放公共骨架
}

/* null sink 特有：返回已丢弃的帧数（测试/调试用） */
unsigned long sink_null_frame_count(const frame_sink *s);

#endif