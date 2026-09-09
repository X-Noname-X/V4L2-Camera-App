/* decoder 单元测试：验证 YUYV→RGB24 与 RGB24 直通（MJPEG 需 libjpeg，集成阶段再测） */
#include <stdio.h>
#include <string.h>
#include "camera/decoder.h"

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond) do {                                          \
    tests_run++;                                                  \
    if (!(cond)) {                                                \
        tests_failed++;                                           \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                             \
} while (0)

/* 构造 2x2 的 YUYV 帧（8 字节），所有像素亮度为 y、色度 U=V=128（无色偏） */
static void make_yuyv(unsigned char *src, unsigned char y)
{
    for (int i = 0; i < 8; i++)
        src[i] = (i % 2 == 0) ? y : 128;
}

/* 解码并断言整帧 RGB 统一为 expected */
static int check_uniform(decoder *d, unsigned char y, int expected)
{
    unsigned char src[8], out[12];
    make_yuyv(src, y);
    if (decoder_decode(d, src, sizeof(src), out, sizeof(out)) != 0) return 0;
    for (int i = 0; i < 12; i++)
        if (out[i] != (unsigned char)expected) return 0;
    return 1;
}

static void test_pixel_format_name(void)
{
    CHECK(strcmp(pixel_format_name(PIX_FMT_YUYV), "YUYV") == 0);
    CHECK(strcmp(pixel_format_name(PIX_FMT_MJPEG), "MJPEG") == 0);
    CHECK(strcmp(pixel_format_name(PIX_FMT_RGB24), "RGB24") == 0);
    CHECK(strcmp(pixel_format_name((pixel_format)99), "unknown") == 0);
}

static void test_yuyv_conversion(void)
{
    decoder *d = decoder_create(PIX_FMT_YUYV, 2, 2);
    CHECK(d != NULL);
    CHECK(decoder_output_size(d) == 12);

    CHECK(check_uniform(d, 16, 0));     /* 黑：Y=16  -> RGB 全 0 */
    CHECK(check_uniform(d, 235, 255));  /* 白：Y=235 -> RGB 全 255 */
    CHECK(check_uniform(d, 128, 130));  /* 中灰：Y=128 -> RGB 全 130 */

    decoder_destroy(d);
}

static void test_rgb24_passthrough(void)
{
    decoder *d = decoder_create(PIX_FMT_RGB24, 2, 2);
    CHECK(d != NULL);

    unsigned char in[12], out[12];
    for (int i = 0; i < 12; i++) in[i] = (unsigned char)i;
    CHECK(decoder_decode(d, in, sizeof(in), out, sizeof(out)) == 0);
    CHECK(memcmp(in, out, sizeof(in)) == 0);

    decoder_destroy(d);
}

static void test_invalid_args(void)
{
    CHECK(decoder_create(PIX_FMT_YUYV, 0, 2) == NULL);   /* 宽度 0 */
    CHECK(decoder_create(PIX_FMT_YUYV, 2, 0) == NULL);   /* 高度 0 */

    decoder *d = decoder_create(PIX_FMT_YUYV, 2, 2);
    unsigned char src[8], out[12];
    make_yuyv(src, 128);

    CHECK(decoder_decode(NULL, src, 8, out, 12) == -1);  /* 解码器为空 */
    CHECK(decoder_decode(d, NULL, 8, out, 12) == -1);    /* 源为空 */
    CHECK(decoder_decode(d, src, 8, NULL, 12) == -1);    /* 目标为空 */
    CHECK(decoder_decode(d, src, 4, out, 12) == -1);     /* src 太小（< 8） */
    CHECK(decoder_decode(d, src, 8, out, 6) == -1);      /* dst 太小（< 12） */

    decoder_destroy(d);
}

static void test_mjpeg_graceful(void)
{
    /* 有无 libjpeg 都不应崩溃：无则返回 NULL，有则能创建并销毁 */
    decoder *d = decoder_create(PIX_FMT_MJPEG, 2, 2);
    if (d) decoder_destroy(d);
    CHECK(1);
}

int main(void)
{
    test_pixel_format_name();
    test_yuyv_conversion();
    test_rgb24_passthrough();
    test_invalid_args();
    test_mjpeg_graceful();

    printf("== %d 个断言, %d 个失败 ==\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
