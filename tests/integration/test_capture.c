/* tests/integration/test_capture.c：真设备集成测试（需要一台摄像头）。
 *
 * 走的是 src/capture.c 的完整 V4L2 路径：
 *   QUERYCAP → S_FMT 协商 → REQBUFS → QUERYBUF+mmap+QBUF
 *   → STREAMON → 采集线程 DQBUF/推送/QBUF → STREAMOFF 让线程退出
 *
 * 用法：test_capture [设备]     默认 /dev/video0
 *
 * 这里断言的是「假采集」永远测不到的东西：
 *   - 驱动协商出的格式和尺寸
 *   - 定长格式下，驱动报的 bytesused 是否恰好等于我们算的帧大小
 *   - cap_stop 能否让阻塞在 DQBUF 的线程干净退出（写错就是永久挂死）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera/capture.h"
#include "camera/frame_queue.h"

#define DEV_DEFAULT "/dev/video0"
#define REQ_W       640
#define REQ_H       480
#define BUF_COUNT   4
#define NFRAMES     10     /* 连续取多少帧做检查 */

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond) do {                                          \
    tests_run++;                                                  \
    if (!(cond)) {                                                \
        tests_failed++;                                           \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                             \
} while (0)

/* 一帧是不是全 0 字节（mmap 到的内存从没被驱动写过就会是全 0） */
static int all_zero(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : DEV_DEFAULT;

    /* ---------- 1. 打开设备并协商格式 ---------- */
    capture_config cfg = { dev, PIX_FMT_YUYV, REQ_W, REQ_H, BUF_COUNT };
    capture *cap = cap_create(&cfg);
    if (!cap) {
        fprintf(stderr, "cap_create 失败：%s（具体原因见上一行）\n"
                        "常见原因：设备不存在、不是采集节点、"
                        "被别的进程占用、当前用户不在 video 组\n", dev);
        return 1;   /* 这条测试依赖真硬件，环境不具备就是失败，不是跳过 */
    }

    size_t frame_size = cap_frame_size(cap);
    printf("协商结果：%s %ux%u，一帧 %zu 字节\n",
           pixel_format_name(cap_format(cap)),
           cap_width(cap), cap_height(cap), frame_size);

    /* 格式必须是定长 YUYV——下面「bytesused 恰好等于帧大小」的断言依赖它 */
    CHECK(cap_format(cap) == PIX_FMT_YUYV);
    CHECK(cap_width(cap) > 0 && cap_height(cap) > 0);
    CHECK(frame_size == (size_t)cap_width(cap) * cap_height(cap) * 2);

    /* ---------- 2. 队列：帧大小须与采集格式一致，否则 cap_start 会拒绝 ---------- */
    frame_queue *q = fq_create(8, frame_size, FQ_DROP_OLDEST);
    CHECK(q != NULL);
    CHECK(fq_frame_size(q) == frame_size);

    /* ---------- 3. 起采集线程（生产端） ---------- */
    CHECK(cap_start(cap, q) == 0);

    /* ---------- 4. 连续取帧 ---------- */
    unsigned char *frame = malloc(frame_size);
    unsigned char *first = malloc(frame_size);
    CHECK(frame != NULL && first != NULL);

    int got = 0, differing = 0, zero_frames = 0;
    for (int i = 0; i < NFRAMES && frame && first; i++) {
        size_t sz = frame_size;
        if (fq_pop(q, frame, &sz, 3000) != 0) break;   /* 超时说明帧没流动 */
        got++;

        /* 定长 YUYV：驱动报的 bytesused 应当恰好等于我们算出的帧大小。
         * 这条一旦不符，说明帧大小计算或格式协商有错，后面解码会全部错位。 */
        if (sz != frame_size) {
            tests_run++; tests_failed++;
            printf("FAIL 第 %d 帧实际 %zu 字节，期望 %zu\n", i, sz, frame_size);
        }
        if (all_zero(frame, frame_size)) zero_frames++;
        if (i == 0) memcpy(first, frame, frame_size);
        else if (memcmp(first, frame, frame_size) != 0) differing++;
    }
    CHECK(got == NFRAMES);      /* 每帧都在 3s 内取到 → 帧确实在流动 */
    CHECK(zero_frames < got);   /* 不是帧帧全黑 → 驱动真的往缓冲里写了数据 */
    CHECK(differing > 0);       /* 至少一帧与首帧不同 → 不是反复读同一块缓冲 */

    free(frame);
    free(first);

    /* ---------- 5. 停流 ----------
     * cap_stop 靠 STREAMOFF 让阻塞中的 DQBUF 返回错误，线程才会退出。
     * 这个机制写错了 pthread_join 会永久阻塞——这里挂了就说明有 bug
     * （CTest 侧有 TIMEOUT 兜底，不会真的挂住整个套件）。 */
    cap_stop(cap);
    CHECK(1);

    cap_stop(cap);   /* 幂等：未在运行时应当安全返回 */
    CHECK(1);

    /* 采集线程真的退出了吗：排空残留帧后，再取应当超时（没有生产者在推了） */
    size_t sz = frame_size;
    while (fq_pop(q, frame, &sz, 0) == 0) sz = frame_size;   /* timeout=0，取空为止 */
    sz = frame_size;
    CHECK(fq_pop(q, frame, &sz, 200) != 0);

    cap_destroy(cap);
    fq_destroy(q);

    /* ---------- 6. 另一条路径：运行中直接 cap_destroy ----------
     * cap_destroy 内部会自动 cap_stop。这条路径 apps/main.c 出错时走得到，
     * 容易漏测（不 stop 就直接 destroy 会不会泄漏 / 挂死）。 */
    capture *cap2 = cap_create(&cfg);
    CHECK(cap2 != NULL);
    if (cap2) {
        frame_queue *q2 = fq_create(4, cap_frame_size(cap2), FQ_DROP_OLDEST);
        CHECK(q2 != NULL);
        CHECK(cap_start(cap2, q2) == 0);
        cap_destroy(cap2);   /* 不先 stop：验证内部自动停流 + join */
        CHECK(1);            /* 走到这儿说明没挂死 */
        fq_destroy(q2);
    }

    printf("== %d 个断言, %d 个失败 ==\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
