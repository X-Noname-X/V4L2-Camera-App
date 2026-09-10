/* apps/main.c：把 采集 → 队列 → 解码 → 输出 四模块串成完整程序（demo 版）。
 * 目前用 mock_capture（假采集）+ sink_null（丢帧）跑通整条链路，无硬件依赖。
 * 之后接真摄像头：把 CMake 里的 tests/unit/mock_capture.c 换成 src/capture.c，
 * 把 sink_null 换成 sink_sdl 即可。 */
#include <stdio.h>
#include <stdlib.h>
#include "camera/capture.h"
#include "camera/frame_queue.h"
#include "camera/decoder.h"
#include "camera/sink.h"

int main(int argc, char **argv)
{
    int num_frames = (argc > 1) ? atoi(argv[1]) : 60;   /* 默认跑 60 帧 */
    void *raw = NULL, *rgb = NULL;                      /* 提前声明，统一在 out 清理 */

    /* 1. 假采集：640x480 YUYV */
    capture_config cfg = { "mock", PIX_FMT_YUYV, 640, 480, 4 };
    capture *cap = cap_create(&cfg);
    if (!cap) { fprintf(stderr, "创建采集失败\n"); return 1; }

    /* 2. 原始帧队列（按协商出的一帧实际大小） */
    size_t raw_size = cap_frame_size(cap);
    frame_queue *raw_q = fq_create(8, raw_size, FQ_DROP_OLDEST);
    if (!raw_q) { cap_destroy(cap); return 1; }

    /* 3. 解码器：YUYV → RGB24 */
    decoder *dec = decoder_create(cap_format(cap), cap_width(cap), cap_height(cap));
    if (!dec) { fq_destroy(raw_q); cap_destroy(cap); return 1; }
    size_t rgb_size = decoder_output_size(dec);

    /* 4. 输出：null sink（丢弃帧，只计数） */
    frame_sink *sink = sink_null_create();
    if (!sink) { decoder_destroy(dec); fq_destroy(raw_q); cap_destroy(cap); return 1; }

    printf("采集格式 %s %ux%u，计划处理 %d 帧\n",
           pixel_format_name(cap_format(cap)), cap_width(cap), cap_height(cap), num_frames);

    /* 5. 起采集线程（生产端） */
    if (cap_start(cap, raw_q) != 0) {
        fprintf(stderr, "启动采集失败\n");
        goto out;
    }

    /* 6. 帧缓冲：一次分配，循环复用（热路径零 malloc） */
    raw = malloc(raw_size);
    rgb = malloc(rgb_size);
    if (!raw || !rgb) {
        fprintf(stderr, "分配帧缓冲失败\n");
        goto out;
    }

    /* 7. 主循环（消费端）：pop 原始帧 → 解码 → 输出 */
    int got = 0;
    for (int i = 0; i < num_frames; i++) {
        size_t sz = raw_size;
        if (fq_pop(raw_q, raw, &sz, 1000) != 0) break;   /* 超时/队列空 → 退出 */
        if (decoder_decode(dec, raw, sz, rgb, rgb_size) != 0) continue;
        sink_render(sink, rgb, rgb_size);
        got++;
    }
    printf("处理了 %d 帧（sink 收到 %lu 帧）\n", got, sink_null_frame_count(sink));

out:
    /* 8. 逆序清理 */
    free(raw);
    free(rgb);
    cap_stop(cap);           /* 幂等：未在运行直接返回 */
    sink_destroy(sink);
    decoder_destroy(dec);
    fq_destroy(raw_q);
    cap_destroy(cap);
    return 0;
}
