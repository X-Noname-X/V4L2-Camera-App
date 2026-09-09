#include "camera/decoder.h"

#include <stdlib.h>   /* malloc / free */
#include <string.h>   /* memcpy */
#include <stdint.h>   /* uint8_t */

/* MJPEG 依赖 libjpeg(-turbo)；CMake 找到该库时定义 HAVE_LIBJPEG */
#ifdef HAVE_LIBJPEG
#include <stdio.h>    /* jpeglib.h 用到 FILE，需先包含 */
#include <jpeglib.h>
#include <setjmp.h>
#endif

struct decoder {
    pixel_format fmt;
    unsigned width;
    unsigned height;
};

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
#ifndef HAVE_LIBJPEG
    if (fmt == PIX_FMT_MJPEG) return NULL;   /* 未编译 libjpeg，MJPEG 不可用 */
#endif

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
    return d ? (size_t)d->width * d->height * 3 : 0;
}

/* 把整数结果截回 [0,255]（BT.601 定点运算可能溢出） */
static int clamp_byte(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return v;
}

/* YUYV → RGB24。BT.601 全范围整数变换，避免浮点。
 * YUYV 每 4 字节 = 相邻 2 像素（Y0 U Y1 V），要求 width 为偶数（V4L2 下恒成立）。 */
static int yuyv_to_rgb24(const uint8_t *src, size_t src_size, unsigned width,
                         unsigned height, uint8_t *dst, size_t dst_size)
{
    size_t npix = (size_t)width * height;

    if (src_size < npix * 2) return -1;   /* YUYV 每像素 2 字节 */
    if (dst_size < npix * 3) return -1;

    for (size_t i = 0; i < npix; i += 2) {
        int y0 = src[0], u = src[1], y1 = src[2], v = src[3];
        int d = u - 128, e = v - 128, c;
        src += 4;

        c = y0 - 16;
        *dst++ = (uint8_t)clamp_byte((298 * c + 409 * e + 128) >> 8);
        *dst++ = (uint8_t)clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
        *dst++ = (uint8_t)clamp_byte((298 * c + 516 * d + 128) >> 8);

        c = y1 - 16;
        *dst++ = (uint8_t)clamp_byte((298 * c + 409 * e + 128) >> 8);
        *dst++ = (uint8_t)clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
        *dst++ = (uint8_t)clamp_byte((298 * c + 516 * d + 128) >> 8);
    }
    return 0;
}

#ifdef HAVE_LIBJPEG

/* libjpeg 默认出错直接 exit()，对库是致命的；换成 setjmp 兜底返回错误码 */
struct jerr_mgr {
    struct jpeg_error_mgr pub;   /* 必须是第一个成员 */
    jmp_buf jmp;
};

static void jerr_exit(j_common_ptr cinfo)
{
    struct jerr_mgr *e = (struct jerr_mgr *)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(e->jmp, 1);          /* 跳回 setjmp 处 */
}

/* MJPEG → RGB24：内存解码，不落盘 */
static int mjpeg_to_rgb24(const uint8_t *src, size_t src_size, unsigned width,
                          unsigned height, uint8_t *dst, size_t dst_size)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr_mgr jerr;
    uint8_t *row = dst;
    int created = 0, rc = -1;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jerr_exit;
    if (setjmp(jerr.jmp)) goto out;   /* 解压中出错，跳到清理 */

    jpeg_create_decompress(&cinfo);
    created = 1;
    jpeg_mem_src(&cinfo, (unsigned char *)src, (unsigned long)src_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) goto out;

    /* 分辨率必须与采集端协商的一致，否则直接判失败 */
    if (cinfo.image_width != width || cinfo.image_height != height) goto out;
    if (dst_size < (size_t)width * height * 3) goto out;

    cinfo.out_color_space = JCS_RGB;   /* 输出 3 字节 RGB */
    jpeg_start_decompress(&cinfo);
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *rowptr[1] = { row };
        jpeg_read_scanlines(&cinfo, rowptr, 1);
        row += cinfo.output_width * 3;
    }
    jpeg_finish_decompress(&cinfo);
    rc = 0;

out:
    if (created) jpeg_destroy_decompress(&cinfo);
    return rc;
}

#endif /* HAVE_LIBJPEG */

int decoder_decode(decoder *d, const void *src, size_t src_size,
                   void *dst, size_t dst_size)
{
    if (!d || !src || !dst) return -1;

    switch (d->fmt) {
    case PIX_FMT_YUYV:
        return yuyv_to_rgb24(src, src_size, d->width, d->height, dst, dst_size);
    case PIX_FMT_MJPEG:
#ifdef HAVE_LIBJPEG
        return mjpeg_to_rgb24(src, src_size, d->width, d->height, dst, dst_size);
#else
        return -1;   /* 编译时未启用 MJPEG */
#endif
    case PIX_FMT_RGB24:
        if (src_size > dst_size) return -1;
        memcpy(dst, src, src_size);
        return 0;
    default:
        return -1;
    }
}
