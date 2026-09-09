/* mock_capture：实现 capture.h 接口的假采集，不碰 /dev/video0。
 * 采集线程按 ~30fps 生成灰度随帧号变化的 YUYV/RGB24 帧 push 进队列，
 * 用于无摄像头环境下测试/演示整条采集链路。
 * 与 src/capture.c 定义同一批函数，二者不能链接进同一个可执行文件。 */
#define _POSIX_C_SOURCE 200809L   /* nanosleep */

#include "camera/capture.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>   /* nanosleep */

struct capture {
    pixel_format fmt;
    unsigned width, height;
    frame_queue *fq;
    pthread_t thread;
    _Atomic int running;       /* 线程读、主线程写，用原子类型避免数据竞争 */
    unsigned long frame_count; /* 已生成的帧序号（单调递增，仅采集线程访问） */
};

/* 生成第 frame 帧：灰度 Y = frame & 0xFF，色度 U/V = 128（无色偏） */
static void gen_frame(capture *cap, unsigned char *buf, unsigned long frame)
{
    size_t npix = (size_t)cap->width * cap->height;
    unsigned char y = (unsigned char)(frame & 0xFF);

    switch (cap->fmt) {
    case PIX_FMT_YUYV:
        for (size_t i = 0; i < npix; i++) {
            buf[2 * i]     = y;    /* Y */
            buf[2 * i + 1] = 128;  /* U / V */
        }
        break;
    case PIX_FMT_RGB24:
        for (size_t i = 0; i < npix; i++) {
            buf[3 * i]     = y;
            buf[3 * i + 1] = y;
            buf[3 * i + 2] = y;
        }
        break;
    default:
        break;
    }
}

static void *capture_thread(void *arg)
{
    capture *cap = arg;
    size_t frame_size = cap_frame_size(cap);
    unsigned char *frame = malloc(frame_size);
    const struct timespec ts = { 0, 33 * 1000 * 1000 };   /* 33ms ≈ 30fps */
    if (!frame) return NULL;

    while (atomic_load(&cap->running)) {
        gen_frame(cap, frame, cap->frame_count++);
        fq_push(cap->fq, frame, frame_size);
        nanosleep(&ts, NULL);
    }
    free(frame);
    return NULL;
}

capture *cap_create(const capture_config *cfg)
{
    capture *cap;

    if (!cfg || cfg->width == 0 || cfg->height == 0) return NULL;
    if (cfg->fmt == PIX_FMT_MJPEG) return NULL;   /* 变长格式，mock 不支持 */

    cap = calloc(1, sizeof(*cap));
    if (!cap) return NULL;
    cap->fmt = cfg->fmt;
    cap->width = cfg->width;
    cap->height = cfg->height;
    return cap;
}

void cap_destroy(capture *cap)
{
    if (!cap) return;
    if (atomic_load(&cap->running))
        cap_stop(cap);
    free(cap);
}

int cap_start(capture *cap, frame_queue *fq)
{
    if (!cap || !fq) return -1;
    if (atomic_load(&cap->running)) return -1;   /* 已在运行 */
    if (fq_frame_size(fq) != cap_frame_size(cap)) return -1;

    cap->fq = fq;
    atomic_store(&cap->running, 1);
    if (pthread_create(&cap->thread, NULL, capture_thread, cap) != 0) {
        atomic_store(&cap->running, 0);   /* 回滚 */
        return -1;
    }
    return 0;
}

void cap_stop(capture *cap)
{
    if (!cap || !atomic_load(&cap->running)) return;   /* 幂等 */
    atomic_store(&cap->running, 0);      /* 线程看到后退出循环 */
    pthread_join(cap->thread, NULL);
}

/* getters */
pixel_format cap_format(const capture *cap) { return cap ? cap->fmt : 0; }
unsigned     cap_width(const capture *cap)  { return cap ? cap->width : 0; }
unsigned     cap_height(const capture *cap) { return cap ? cap->height : 0; }

size_t cap_frame_size(const capture *cap)
{
    if (!cap) return 0;
    switch (cap->fmt) {
    case PIX_FMT_YUYV:  return (size_t)cap->width * cap->height * 2;
    case PIX_FMT_RGB24: return (size_t)cap->width * cap->height * 3;
    default:            return 0;
    }
}
