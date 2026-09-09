#ifndef DECODER_H_
#define DECODER_H_

#include <stddef.h>   /* size_t */

/* 像素格式：V4L2 采集端输出的原始帧格式，解码器统一转成 RGB24 */
typedef enum {
    PIX_FMT_YUYV,   /* YUYV 4:2:2 交错打包，绝大多数 UVC 摄像头默认格式 */
    PIX_FMT_MJPEG,  /* JPEG 压缩帧，解码需链接 libjpeg(-turbo) */
    PIX_FMT_RGB24,  /* 已是 RGB24，直通复制 */
} pixel_format;

/* 不透明类型：内部结构只在 decoder.c 定义 */
typedef struct decoder decoder;

/* 创建解码器，输出固定为 RGB24（width*height*3 字节）。
 * fmt/width/height 必须与采集端协商出的分辨率一致。
 * MJPEG 若未编译 libjpeg 支持则返回 NULL。
 * 成功返回解码器指针，失败返回 NULL。 */
decoder *decoder_create(pixel_format fmt, unsigned width, unsigned height);

/* 释放解码器 */
void decoder_destroy(decoder *d);

/* 将一帧 src 解码为 RGB24 写入 dst。
 *   src/src_size : 输入帧数据及其字节数（YUYV 应为 width*height*2）
 *   dst/dst_size : 输出缓冲及其容量（至少 width*height*3）
 * 成功返回 0，失败返回 -1（参数非法 / 数据损坏 / 缓冲不足）。 */
int decoder_decode(decoder *d, const void *src, size_t src_size,
                   void *dst, size_t dst_size);

/* 解码后一帧 RGB24 的字节数 = width*height*3 */
size_t decoder_output_size(const decoder *d);

/* 像素格式的可读名字（日志用） */
const char *pixel_format_name(pixel_format fmt);

#endif /* DECODER_H_ */
