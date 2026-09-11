/* apps/main.c：把 采集 → 队列 → 解码 → 输出 串成完整程序。
 * 采集走真 V4L2（src/capture.c）；输出可选 SDL 窗口或 null，由命令行选项决定；
 * 可以同时录制原始帧到文件（--record）。
 *
 * ./camera_app --help 看全部选项。常用：
 *   ./camera_app                              默认 sdl，显示到关窗口
 *   ./camera_app --frames=300 --sink=null     空跑 300 帧，不弹窗
 *   ./camera_app --record=rec.raw             一边显示一边录
 *   ./camera_app --device=/dev/video2         换一个摄像头 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <limits.h>   /* INT_MAX */
#include <getopt.h>   /* getopt_long */
#include "camera/capture.h"
#include "camera/frame_queue.h"
#include "camera/decoder.h"
#include "camera/sink.h"
#include "camera/recorder.h"
#include "camera/stats.h"

/* null 模式没有窗口可关，只能靠信号停：收到后置标志，主循环下一轮退出。
 * sdl 模式不需要这套——SDL_Init 会自己装信号处理并投递 SDL_QUIT 事件。 */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* 按名字创建 sink。名字不认识、或该 sink 没编进这次构建时，打印原因并返回 NULL */
static frame_sink *make_sink(const char *name, unsigned width, unsigned height)
{
#ifndef HAVE_SDL2
    (void)width;    /* 这个构建里只有 null sink，窗口尺寸用不上 */
    (void)height;
#endif

    if (strcmp(name, "null") == 0)
        return sink_null_create();

    if (strcmp(name, "sdl") == 0) {
#ifdef HAVE_SDL2
        return sink_sdl_create(width, height);
#else
        fprintf(stderr, "这个构建没有 SDL2，用不了 sdl sink（可改用 null）\n");
        return NULL;
#endif
    }

    fprintf(stderr, "未知的 sink：%s（可选：sdl / null）\n", name);
    return NULL;
}

/* 解析一个「不小于 0 的整数」选项。失败打印原因并返回 -1。
 * 用 strtol 而不是 atoi：atoi 遇到乱输会静默返回 0，而 0 在这两个选项里
 * 都是合法值（不限帧数 / 不滚动输出）——打错一个字就变成了另一回事。 */
static int parse_uint_arg(const char *arg, const char *what, int *out)
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

/* 录制文件是裸帧、没有头，转码时要显式告诉 ffmpeg 像素格式。
 * ffmpeg 的格式名和我们的枚举不是一回事（YUYV → yuyv422）。 */
static const char *ffmpeg_pix_fmt(pixel_format fmt)
{
    switch (fmt) {
    case PIX_FMT_YUYV:  return "yuyv422";
    case PIX_FMT_RGB24: return "rgb24";
    default:            return NULL;   /* MJPEG 等变长格式不适用裸流 */
    }
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
        "用法：%s [选项]\n"
        "  -n, --frames=N     处理 N 帧后退出；0 = 不限（默认 0）\n"
        "  -d, --device=PATH  采集设备（默认 /dev/video0）\n"
        "  -s, --sink=NAME    输出方式：sdl（开窗口，空格拍照）或 null（丢弃并计数）\n"
        "                     默认 sdl；本次构建未编入 SDL2 时默认 null\n"
        "  -r, --record=PATH  把原始帧录到文件（可与显示同时进行）\n"
        "  -t, --stats=SEC    每 SEC 秒打印一行实时统计；0 = 只打收尾汇总（默认 0）\n"
        "  -h, --help         显示本帮助\n"
        "\n"
        "例：%s --frames=300 --sink=null\n"
        "    %s --record=rec.raw --stats=1 --frames=150\n", prog, prog, prog);
}

int main(int argc, char **argv)
{
    static const struct option long_opts[] = {
        { "frames", required_argument, NULL, 'n' },
        { "device", required_argument, NULL, 'd' },
        { "sink",   required_argument, NULL, 's' },
        { "record", required_argument, NULL, 'r' },
        { "stats",  required_argument, NULL, 't' },
        { "help",   no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int num_frames = 0;                 /* 0 = 不限帧数 */
    int stats_sec  = 0;                 /* 0 = 不滚动输出，只在最后汇总 */
    const char *device = "/dev/video0";
    const char *record_path = NULL;     /* NULL = 不录制 */
#ifdef HAVE_SDL2
    const char *sink_name = "sdl";
#else
    const char *sink_name = "null";
#endif

    int opt;
    while ((opt = getopt_long(argc, argv, "n:d:s:r:t:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'n': if (parse_uint_arg(optarg, "帧数", &num_frames) != 0) return 1; break;
        case 't': if (parse_uint_arg(optarg, "统计间隔（秒）", &stats_sec) != 0) return 1; break;
        case 'd': device = optarg; break;
        case 's': sink_name = optarg; break;
        case 'r': record_path = optarg; break;
        case 'h': usage(stdout, argv[0]); return 0;
        default:  usage(stderr, argv[0]); return 1;
        }
    }

    /* 选项之外还有剩的，说明用了旧的位置参数写法（如 `camera_app 300`）。
     * 不拦的话它会被静默忽略，帧数就悄悄变成默认的「不限」。 */
    if (optind < argc) {
        fprintf(stderr, "多余的参数：%s\n", argv[optind]);
        usage(stderr, argv[0]);
        return 1;
    }

    /* 所有资源先置空，失败时就地 goto out 统一回收 */
    capture *cap = NULL;
    frame_queue *raw_q = NULL;
    decoder *dec = NULL;
    frame_sink *sink = NULL;
    recorder *rec = NULL;
    stats *st = NULL;
    void *raw = NULL, *rgb = NULL;
    int rc = 1;                                                 /* 默认为失败 */

    /* 1. 采集：640x480 YUYV（驱动可能协商成别的，以 cap_* 查询为准） */
    capture_config cfg = { device, PIX_FMT_YUYV, 640, 480, 4 };
    cap = cap_create(&cfg);
    if (!cap) {
        fprintf(stderr, "打开采集设备失败：%s（具体原因见上一行）\n"
                        "常见原因：设备不存在、不是采集节点、"
                        "被别的进程占用、当前用户不在 video 组\n", device);
        goto out;
    }

    /* 2. 原始帧队列（按协商出的一帧实际大小） */
    size_t raw_size = cap_frame_size(cap);
    raw_q = fq_create(8, raw_size, FQ_DROP_OLDEST);
    if (!raw_q) { fprintf(stderr, "创建队列失败\n"); goto out; }

    /* 3. 解码器：YUYV → RGB24 */
    dec = decoder_create(cap_format(cap), cap_width(cap), cap_height(cap));
    if (!dec) { fprintf(stderr, "创建解码器失败\n"); goto out; }
    size_t rgb_size = decoder_output_size(dec);

    /* 4. 输出：按参数选。窗口尺寸用协商出的宽高，否则画面会被拉伸 */
    sink = make_sink(sink_name, cap_width(cap), cap_height(cap));
    if (!sink) goto out;

    if (strcmp(sink_name, "null") == 0) {
        signal(SIGINT,  on_signal);
        signal(SIGTERM, on_signal);
    }

    st = stats_create();
    if (!st) { fprintf(stderr, "创建统计失败\n"); goto out; }

    printf("采集格式 %s %ux%u：原始 %zu 字节/帧，RGB %zu 字节/帧\n",
           pixel_format_name(cap_format(cap)), cap_width(cap), cap_height(cap),
           raw_size, rgb_size);
    printf("输出：%s sink%s\n", sink_name,
           strcmp(sink_name, "sdl") == 0 ? "（空格拍照，ESC 或关窗口退出）"
                                         : "（丢弃并计数，Ctrl+C 退出）");

    /* 5. 录制（可选）：录的是采集到的原始帧，解码之前那一份 */
    if (record_path) {
        rec = rec_create(record_path);
        if (!rec) { fprintf(stderr, "打开录制文件失败：%s\n", record_path); goto out; }
        printf("录制：%s（原始帧直接落盘）\n", record_path);
    }

    /* 5. 起采集线程（生产端） */
    if (cap_start(cap, raw_q) != 0) { fprintf(stderr, "启动采集失败\n"); goto out; }

    /* 6. 帧缓冲：一次分配，循环复用（热路径零 malloc） */
    raw = malloc(raw_size);
    rgb = malloc(rgb_size);
    if (!raw || !rgb) { fprintf(stderr, "分配帧缓冲失败\n"); goto out; }

    /* 8. 主循环（消费端）：pop 原始帧 →（录制）→ 解码 → 输出
     * 同一帧先落盘再解码：录制要的是原始数据，跟显示互不干扰。 */
    int got = 0;
    for (int i = 0; (num_frames == 0 || i < num_frames) && !g_stop; i++) {
        size_t sz = raw_size;
        if (fq_pop(raw_q, raw, &sz, 1000) != 0) break;   /* 超时/队列空 → 退出 */

        /* 录制失败（磁盘满等）不该打断显示：停掉录制，画面继续 */
        if (rec && rec_write(rec, raw, sz) != 0) {
            fprintf(stderr, "录制中断，继续显示但不再录制\n");
            rec_destroy(rec);
            rec = NULL;
        }

        if (decoder_decode(dec, raw, sz, rgb, rgb_size) != 0) continue;

        int r = sink_render(sink, rgb, rgb_size);
        if (r == 1) { printf("窗口已关闭，退出\n"); break; }   /* 用户主动退出 */
        if (r < 0)  { fprintf(stderr, "渲染第 %d 帧失败\n", got + 1); break; }
        got++;

        /* 这一帧确实走完整条链路了，才记进统计 */
        uint64_t now = stats_now_ns();
        stats_tick(st, now, fq_dropped(raw_q));
        stats_report(st, now, stats_sec * 1000);
    }
    if (g_stop) printf("收到中断信号，退出\n");
    printf("完成：%d 帧\n", got);
    stats_summary(st, stats_now_ns());

    /* 录制收尾：报大小，并给出转成能直接播的 mp4 的命令。
     * 裸帧文件没有头，ffmpeg 只能靠命令行参数知道格式和宽高。 */
    if (rec) {
        const char *pf = ffmpeg_pix_fmt(cap_format(cap));
        printf("录制：%lu 帧，%.1f MB\n",
               rec_frame_count(rec), rec_bytes_written(rec) / (1024.0 * 1024.0));
        if (pf)
            printf("  转成可播放的 mp4：\n"
                   "  ffmpeg -f rawvideo -pix_fmt %s -s %ux%u -i %s out.mp4\n",
                   pf, cap_width(cap), cap_height(cap), record_path);
        else
            printf("  （%s 是变长格式，套用上面的命令需另行指定参数）\n",
                   pixel_format_name(cap_format(cap)));
    }
    rc = 0;

out:
    /* 9. 逆序清理。每项都判空、都幂等，从任何一步跳进来都安全 */
    free(raw);
    free(rgb);
    if (cap)   cap_stop(cap);        /* 幂等：未在运行直接返回 */
    if (st)    stats_destroy(st);
    if (rec)   rec_destroy(rec);
    if (sink)  sink_destroy(sink);
    if (dec)   decoder_destroy(dec);
    if (raw_q) fq_destroy(raw_q);
    if (cap)   cap_destroy(cap);
    return rc;
}
