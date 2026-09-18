/* open/O_EXCL/fdopen/close 都是 POSIX，不加这句编译器看不到声明，
 * 会按隐式 int 调，指针被截断成 32 位 */
#define _POSIX_C_SOURCE 200809L

/* 采集 → 队列 → 解码 → 预览，可选把原始帧录到文件
 *
 * 本仓库的演示程序，不是库的一部分，真正的产品（比如 LCD 上的显示）应该
 * 只链接 libcamera.a，自己决定把数据送去哪里
 *
 * 用法见 --help */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>    // open / O_EXCL
#include <unistd.h>   // close
#include <errno.h>
#include <getopt.h>

#include "camera/capture.h"
#include "camera/frame_queue.h"
#include "camera/decoder.h"
#include "preview.h"

#define NS_PER_SEC 1000000000ULL

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);   // 单调时钟，不受系统时间被改影响
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

/* 存一帧原始采集数据，MJPEG 的帧本身就是一张完整的 JPEG，直接落盘即可，
 * 别的格式的帧不是 JPEG，存出来打不开，所以调用方要先判格式
 * 文件名自动往后找空位，绝不覆盖已有文件 */
static int save_photo(const char *prefix, const void *data, size_t size)
{
    char path[128];
    int fd;

    for (int i = 1; ; i++) {
        snprintf(path, sizeof(path), "%s_%04d.jpg", prefix, i);
        /* O_EXCL 的语义正是「文件已存在就失败」，而且是原子的，
         * 所以换多少次运行都不会覆盖之前拍的照片 */
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) break;                    // 抢到了这个编号
        if (errno != EEXIST) { perror("open"); return -1; }
    }

    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        perror("fdopen");
        close(fd);
        remove(path);
        return -1;
    }

    if (fwrite(data, 1, size, fp) != size || fclose(fp) != 0) {
        fprintf(stderr, "保存照片失败：%s\n", path);
        remove(path);                          // 别留半截文件
        return -1;
    }

    printf("已保存 %s（%zu 字节）\n", path, size);
    return 0;
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
        "用法：%s [选项]\n"
        "  -d, --device=PATH  采集设备（默认 /dev/video0）\n"
        "  -n, --frames=N     处理 N 帧后退出；0 = 不限（默认 0）\n"
        "  -r, --record=PATH  把原始帧录到文件（可与预览同时进行）\n"
        "  -f, --format=NAME  像素格式：mjpeg / yuyv / rgb24（默认 mjpeg）\n"
        "  -s, --size=WxH     分辨率（默认 640x480）\n"
        "  -h, --help         显示本帮助\n"
        "\n"
        "运行时：空格 = 存一张 photo_000N.jpg（仅 MJPEG），ESC 或关窗口 = 退出\n"
        "\n"
        "例：%s --frames=150 --record=rec.mjpg\n"
        "    %s --format=yuyv --size=1280x720\n", prog, prog, prog);
}

/* 解析「不小于 0 的整数」选项，用 strtol 而不是 atoi：atoi 遇到乱输会
 * 静默返回 0，而 0 恰好是「不限帧数」，打错一个字就变成跑个没完 */
static int parse_uint(const char *arg, const char *what, int *out)
{
    char *end;
    long v = strtol(arg, &end, 10);
    if (*arg == '\0' || *end != '\0' || v < 0 || v > INT_MAX) {
        fprintf(stderr, "%s必须是不小于 0 的整数：%s\n", what, arg);
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int parse_format(const char *arg, pixel_format *out)
{
    if (!strcmp(arg, "mjpeg")) *out = PIX_FMT_MJPEG;
    else if (!strcmp(arg, "yuyv")) *out = PIX_FMT_YUYV;
    else if (!strcmp(arg, "rgb24")) *out = PIX_FMT_RGB24;
    else { fprintf(stderr, "未知格式：%s（mjpeg / yuyv / rgb24）\n", arg); return -1; }
    return 0;
}

/* 解析 "宽x高"，strtol 显式传了基数 10，所以不会被 0x 之类的当成十六进制 */
static int parse_size(const char *arg, unsigned *w, unsigned *h)
{
    char *end;
    long ww = strtol(arg, &end, 10);
    if (*end != 'x' && *end != 'X') goto bad;
    long hh = strtol(end + 1, &end, 10);
    if (*end || ww <= 0 || hh <= 0 || ww > 65535 || hh > 65535) goto bad;
    *w = (unsigned)ww;
    *h = (unsigned)hh;
    return 0;
bad:
    fprintf(stderr, "尺寸要写成 宽x高，如 640x480：%s\n", arg);
    return -1;
}

int main(int argc, char **argv)
{
    static const struct option long_opts[] = {
        { "device", required_argument, NULL, 'd' },
        { "frames", required_argument, NULL, 'n' },
        { "record", required_argument, NULL, 'r' },
        { "format", required_argument, NULL, 'f' },
        { "size",   required_argument, NULL, 's' },
        { "help",   no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    const char *device = "/dev/video0";
    const char *rec_path = NULL;        // NULL = 不录制
    int num_frames = 0;                 // 0 = 不限
    pixel_format fmt = PIX_FMT_MJPEG;
    unsigned width = 640, height = 480;

    int opt;
    while ((opt = getopt_long(argc, argv, "d:n:r:f:s:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 'r': rec_path = optarg; break;
        case 'n': if (parse_uint(optarg, "帧数", &num_frames) != 0) return 1; break;
        case 'f': if (parse_format(optarg, &fmt) != 0) return 1; break;
        case 's': if (parse_size(optarg, &width, &height) != 0) return 1; break;
        case 'h': usage(stdout, argv[0]); return 0;
        default:  usage(stderr, argv[0]); return 1;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "多余的参数：%s\n", argv[optind]);
        usage(stderr, argv[0]);
        return 1;
    }

    /* 所有资源先置空，失败时就地 goto out 统一回收 */
    capture *cap = NULL;
    frame_queue *raw_q = NULL;
    decoder *dec = NULL;
    preview *pv = NULL;
    FILE *rec_fp = NULL;
    void *raw = NULL, *rgb = NULL;
    int rc = 1;

    /* 驱动可能协商成别的格式和尺寸，以 cap_* 查询到的为准 */
    capture_config cfg = { device, fmt, width, height, 4 };
    cap = cap_create(&cfg);
    if (!cap) {
        fprintf(stderr, "打开采集设备失败：%s（具体原因见上一行）\n", device);
        goto out;
    }

    /* MJPEG 每帧长度不定，所以槽按「驱动最多能写多少」来设，
     * 而不是按某一帧的实际长度 */
    size_t slot_size = cap_max_frame_size(cap);
    raw_q = fq_create(8, slot_size, FQ_DROP_OLDEST);
    if (!raw_q) { fprintf(stderr, "创建队列失败\n"); goto out; }

    dec = decoder_create(cap_format(cap), cap_width(cap), cap_height(cap));
    if (!dec) { fprintf(stderr, "创建解码器失败\n"); goto out; }
    size_t rgb_size = decoder_output_size(dec);

    pv = preview_open(cap_width(cap), cap_height(cap));
    if (!pv) goto out;

    unsigned long rec_frames = 0;
    if (rec_path) {
        rec_fp = fopen(rec_path, "wb");
        if (!rec_fp) { perror("fopen"); goto out; }
    }

    printf("采集 %s %ux%u：接收缓冲 %zu 字节，RGB %zu 字节/帧\n",
           pixel_format_name(cap_format(cap)), cap_width(cap), cap_height(cap),
           slot_size, rgb_size);
    printf("空格 = 拍照，ESC 或关窗口 = 退出\n");

    /* 起采集线程，并分配两块循环复用的缓冲（热路径零 malloc） */
    if (cap_start(cap, raw_q) != 0) { fprintf(stderr, "启动采集失败\n"); goto out; }
    raw = malloc(slot_size);
    rgb = malloc(rgb_size);
    if (!raw || !rgb) { fprintf(stderr, "分配帧缓冲失败\n"); goto out; }

    uint64_t t0 = now_ns();
    int got = 0, quit = 0;

    /* i 是「取了多少帧」的循环计数，不是 got：解码失败的帧也要算进去，
     * 否则画面一直解不开时这个循环永远退不出来 */
    for (int i = 0; !quit && (num_frames == 0 || i < num_frames); i++) {
        size_t sz = slot_size;
        if (fq_pop(raw_q, raw, &sz, 1000) != 0) break;   // 超时/队列空 → 退出

        /* 录像失败（磁盘满等）不打断预览：停掉录制，画面继续 */
        if (rec_fp) {
            if (fwrite(raw, 1, sz, rec_fp) == sz) {
                rec_frames++;
            } else {
                fprintf(stderr, "录制中断，继续预览但不再录制\n");
                fclose(rec_fp);
                rec_fp = NULL;
            }
        }

        if (decoder_decode(dec, raw, sz, rgb, rgb_size) != 0) continue;

        preview_action action = preview_show(pv, rgb);
        if (action == PREVIEW_QUIT) { quit = 1; break; }

        /* 只有 MJPEG 的帧本身是完整 JPEG，别的格式直接落盘会得到一个
         * 名字叫 .jpg 但内容不是 JPEG 的坏文件，所以这里拦住 */
        if (action == PREVIEW_SNAPSHOT) {
            if (cap_format(cap) == PIX_FMT_MJPEG)
                save_photo("photo", raw, sz);
            else
                fprintf(stderr, "当前是 %s，只有 MJPEG 能直接存成 .jpg\n",
                        pixel_format_name(cap_format(cap)));
        }
        got++;
    }

    double secs = (double)(now_ns() - t0) / (double)NS_PER_SEC;
    unsigned long dropped = fq_dropped(raw_q);
    unsigned long total = (unsigned long)got + dropped;
    printf("完成：%d 帧，平均 %.1f fps，丢帧 %lu（%.1f%%）\n",
           got, secs > 0.0 ? (double)got / secs : 0.0,
           dropped, total ? 100.0 * (double)dropped / (double)total : 0.0);
    if (rec_fp)
        printf("录制：%s，%lu 帧\n", rec_path, rec_frames);
    rc = 0;

out:
    /* 逆序清理，每项都判空、都幂等，从任何一步跳进来都安全 */
    free(raw);
    free(rgb);
    if (cap) cap_stop(cap);          // 幂等：未在运行直接返回
    if (rec_fp) fclose(rec_fp);
    if (pv) preview_close(pv);
    if (dec) decoder_destroy(dec);
    if (raw_q) fq_destroy(raw_q);
    if (cap) cap_destroy(cap);
    return rc;
}
