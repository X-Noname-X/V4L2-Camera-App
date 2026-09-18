#ifndef CAPTURE_H_
#define CAPTURE_H_
#include "camera/decoder.h"
#include "camera/frame_queue.h"
#include <stddef.h>   // size_t

typedef struct {
    const char *device;      // 设备路径，如 "/dev/video0"
    pixel_format fmt;        // 期望像素格式
    unsigned width;          // 期望宽
    unsigned height;         // 期望高
    unsigned buffer_count;   // mmap 内核缓冲数，通常 4
} capture_config;

typedef struct capture capture;

capture *cap_create(const capture_config *cfg);

void cap_destroy(capture *cap);

/* 启动后台采集线程（生产端）：把采集到的原始帧 push 进 queue
 *
 * queue 的 max_size 须 >= cap_max_frame_size(cap)，cap_start 会校验这一点，
 * 不满足直接返回 -1，因为这个值一旦偏小，采集线程里 fq_push 会失败而帧被
 * **静默丢掉**（采不到画面，但没有任何报错）
 * 成功 0，失败 -1 */
int cap_start(capture *cap, frame_queue *fq);

void cap_stop(capture *cap);

/* 查询实际协商出的参数（设备可能改掉请求值，以这些为准） */
pixel_format cap_format(const capture *cap);
unsigned cap_width(const capture *cap);
unsigned cap_height(const capture *cap);

/* 驱动一帧最多写多少字节，即 QUERYBUF 报出来的缓冲区长度
 *
 * 这是 MJPEG 这类变长格式下唯一的可靠上界（每帧实际长度都不同），但它也
 * 只是上界：驱动对压缩格式同样按「不压缩」预留，所以它比真实帧长大得多，
 * 按它设队列槽容量会很浪费内存 */
size_t cap_max_frame_size(const capture *cap);

#endif