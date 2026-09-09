/* 集成测试：mock_capture → frame_queue 采集链路（无硬件）
 * 验证假采集能起线程、生成帧、经队列交到消费者手里。 */
#include <stdio.h>
#include <string.h>
#include "camera/capture.h"
#include "camera/frame_queue.h"

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond) do {                                          \
    tests_run++;                                                  \
    if (!(cond)) {                                                \
        tests_failed++;                                           \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                             \
} while (0)

/* 校验一帧是合法灰度 YUYV：所有色度字节（奇数下标）都应是 128 */
static int is_gray_yuyv(const unsigned char *frame, size_t size)
{
    if (size % 2 != 0) return 0;
    for (size_t i = 0; i < size; i += 2)
        if (frame[i + 1] != 128) return 0;
    return 1;
}

int main(void)
{
    /* 2x2 YUYV → 一帧 8 字节 */
    capture_config cfg = { "mock", PIX_FMT_YUYV, 2, 2, 4 };
    capture *cap = cap_create(&cfg);
    CHECK(cap != NULL);
    CHECK(cap_format(cap) == PIX_FMT_YUYV);
    CHECK(cap_width(cap) == 2 && cap_height(cap) == 2);
    CHECK(cap_frame_size(cap) == 8);

    /* 队列帧大小与采集格式匹配，cap_start 校验才过 */
    frame_queue *q = fq_create(4, cap_frame_size(cap), FQ_DROP_OLDEST);
    CHECK(q != NULL);

    CHECK(cap_start(cap, q) == 0);

    /* 连取 3 帧：都是合法灰度帧，且内容逐帧变化（证明帧在流动） */
    unsigned char f1[8] = {0}, f2[8] = {0}, f3[8] = {0};
    size_t sz = 8;
    CHECK(fq_pop(q, f1, &sz, 1000) == 0 && sz == 8);
    CHECK(is_gray_yuyv(f1, 8));

    sz = 8; CHECK(fq_pop(q, f2, &sz, 1000) == 0);
    sz = 8; CHECK(fq_pop(q, f3, &sz, 1000) == 0);
    CHECK(memcmp(f1, f2, 8) != 0);   /* 灰度逐帧递增，相邻帧内容不同 */
    CHECK(memcmp(f2, f3, 8) != 0);

    cap_stop(cap);
    cap_destroy(cap);
    fq_destroy(q);

    printf("== %d 个断言, %d 个失败 ==\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
