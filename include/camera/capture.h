#ifndef CAPTURE_H_
#define CAPTURE_H_
#include "camera/decoder.h"
#include "camera/frame_queue.h"
#include <stddef.h>   /* size_t */

typedef struct{
    const char *device;      // 设备路径，如 "/dev/video0"
    pixel_format fmt;        // 期望像素格式
    unsigned width;          // 期望宽
    unsigned height;         // 期望高
    unsigned buffer_count;   // mmap 内核缓冲数，通常 4
}capture_config;

typedef struct capture capture;

capture *cap_create(const capture_config *cfg);

void cap_destroy(capture *cap);

/* 启动后台采集线程（生产端）：把采集到的原始帧 push 进 queue
* queue 的 frame_size 须等于 cap_frame_size(cap)   成功 0，失败 -1*/
int cap_start(capture *cap, frame_queue *fq);

void cap_stop(capture *cap);

/* 查询实际协商出的参数（设备可能改掉请求值，以这些为准） */
pixel_format cap_format(const capture *cap);
unsigned cap_width(const capture *cap);
unsigned cap_height(const capture *cap);

/* 定长格式（YUYV/RGB24）下，一帧字节数；MJPEG 变长返回 0 */
size_t cap_frame_size(const capture *cap);

#endif