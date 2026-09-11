#define _POSIX_C_SOURCE 200809L   /* open/O_EXCL/fdopen/close 都是 POSIX，不加这句
                                     * 编译器看不到声明，会按隐式 int 调，指针被截断 */
/* sink_sdl：把解码后的 RGB24 帧显示到 SDL2 窗口，按空格键可存下当前画面。
 * 和 sink_null 一样填一张 ops 表，区别是 render 真的把像素推上屏幕。
 *
 * 拍照放在这里、而不是 recorder 里，因为只有这里同时具备两样东西：
 * 键盘事件，以及已经解码好、正在显示的那一帧。拍照语义就是「把此刻
 * 屏幕上的画面存下来」，所以存的就是 render 收到的那个 rgb 缓冲。 */
#include "camera/sink.h"

#include <SDL.h>      /* 由 CMake 的 SDL2_INCLUDE_DIRS（/usr/include/SDL2）提供 */
#include <stdio.h>
#include <stdlib.h>   /* malloc / calloc / free */
#include <stdint.h>   /* uint16_t 等 */
#include <fcntl.h>    /* open / O_EXCL */
#include <unistd.h>   /* close */
#include <errno.h>

/* sdl sink 的私有数据：窗口 + 渲染器 + 纹理，外加帧计数和退出标志。
 * 这就是 frame_sink.ctx 指向的那个东西。 */
struct sdl_ctx {
    SDL_Window   *win;      // 窗口
    SDL_Renderer *ren;      // 渲染器：真正往窗口上画的东西
    SDL_Texture  *tex;      // 纹理：显存里的一块画布，每帧把 RGB 数据贴上去
    unsigned      width;
    unsigned      height;
    unsigned long frames;   // 已显示帧数
    unsigned long photo_seq;// 最后用掉的照片文件编号（下次从它 +1 往后找）
    int           quit;     // 用户关窗口或按 ESC 后置 1
};

static void sdl_close(struct sdl_ctx *c);

/* BMP 文件头，共 54 字节。pack(1) 关掉对齐填充——BMP 要求字段严丝合缝，
 * 默认对齐会在 offset 4 后面插空洞，写出来就是坏文件。 */
#pragma pack(push, 1)
struct bmp_header {
    /* BITMAPFILEHEADER，14 字节 */
    uint16_t type;          // 'BM'（小端存成 0x4D42）
    uint32_t size;          // 整个文件大小
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset;        // 像素数据的起始偏移，本格式恒为 54
    /* BITMAPINFOHEADER，40 字节 */
    uint32_t hdr_size;      // 本结构大小，40
    int32_t  width;
    int32_t  height;        // 正数 = 自下而上存（BMP 的传统顺序）
    uint16_t planes;        // 恒为 1
    uint16_t bpp;           // 每像素位数，24
    uint32_t compression;   // 0 = BI_RGB，不压缩
    uint32_t img_size;      // 像素数据字节数
    int32_t  x_ppm;         // 横向 DPI，2835 即 72 DPI
    int32_t  y_ppm;
    uint32_t colors;        // 调色板项数，24 位用不着
    uint32_t important;
};
#pragma pack(pop)

/* 把一帧 RGB24 编码成 BMP 写进已经打开的流里。文件由调用方开关，
 * 本函数只管编码。640x480 的 RGB24 是一行 1920 字节，正好 4 的倍数所以
 * 没有行填充；但宽度不是 4 的倍数时 BMP 要求补齐，这里按通用情况处理。 */
static int write_bmp(FILE *fp, const unsigned char *rgb, unsigned w, unsigned h)
{
    size_t row      = (size_t)w * 3;
    size_t row_pad  = (4 - (row % 4)) % 4;      // 每行补到 4 字节对齐
    size_t img_size = (row + row_pad) * h;

    struct bmp_header hdr = {
        .type = 0x4D42, .size = (uint32_t)(sizeof(hdr) + img_size),
        .reserved1 = 0, .reserved2 = 0, .offset = (uint32_t)sizeof(hdr),
        .hdr_size = 40, .width = (int32_t)w, .height = (int32_t)h,
        .planes = 1, .bpp = 24, .compression = 0,
        .img_size = (uint32_t)img_size,
        .x_ppm = 2835, .y_ppm = 2835, .colors = 0, .important = 0,
    };
    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) return -1;

    /* BMP 的 24 位像素在内存里存的是 B,G,R，而我们的缓冲是 R,G,B——
     * 原样写进去红蓝会调换（BMP 这个字节序纯属历史遗留，没有开关可关）。
     * 逐行翻到一个临时缓冲再写，不碰调用方的数据。 */
    unsigned char *bgr = malloc(row);
    if (!bgr) return -1;

    static const unsigned char pad_bytes[3] = { 0, 0, 0 };
    int rc = 0;

    /* 像素自下而上写：BMP 的第一个像素行是图像的最后一行 */
    for (int y = (int)h - 1; y >= 0 && rc == 0; y--) {
        const unsigned char *src = rgb + (size_t)y * row;
        for (size_t i = 0; i < row; i += 3) {
            bgr[i]     = src[i + 2];   // B
            bgr[i + 1] = src[i + 1];   // G
            bgr[i + 2] = src[i];       // R
        }
        if (fwrite(bgr, 1, row, fp) != row) { rc = -1; break; }
        if (row_pad && fwrite(pad_bytes, 1, row_pad, fp) != row_pad) { rc = -1; break; }
    }

    free(bgr);
    return rc;
}

/* 存下当前这一帧：photo_0001.bmp、photo_0002.bmp … 落在进程的工作目录。
 *
 * 编号在每个进程里都从 1 开始，所以必须先找一个还没被占用的名字——
 * 否则第二次运行按空格就会把上次拍的照片覆盖掉。用 O_EXCL 打开，
 * 它的语义正是「文件已存在就失败」，找不到就用下一个号继续试。
 * O_EXCL 是原子的，比先 access() 判断再 fopen 更靠得住。
 *
 * 失败只打印原因，不打断显示——拍不成照片不该把画面也弄停。 */
static void save_photo(struct sdl_ctx *c, const void *rgb, size_t size)
{
    char path[64];
    int  fd;

    if (size < (size_t)c->width * c->height * 3) return;

    for (;;) {
        snprintf(path, sizeof(path), "photo_%04lu.bmp", c->photo_seq + 1);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) break;                          /* 抢到了这个编号 */
        if (errno != EEXIST) { perror("open"); return; }   /* 别的错：没权限、磁盘满… */
        c->photo_seq++;                              /* 名字被占了，看下一个 */
    }

    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        perror("fdopen");
        close(fd);
        remove(path);
        return;
    }

    if (write_bmp(fp, rgb, c->width, c->height) != 0 || fclose(fp) != 0) {
        fprintf(stderr, "保存照片失败：%s\n", path);
        remove(path);          // 别留半截文件
        return;                // photo_seq 不动，下次还是用这个号
    }

    c->photo_seq++;            // 这次用掉了这个编号
    printf("已保存 %s（%ux%u）\n", path, c->width, c->height);
}

/* 建好 SDL 三件套。任何一步失败都要把前面建好的收回去，
 * 否则会漏掉窗口/渲染器（SDL 的对象不像 malloc 一个 free 就完事）。 */
static int sdl_open(struct sdl_ctx *c, unsigned width, unsigned height)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init 失败: %s\n", SDL_GetError());
        return -1;
    }

    c->win = SDL_CreateWindow("V4L2 camera",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              (int)width, (int)height, SDL_WINDOW_SHOWN);
    if (!c->win) {
        fprintf(stderr, "SDL_CreateWindow 失败: %s\n", SDL_GetError());
        goto fail;
    }

    /* PRESENTVSYNC：跟随显示器刷新节奏，否则这个 while 循环会空转把 CPU 跑满。
     * 个别后端不支持 vsync，退一步用不带它的版本。 */
    c->ren = SDL_CreateRenderer(c->win, -1,
                                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!c->ren)
        c->ren = SDL_CreateRenderer(c->win, -1, SDL_RENDERER_ACCELERATED);
    if (!c->ren) {
        fprintf(stderr, "SDL_CreateRenderer 失败: %s\n", SDL_GetError());
        goto fail;
    }

    /* STREAMING：声明这块纹理会被整帧反复重写，让 SDL 选合适的存放方式。
     * SDL_PIXELFORMAT_RGB24 的内存字节序正是 R,G,B —— 与 decoder 的输出一致。 */
    c->tex = SDL_CreateTexture(c->ren, SDL_PIXELFORMAT_RGB24,
                               SDL_TEXTUREACCESS_STREAMING,
                               (int)width, (int)height);
    if (!c->tex) {
        fprintf(stderr, "SDL_CreateTexture 失败: %s\n", SDL_GetError());
        goto fail;
    }
    return 0;

fail:
    sdl_close(c);
    return -1;
}

/* 释放 SDL 三件套（逆序）。指针置 NULL，重复调用安全。 */
static void sdl_close(struct sdl_ctx *c)
{
    if (c->tex) { SDL_DestroyTexture(c->tex);   c->tex = NULL; }
    if (c->ren) { SDL_DestroyRenderer(c->ren);  c->ren = NULL; }
    if (c->win) { SDL_DestroyWindow(c->win);    c->win = NULL; }
    SDL_Quit();
}

/* vtable 里的 render：抽事件 → 上传这一帧 → 画到窗口上。
 * 返回 0 继续 / 1 用户要退出 / -1 出错（约定见 sink.h）。 */
static int sdl_render(void *ctx, const void *rgb, size_t size)
{
    struct sdl_ctx *c = ctx;
    SDL_Event ev;

    if (size < (size_t)c->width * c->height * 3) return -1;

    /* 必须抽空事件队列：不抽的话窗口会变成「无响应」，点关闭按钮也没反应。
     * 空格在这里处理：此刻手上的 rgb 正是马上要显示的那一帧，存下来即所见即所得。 */
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            c->quit = 1;
        } else if (ev.type == SDL_KEYDOWN) {
            /* repeat 非 0 表示按住不放产生的重复事件——不滤掉的话
             * 按住空格会哗哗生成一堆照片 */
            if (ev.key.repeat) continue;
            if (ev.key.keysym.sym == SDLK_ESCAPE) c->quit = 1;
            else if (ev.key.keysym.sym == SDLK_SPACE) save_photo(c, rgb, size);
        }
    }
    if (c->quit) return 1;   /* 告诉调用方：该收摊了 */

    /* 整帧上传。pitch = 每行字节数；RGB24 紧密排列、行间无填充，就是 宽 * 3。 */
    if (SDL_UpdateTexture(c->tex, NULL, rgb, (int)c->width * 3) != 0) {
        fprintf(stderr, "SDL_UpdateTexture 失败: %s\n", SDL_GetError());
        return -1;
    }
    SDL_RenderClear(c->ren);
    SDL_RenderCopy(c->ren, c->tex, NULL, NULL);
    SDL_RenderPresent(c->ren);   /* 到这一步画面才真的出现在屏幕上 */

    c->frames++;
    return 0;
}

// vtable 里的 destroy：先拆 SDL，再释放私有数据（由 sink_destroy 调用）
static void sdl_destroy(void *ctx)
{
    struct sdl_ctx *c = ctx;
    sdl_close(c);
    free(c);
}

// 本实现的 vtable：结构和 null 那张一样，只是函数不同
static const struct sink_ops sdl_ops = { sdl_render, sdl_destroy };

/* 工厂：建窗口 + 绑定 vtable，返回公共骨架。
 * 窗口尺寸应传采集实际协商出的宽高，否则画面会被拉伸。 */
frame_sink *sink_sdl_create(unsigned width, unsigned height)
{
    frame_sink *s;
    struct sdl_ctx *c;

    if (width == 0 || height == 0) return NULL;

    s = malloc(sizeof(*s));
    c = calloc(1, sizeof(*c));
    if (!s || !c) {          // 任一分失败都要回滚，避免泄漏
        free(s);
        free(c);
        return NULL;
    }

    c->width  = width;
    c->height = height;
    if (sdl_open(c, width, height) != 0) {
        free(s);
        free(c);   /* 窗口等资源已由 sdl_open 内部的 sdl_close 收掉了 */
        return NULL;
    }

    s->ops = &sdl_ops;         // 绑定 vtable
    s->ctx = c;                // 塞进私有数据
    return s;
}
