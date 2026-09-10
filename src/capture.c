#include "camera/capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>            /* open */
#include <unistd.h>           /* close */
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>         /* mmap / munmap */
#include <sys/ioctl.h>        /* ioctl */
#include <linux/videodev2.h>  /* V4L2 常量 */

struct capture{
    int fd;
    pixel_format fmt;
    unsigned width;
    unsigned height;
    unsigned buffer_count;  // 实际分配的缓冲数（以 REQBUFS 返回为准）
    void **buffers;        // 每块 mmap 出的缓冲区的起始地址
    size_t *buffer_sizes;  // 每块缓冲区的长度
    frame_queue *fq;   // cap_start 指定的目标队列
    pthread_t thread;  // 线程句柄
    int running;  // 线程退出标志
};

/* ---------- 像素格式 <-> V4L2 fourcc （四字符码）---------- */
static unsigned fmt_to_fourcc(pixel_format fmt)
{
    switch (fmt) {
        case PIX_FMT_YUYV:  return V4L2_PIX_FMT_YUYV;
        case PIX_FMT_MJPEG: return V4L2_PIX_FMT_MJPEG;
        case PIX_FMT_RGB24: return V4L2_PIX_FMT_RGB24;
        default:            return 0;
    }
}

static pixel_format fourcc_to_fmt(unsigned fourcc)
{
    switch (fourcc) {
        case V4L2_PIX_FMT_YUYV:  return PIX_FMT_YUYV;
        case V4L2_PIX_FMT_MJPEG: return PIX_FMT_MJPEG;
        case V4L2_PIX_FMT_RGB24: return PIX_FMT_RGB24;
        default:                 return PIX_FMT_YUYV;
    }
}

/* ioctl 包装：失败打印错误并返回 -1，调用方据此回滚 */
static int xioctl(int fd, unsigned long request, void *arg, const char *what)
{
    if (ioctl(fd, request, arg) == -1) {
        perror(what);
        return -1;
    }
    return 0;
}

capture *cap_create(const capture_config *cfg)
{
    capture *cap = NULL;
    int mapped = 0;   /* 已成功 mmap 的块数，用于失败回滚 */

    if (!cfg || !cfg->device || cfg->width == 0 || cfg->height == 0 || cfg->buffer_count == 0)
        return NULL;

    cap = calloc(1, sizeof(*cap));
    if (!cap) return NULL;
    cap->fd = -1;

    /* 1. 打开设备 */
    cap->fd = open(cfg->device, O_RDWR);   /* 阻塞模式：DQBUF 会阻塞等帧，STREAMOFF 时返回错误退出 */
    if (cap->fd < 0) { perror("open"); goto fail; }

    /* 2. 确认是视频采集设备，且支持流式 I/O */
    struct v4l2_capability capability;
    memset(&capability, 0, sizeof(capability));
    if (xioctl(cap->fd, VIDIOC_QUERYCAP, &capability, "VIDIOC_QUERYCAP") < 0) goto fail;

    /* 新驱动在 device_caps 里给出「这个 video 节点自己」的能力，比 capabilities
     * （整个硬件设备的总能力）准确：一个摄像头可能同时导出采集节点和 metadata
     * 节点，看总能力会误判。老驱动没有 DEVICE_CAPS 时才退回用 capabilities。 */
    unsigned caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                  ? capability.device_caps
                  : capability.capabilities;

    /* 这两项是硬性依赖：缺了后面要么 S_FMT 报 EINVAL，要么 QBUF/DQBUF 用不了。
     * 在这里就拦下，免得带着不合适的设备一路走到深处才失败、错误信息还看不懂。 */
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s 不是视频采集设备\n", cfg->device);
        goto fail;
    }
    if (!(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s 不支持流式 I/O（QBUF/DQBUF 用不了）\n", cfg->device);
        goto fail;
    }
    printf("[%s] 支持视频捕获 + 流式输入输出\n", cfg->device);

    /* 3. 设置格式。S_FMT 成功后 fmt 会被驱动改成实际支持的格式，
     *    直接读回即可，不必再单独 VIDIOC_G_FMT。 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = cfg->width;
    fmt.fmt.pix.height      = cfg->height;
    fmt.fmt.pix.pixelformat = fmt_to_fourcc(cfg->fmt);   /* 这里要 fourcc，不是枚举值 */
    if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT") < 0) goto fail;
    cap->width  = fmt.fmt.pix.width;
    cap->height = fmt.fmt.pix.height;
    cap->fmt    = fourcc_to_fmt(fmt.fmt.pix.pixelformat);   /* 读回也要转成枚举 */
    printf("驱动实际调整到: %dx%d\n", cap->width, cap->height);

    /* 4. 申请 mmap 缓冲 */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = cfg->buffer_count;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cap->fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS") < 0) goto fail;
    if (req.count == 0) { fprintf(stderr, "驱动返回 0 个缓冲\n"); goto fail; }

    cap->buffer_count = req.count;   /* 实际数量以驱动返回为准 */
    cap->buffers      = calloc(req.count, sizeof(*cap->buffers));
    cap->buffer_sizes = calloc(req.count, sizeof(*cap->buffer_sizes));
    if (!cap->buffers || !cap->buffer_sizes) goto fail;

    /* 5. 逐块 QUERYBUF + mmap + QBUF */
    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        /* 查询第 i 个缓冲区的长度和偏移 */
        if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF") < 0) goto fail;
        /* 把内核缓冲区映射到用户空间，之后就能直接读写这块内存 */
        cap->buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, cap->fd, buf.m.offset);
        if (cap->buffers[i] == MAP_FAILED) { perror("mmap"); goto fail; }
        cap->buffer_sizes[i] = buf.length;
        mapped++;   /* 多成功映射了一块 */
        /* 把缓冲区放入采集队列，驱动填满后即可取出 */
        if (xioctl(cap->fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF") < 0) goto fail;
    }
    printf("成功映射 %d 个缓冲区\n", mapped);

    return cap;

fail:
    if (cap) {
        for (int i = 0; i < mapped; i++)   /* 只回滚成功映射过的 */
            munmap(cap->buffers[i], cap->buffer_sizes[i]);
        free(cap->buffers);
        free(cap->buffer_sizes);
        if (cap->fd >= 0) close(cap->fd);
        free(cap);
    }
    return NULL;
}

void cap_destroy(capture *cap)
{
    if (!cap) return;
    if (cap->running)
        cap_stop(cap);   /* 内部会 STREAMOFF + join，并置 running=0 */

    for (unsigned i = 0; i < cap->buffer_count; i++)
        munmap(cap->buffers[i], cap->buffer_sizes[i]);
    free(cap->buffers);
    free(cap->buffer_sizes);
    if (cap->fd >= 0) close(cap->fd);
    free(cap);
}

/* 采集线程：阻塞 DQBUF → push → QBUF，直到 STREAMOFF 让 DQBUF 出错退出 */
static void *capture_thread(void *arg)
{
    capture *cap = arg;

    for (;;) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        /* 阻塞等一帧；STREAMOFF 会让它返回错误，借此退出线程 */
        if (ioctl(cap->fd, VIDIOC_DQBUF, &buf) < 0) break;

        fq_push(cap->fq, cap->buffers[buf.index], buf.bytesused);

        /* 归还缓冲；退出阶段失败也无所谓，忽略 */
        ioctl(cap->fd, VIDIOC_QBUF, &buf);
    }
    return NULL;
}

int cap_start(capture *cap, frame_queue *fq)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (!cap || !fq) return -1;
    if (cap->running) return -1;   /* 已在运行 */
    if (fq_frame_size(fq) != cap_frame_size(cap)) return -1;   /* 队列帧大小不匹配 */

    if (xioctl(cap->fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON") < 0) return -1;

    cap->fq = fq;   /* 先赋值再起线程，线程里读到的才是对的 */
    if (pthread_create(&cap->thread, NULL, capture_thread, cap) != 0) {
        xioctl(cap->fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");   /* 回滚 */
        return -1;
    }
    cap->running = 1;
    return 0;
}

void cap_stop(capture *cap)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (!cap || !cap->running) return;   /* 未在运行，幂等返回 */
    /* 先停流：让阻塞中的 DQBUF 立即返回错误，线程随之退出 */
    xioctl(cap->fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
    pthread_join(cap->thread, NULL);
    cap->running = 0;
}

/* ---------- getters ---------- */
pixel_format cap_format(const capture *cap) { return cap ? cap->fmt : 0; }
unsigned     cap_width(const capture *cap)  { return cap ? cap->width : 0; }
unsigned     cap_height(const capture *cap) { return cap ? cap->height : 0; }

size_t cap_frame_size(const capture *cap)
{
    if (!cap) return 0;
    switch (cap->fmt) {
    case PIX_FMT_YUYV:  return (size_t)cap->width * cap->height * 2;
    case PIX_FMT_RGB24: return (size_t)cap->width * cap->height * 3;
    case PIX_FMT_MJPEG: return 0;   /* 变长 */
    default:            return 0;
    }
}
