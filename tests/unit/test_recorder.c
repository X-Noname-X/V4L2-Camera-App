/* recorder 单元测试：录制器是纯文件 I/O，不碰硬件，所以这里能完整测。
 * 除了计数，重点验「落盘的内容和写进去的逐字节一致」——录制的全部意义就在这。
 * 断言 17 个。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera/recorder.h"

#define OUT_PATH "test_recorder_out.raw"
#define FRAME_SZ 256
#define NFRAMES  7

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond) do {                                          \
    tests_run++;                                                  \
    if (!(cond)) {                                                \
        tests_failed++;                                           \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                             \
} while (0)

int main(void)
{
    unsigned char frame[FRAME_SZ];
    unsigned char back[FRAME_SZ];

    /* ---------- 1. 参数校验 ---------- */
    CHECK(rec_create(NULL) == NULL);
    CHECK(rec_create("")   == NULL);

    /* 打不开的路径也应当安全返回 NULL（这里会打一行 perror，属预期） */
    CHECK(rec_create("/nonexistent_dir_xyz/rec.raw") == NULL);

    /* ---------- 2. 创建并写帧 ---------- */
    recorder *rec = rec_create(OUT_PATH);
    CHECK(rec != NULL);
    CHECK(rec_frame_count(rec) == 0);
    CHECK(rec_bytes_written(rec) == 0);

    /* 第 i 帧整帧填成 i，回读时就能逐字节核对是哪一帧、有没有错位 */
    for (int i = 0; i < NFRAMES; i++) {
        memset(frame, i, FRAME_SZ);
        CHECK(rec_write(rec, frame, FRAME_SZ) == 0);
    }
    CHECK(rec_frame_count(rec) == NFRAMES);
    CHECK(rec_bytes_written(rec) == (size_t)NFRAMES * FRAME_SZ);

    /* ---------- 3. 非法参数不该被算进计数 ---------- */
    memset(frame, 0xAA, FRAME_SZ);
    CHECK(rec_write(NULL, frame, FRAME_SZ) == -1);   /* 句柄为空 */
    CHECK(rec_write(rec, NULL, FRAME_SZ) == -1);     /* 数据为空 */
    CHECK(rec_write(rec, frame, 0) == -1);           /* 长度为 0 */
    CHECK(rec_frame_count(rec) == NFRAMES);          /* 三次都没写进去 */
    CHECK(rec_bytes_written(rec) == (size_t)NFRAMES * FRAME_SZ);

    /* ---------- 4. 关闭（fclose 负责刷盘） ---------- */
    rec_destroy(rec);
    rec_destroy(NULL);   /* 传 NULL 应当安全 */

    /* ---------- 5. 回读文件，逐字节核对 ---------- */
    FILE *fp = fopen(OUT_PATH, "rb");
    CHECK(fp != NULL);
    if (fp) {
        int bad_frames = 0, read_bytes = 0;

        for (int i = 0; i < NFRAMES; i++) {
            memset(back, 0, FRAME_SZ);
            size_t n = fread(back, 1, FRAME_SZ, fp);
            if (n != FRAME_SZ) { bad_frames++; continue; }
            read_bytes += (int)n;
            for (size_t j = 0; j < FRAME_SZ; j++) {
                if (back[j] != (unsigned char)i) { bad_frames++; break; }
            }
        }
        CHECK(read_bytes == NFRAMES * FRAME_SZ);
        CHECK(bad_frames == 0);        /* 每一帧都逐字节吻合，没错位也没丢帧 */
        CHECK(fgetc(fp) == EOF);       /* 末尾没有多余内容 */
        fclose(fp);
    }

    remove(OUT_PATH);   /* 不留垃圾文件 */

    printf("== %d 个断言, %d 个失败 ==\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
