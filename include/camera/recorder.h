#ifndef RECORDER_H_
#define RECORDER_H_

#include <stddef.h>   /* size_t */

/* 录制器：把采集到的帧按顺序落盘。
 *
 * 录的是「原始帧」（YUYV 等），不是解码后的 RGB24——越早落盘越省空间，
 * 也省掉一次解码。文件里没有任何头，就是帧挨着帧的裸数据，所以读的时候
 * 得自己知道格式和宽高：
 *     ffmpeg -f rawvideo -pix_fmt yuyv422 -s 640x480 -i rec.raw out.mp4
 * 格式和宽高由调用方掌握，本模块不记录、也不校验。
 *
 * 不带线程：调用方在自己的循环里逐帧 rec_write，写盘是同步的。
 * （614400 字节/帧在 SSD 上约 1ms，30fps 下占用不到 5%，不值得为它开线程。） */

typedef struct recorder recorder;

/* 打开 path 准备录制，已存在则覆盖。失败返回 NULL（并打印原因）。 */
recorder *rec_create(const char *path);

/* 追加一帧：data 原样落盘，size 为字节数。成功 0，失败 -1。
 * 每帧大小应当一致（本模块不做校验，由调用方保证）。 */
int rec_write(recorder *rec, const void *data, size_t size);

/* 关闭文件并释放（fclose 会把 stdio 缓冲刷盘）。传 NULL 安全，可重复调用。 */
void rec_destroy(recorder *rec);

/* 已写入的帧数 / 字节数，用于收尾时汇报 */
unsigned long rec_frame_count(const recorder *rec);
size_t        rec_bytes_written(const recorder *rec);

#endif
