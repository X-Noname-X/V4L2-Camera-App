/* sink_sdl：把解码后的 RGB24 帧显示到 SDL2 窗口。
 * 和 sink_null 一样填一张 ops 表，区别是 render 真的把像素推上屏幕。 */
#include "camera/sink.h"

#include <SDL.h>      /* 由 CMake 的 SDL2_INCLUDE_DIRS（/usr/include/SDL2）提供 */
#include <stdio.h>
#include <stdlib.h>   /* malloc / calloc / free */

/* sdl sink 的私有数据：窗口 + 渲染器 + 纹理，外加帧计数和退出标志。
 * 这就是 frame_sink.ctx 指向的那个东西。 */
struct sdl_ctx {
    SDL_Window   *win;      // 窗口
    SDL_Renderer *ren;      // 渲染器：真正往窗口上画的东西
    SDL_Texture  *tex;      // 纹理：显存里的一块画布，每帧把 RGB 数据贴上去
    unsigned      width;
    unsigned      height;
    unsigned long frames;   // 已显示帧数
    int           quit;     // 用户关窗口或按 ESC 后置 1
};

static void sdl_close(struct sdl_ctx *c);

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

    /* 必须抽空事件队列：不抽的话窗口会变成「无响应」，点关闭按钮也没反应 */
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            c->quit = 1;
        else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
            c->quit = 1;
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
