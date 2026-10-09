#include "camera/decoder.h"

#include <stdlib.h>   // malloc / free
#include <stdint.h>   // uint8_t / uint16_t

/* MJPEG 依赖 libjpeg 的 API（jpeglib.h / jpeg_*）。
 *
 * 本文件要求 libjpeg-turbo：RGB565 直出用的是它的 JCS_RGB565 扩展，
 * IJG 原版没有这个枚举，编不过——这是故意的，编不过比悄悄出花屏好。
 * 另外别跟 TurboJPEG API（turbojpeg.h）混为一谈：那是另一套高层封装 */
#include <stdio.h>    // jpeglib.h 用到 FILE，必须先包含
#include <jpeglib.h>
#include <setjmp.h>

struct decoder {
    pixel_format fmt;
    unsigned width;
    unsigned height;
};

/* R/G/B(0-255) 打包成 RGB565。
 * 小端机上与 LVGL 的 RGB565 布局、以及 libjpeg-turbo 的 PACK_SHORT_565_LE
 * 一致（都是 R 占 bit15-11），所以解出来的缓冲可以直接送屏幕 */
static uint16_t pack565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

const char *pixel_format_name(pixel_format fmt)
{
    switch (fmt) {
    case PIX_FMT_YUYV:  return "YUYV";
    case PIX_FMT_MJPEG: return "MJPEG";
    case PIX_FMT_RGB24: return "RGB24";
    default:            return "unknown";
    }
}

decoder *decoder_create(pixel_format fmt, unsigned width, unsigned height)
{
    decoder *d;

    if (width == 0 || height == 0) return NULL;

    d = malloc(sizeof(*d));
    if (!d) return NULL;
    d->fmt = fmt;
    d->width = width;
    d->height = height;
    return d;
}

void decoder_destroy(decoder *d)
{
    free(d);
}

size_t decoder_output_size(const decoder *d)
{
    return d ? (size_t)d->width * d->height * 2 : 0;
}

// 把整数截回 [0,255]（BT.601 定点运算会溢出）
static int clamp_byte(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return v;
}

/* YUYV → RGB565，BT.601 全范围整数变换，避免浮点
 * YUYV 每 4 字节 = 相邻 2 像素（Y0 U Y1 V），要求 width 为偶数（V4L2 下恒成立） */
static int yuyv_to_rgb565(const uint8_t *src, size_t src_size, unsigned width,
                          unsigned height, uint8_t *dst, size_t dst_size)
{
    size_t npix = (size_t)width * height;
    uint16_t *out = (uint16_t *)dst;

    if (src_size < npix * 2) return -1;   // YUYV 每像素 2 字节
    if (dst_size < npix * 2) return -1;

    for (size_t i = 0; i < npix; i += 2) {
        int y0 = src[0], u = src[1], y1 = src[2], v = src[3];
        int d = u - 128, e = v - 128, c;
        src += 4;

        c = y0 - 16;
        *out++ = pack565(clamp_byte((298 * c + 409 * e + 128) >> 8),
                         clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8),
                         clamp_byte((298 * c + 516 * d + 128) >> 8));

        c = y1 - 16;
        *out++ = pack565(clamp_byte((298 * c + 409 * e + 128) >> 8),
                         clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8),
                         clamp_byte((298 * c + 516 * d + 128) >> 8));
    }
    return 0;
}

/* libjpeg 默认出错直接 exit()，对库是致命的，换成 setjmp 兜底返回错误码 */
struct jerr_mgr {
    struct jpeg_error_mgr pub;   // 必须是第一个成员
    jmp_buf jmp;
};

static void jerr_exit(j_common_ptr cinfo)
{
    struct jerr_mgr *e = (struct jerr_mgr *)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(e->jmp, 1);          // 跳回 setjmp 处
}

/* JPEG → RGB565，内存解码，不落盘。
 *
 * expect_w/expect_h 非 0 时要求文件尺寸与之完全一致——摄像头场景用这个：
 * 大小帧混进来会让后续按声明尺寸推算的缓冲、缩放表全部错位。
 * 相册读照片时尺寸事先不知道，传 0 表示不校验、以文件头为准。
 *
 * out_w/out_h 非 NULL 时写回实际解码尺寸（与 expect 校验用的是同一组值）。 */
static int jpeg_to_rgb565(const uint8_t *src, size_t src_size,
                          unsigned expect_w, unsigned expect_h,
                          uint8_t *dst, size_t dst_size,
                          unsigned *out_w, unsigned *out_h)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr_mgr jerr;
    uint8_t *row = dst;
    /* volatile：这两个变量在 setjmp 之后被改过，longjmp 跳回来时
     * 只有 volatile 才能保证值不被优化掉（见 C11 7.13.2.1） */
    volatile int created = 0, rc = -1;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jerr_exit;
    if (setjmp(jerr.jmp)) goto out;   // 解压中出错，跳到清理

    jpeg_create_decompress(&cinfo);
    created = 1;
    jpeg_mem_src(&cinfo, (unsigned char *)src, (unsigned long)src_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) goto out;

    /* 尺寸一律以文件头为准：校验和缓冲容量检查都用它，
     * 不能用调用方声明的值——那种在 expect 为 0 时根本不存在 */
    const unsigned w = cinfo.image_width;
    const unsigned h = cinfo.image_height;

    if (expect_w && (w != expect_w || h != expect_h)) goto out;
    if (dst_size < (size_t)w * h * 2) goto out;

    /* JCS_RGB565 让色彩转换在库内部完成、且走合并上采样器的快路径
     * （条件见 jdmaster.c 的 use_merged_upsample()），调用方不必再转一遍。
     * 代价是色度改用箱式滤波而非平滑插值——缩到屏幕尺寸后看不出来 */
    cinfo.out_color_space = JCS_RGB565;
    cinfo.do_fancy_upsampling = FALSE;

    jpeg_start_decompress(&cinfo);
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *rowptr[1] = { row };
        jpeg_read_scanlines(&cinfo, rowptr, 1);
        row += cinfo.output_width * 2;
    }
    jpeg_finish_decompress(&cinfo);

    /* 只在成功路径上写回，此时不可能发生过 longjmp */
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    rc = 0;

out:
    if (created) jpeg_destroy_decompress(&cinfo);
    return rc;
}

int decoder_decode(decoder *d, const void *src, size_t src_size,
                   void *dst, size_t dst_size)
{
    if (!d || !src || !dst) return -1;

    switch (d->fmt) {
    case PIX_FMT_YUYV:
        return yuyv_to_rgb565(src, src_size, d->width, d->height, dst, dst_size);
    case PIX_FMT_MJPEG:
        /* expect 传协商尺寸：采集来的帧必须和协商值一致 */
        return jpeg_to_rgb565(src, src_size, d->width, d->height,
                              dst, dst_size, NULL, NULL);
    case PIX_FMT_RGB24: {
        /* 输入已是 RGB24，逐像素打包成 RGB565 */
        size_t npix = (size_t)d->width * d->height;
        if (src_size < npix * 3 || dst_size < npix * 2) return -1;
        const uint8_t *s = src;
        uint16_t *o = dst;
        for (size_t i = 0; i < npix; i++, s += 3)
            *o++ = pack565(s[0], s[1], s[2]);
        return 0;
    }
    default:
        return -1;
    }
}

int decoder_decode_jpeg(const void *src, size_t src_size,
                        void *dst, size_t dst_size,
                        unsigned *out_w, unsigned *out_h)
{
    if (!src || src_size == 0 || !dst) return -1;
    /* expect 传 0 = 不校验尺寸，dst_size 用文件头里的真实尺寸检查 */
    return jpeg_to_rgb565(src, src_size, 0, 0, dst, dst_size, out_w, out_h);
}

int decoder_jpeg_size(const void *src, size_t src_size,
                      unsigned *out_w, unsigned *out_h)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr_mgr jerr;
    /* volatile 的理由同 jpeg_to_rgb565 */
    volatile int created = 0, rc = -1;

    if (!src || src_size == 0) return -1;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jerr_exit;
    if (setjmp(jerr.jmp)) goto out;

    jpeg_create_decompress(&cinfo);
    created = 1;
    jpeg_mem_src(&cinfo, (unsigned char *)src, (unsigned long)src_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) goto out;

    if (out_w) *out_w = cinfo.image_width;
    if (out_h) *out_h = cinfo.image_height;
    rc = 0;

out:
    if (created) jpeg_destroy_decompress(&cinfo);
    return rc;
}
