/* tests/integration/test_capture.c：真设备集成测试（需要一台摄像头）
 *
 * 跑的是 src/capture.c 的完整 V4L2 路径：
 *   QUERYCAP → S_FMT 协商 → REQBUFS → QUERYBUF+mmap+QBUF
 *   → STREAMON → 采集线程 DQBUF/推送/QBUF → STREAMOFF 让线程退出
 *
 * MJPEG（变长）和 YUYV（定长）都跑一遍，这两种帧在队列里走的是不同分支，
 * 只测一种的话另一种坏了没人知道
 *
 * 用法：test_capture [设备]     默认 /dev/video0
 *
 * 这里断言的是「假采集」永远测不到的东西：
 *   - 驱动协商出的格式和尺寸
 *   - 每帧长度不超过驱动缓冲区，内容是合法 JPEG（MJPEG）/ 长度精确（YUYV）
 *   - cap_stop 能否让阻塞在 DQBUF 的线程干净退出（写错就是永久挂死） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera/capture.h"
#include "camera/frame_queue.h"
#include "camera/decoder.h"

#define DEV_DEFAULT "/dev/video0"
#define REQ_W       640
#define REQ_H       480
#define BUF_COUNT   4
#define NFRAMES     10     // 连续取多少帧做检查

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond) do {                                          \
    tests_run++;                                                  \
    if (!(cond)) {                                                \
        tests_failed++;                                           \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                             \
} while (0)

static int has_jpeg_soi(const unsigned char *p, size_t n)
{
    return n >= 2 && p[0] == 0xFF && p[1] == 0xD8;
}

/* 把整套流程跑一遍，返回 -1 表示设备打不开（环境不具备，直接判失败） */
static int run_case(const char *dev, pixel_format fmt, const char *title)
{
    printf("--- %s ---\n", title);

    capture_config cfg = { dev, fmt, REQ_W, REQ_H, BUF_COUNT };
    capture *cap = cap_create(&cfg);
    if (!cap) {
        fprintf(stderr, "cap_create 失败：%s（具体原因见上一行）\n"
                        "常见原因：设备不存在、不是采集节点、"
                        "被别的进程占用、当前用户不在 video 组\n", dev);
        return -1;
    }

    size_t max_size = cap_max_frame_size(cap);
    printf("协商：%s %ux%u，缓冲 %zu 字节\n", pixel_format_name(cap_format(cap)),
           cap_width(cap), cap_height(cap), max_size);

    CHECK(cap_format(cap) == fmt);
    CHECK(cap_width(cap) > 0 && cap_height(cap) > 0);
    CHECK(max_size > 0);   // 来自 QUERYBUF，同时是队列槽容量和接收缓冲大小

    frame_queue *q = fq_create(8, max_size, FQ_DROP_OLDEST);
    CHECK(q != NULL);
    CHECK(fq_max_size(q) == max_size);

    decoder *dec = decoder_create(cap_format(cap), cap_width(cap), cap_height(cap));
    CHECK(dec != NULL);
    size_t rgb_size = decoder_output_size(dec);
    unsigned char *rgb = malloc(rgb_size);
    unsigned char *raw = malloc(max_size);
    CHECK(rgb != NULL && raw != NULL);

    CHECK(cap_start(cap, q) == 0);

    int got = 0, bad = 0;
    size_t min_sz = max_size, max_sz = 0;

    for (int i = 0; i < NFRAMES && raw; i++) {
        size_t sz = max_size;
        if (fq_pop(q, raw, &sz, 3000) != 0) break;   // 超时说明帧没流动
        got++;
        if (sz < min_sz) min_sz = sz;
        if (sz > max_sz) max_sz = sz;

        if (sz == 0 || sz > max_size) bad++;                      // 长度越界
        if (decoder_decode(dec, raw, sz, rgb, rgb_size)) bad++;   // 解不开

        if (fmt == PIX_FMT_MJPEG) {
            if (!has_jpeg_soi(raw, sz)) bad++;                    // 该是完整 JPEG
        } else {
            // YUYV 定长，每帧必须精确等于 宽*高*2
            if (sz != (size_t)REQ_W * REQ_H * 2) bad++;
        }
    }
    if (got) printf("帧长 %zu..%zu（槽 %zu）\n", min_sz, max_sz, max_size);

    CHECK(got == NFRAMES);   // 每帧都在 3s 内取到，说明帧确实在流动
    CHECK(bad == 0);

    free(raw);
    free(rgb);

    /* cap_stop 靠 STREAMOFF 让阻塞中的 DQBUF 返回错误，线程才退得出来
     * 这个机制写错了 pthread_join 会永久阻塞，挂在这儿就说明有 bug
     * （CTest 侧有 TIMEOUT 兜底，不会真挂住整个套件） */
    cap_stop(cap);
    CHECK(1);

    cap_stop(cap);   // 幂等，未在运行时应当安全返回
    CHECK(1);

    cap_destroy(cap);
    fq_destroy(q);
    decoder_destroy(dec);

    /* 另一条路径：运行中直接 cap_destroy（内部会自动停流）
     * 这条 apps/main.c 出错时走得到，也容易漏测 */
    capture *cap2 = cap_create(&cfg);
    CHECK(cap2 != NULL);
    if (cap2) {
        frame_queue *q2 = fq_create(4, cap_max_frame_size(cap2), FQ_DROP_OLDEST);
        CHECK(q2 != NULL);
        CHECK(cap_start(cap2, q2) == 0);
        cap_destroy(cap2);   // 不先 stop
        CHECK(1);            // 走到这儿说明没挂死
        fq_destroy(q2);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : DEV_DEFAULT;

    if (run_case(dev, PIX_FMT_MJPEG, "MJPEG（变长帧）") != 0) return 1;
    if (run_case(dev, PIX_FMT_YUYV,  "YUYV（定长帧）")  != 0) return 1;

    printf("== %d 个断言, %d 个失败 ==\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
