/* recorder：把原始帧顺序写入文件。
 * 不透明类型——struct recorder 只在本文件里定义，外部只能拿到指针。 */
#include "camera/recorder.h"

#include <stdio.h>
#include <stdlib.h>

struct recorder {
    FILE         *fp;       // 目标文件
    unsigned long frames;   // 已写入帧数
    size_t        bytes;    // 已写入字节数
};

recorder *rec_create(const char *path)
{
    recorder *rec;

    if (!path || !path[0]) return NULL;

    rec = calloc(1, sizeof(*rec));
    if (!rec) return NULL;

    /* "wb" 的 b 是 binary。Linux 上带不带 b 都一样，但写明意图更清楚，
     * 也避免哪天代码挪到别的平台时 EOL 被偷偷改写 */
    rec->fp = fopen(path, "wb");
    if (!rec->fp) {
        perror("fopen");        // 磁盘满、目录不存在、没权限都在这暴露
        free(rec);
        return NULL;
    }

    return rec;
}

int rec_write(recorder *rec, const void *data, size_t size)
{
    if (!rec || !rec->fp || !data || size == 0) return -1;

    /* 整帧一次写完。fwrite 返回的「元素个数」，这里元素大小取 1，
     * 所以返回值就等于实际写进去的字节数——比 size 小说明没写全。
     * stdio 自己有缓冲，这里不是每帧一次系统调用。 */
    if (fwrite(data, 1, size, rec->fp) != size) {
        fprintf(stderr, "录制写入失败：第 %lu 帧只写进 %zu 字节中的一部分\n",
                rec->frames + 1, size);
        return -1;
    }

    rec->frames++;
    rec->bytes += size;
    return 0;
}

void rec_destroy(recorder *rec)
{
    if (!rec) return;
    if (rec->fp) fclose(rec->fp);   /* fclose 会先刷缓冲，别在这里 free 掉缓冲 */
    free(rec);
}

unsigned long rec_frame_count(const recorder *rec) { return rec ? rec->frames : 0; }
size_t        rec_bytes_written(const recorder *rec) { return rec ? rec->bytes : 0; }
