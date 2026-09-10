/* sink_null：null sink 实现 —— 丢弃所有帧，只计数。
 * 展示「可插拔 sink」的最小实现：填一张 ops 表 + 一个工厂函数。*/
#include "camera/sink.h"
#include <stdlib.h>   /* malloc / calloc / free */

/* null sink 的私有数据：只记录丢了多少帧
 * 它是 frame_sink.ctx 指向的那个东西。 */
struct null_ctx {
    unsigned long frames;
};

/* vtable 里的 render：收到一帧，什么都不做，只把计数 +1。
 * 参数是通用的 void *ctx，先还原成 null_ctx* 才能用。 */
static int null_render(void *ctx, const void *rgb, size_t size)
{
    (void)rgb;                          // 帧内容直接丢弃
    (void)size;
    ((struct null_ctx *)ctx)->frames++; // void* → null_ctx*，还原私有数据
    return 0;
}

// vtable 里的 destroy：释放私有数据（由 sink_destroy 调用）
static void null_destroy(void *ctx)
{
    free(ctx);
}

// 本实现的 vtable：两个函数指针。所有 null sink 实例共用同一张
static const struct sink_ops null_ops = { null_render, null_destroy };

/* 工厂：分配「私有数据 + 公共骨架」，绑定 vtable，返回骨架指针 */
frame_sink *sink_null_create(void)
{
    frame_sink *s = malloc(sizeof(*s));
    struct null_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!s || !ctx) {          // 任一分失败都要回滚，避免泄漏
        free(s);
        free(ctx);
        return NULL;
    }
    s->ops = &null_ops;        // 绑定 vtable
    s->ctx = ctx;              // 塞进私有数据
    return s;
}

/* null 特有：返回已丢弃的帧数（供测试/统计观察） */
unsigned long sink_null_frame_count(const frame_sink *s)
{
    if (!s) return 0;
    return ((const struct null_ctx *)s->ctx)->frames;
}
