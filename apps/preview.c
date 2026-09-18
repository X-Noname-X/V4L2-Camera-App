/* SDL2 预览窗口，见 preview.h 里关于「为什么它不在库里」的说明 */
#include "preview.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

struct preview {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    unsigned width;
    unsigned height;
};

static void preview_free(preview *pv)
{
    if (pv->tex) { SDL_DestroyTexture(pv->tex);  pv->tex = NULL; }
    if (pv->ren) { SDL_DestroyRenderer(pv->ren); pv->ren = NULL; }
    if (pv->win) { SDL_DestroyWindow(pv->win);   pv->win = NULL; }
    SDL_Quit();
}

preview *preview_open(unsigned width, unsigned height)
{
    preview *pv;

    if (width == 0 || height == 0) return NULL;

    pv = calloc(1, sizeof(*pv));
    if (!pv) return NULL;
    pv->width = width;
    pv->height = height;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init 失败: %s\n", SDL_GetError());
        free(pv);
        return NULL;
    }

    pv->win = SDL_CreateWindow("V4L2 camera",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               (int)width, (int)height, SDL_WINDOW_SHOWN);
    if (!pv->win) {
        fprintf(stderr, "SDL_CreateWindow 失败: %s\n", SDL_GetError());
        goto fail;
    }

    /* PRESENTVSYNC 跟随显示器刷新，否则主循环会空转把 CPU 跑满
     * 个别后端不支持 vsync，退一步用不带它的版本 */
    pv->ren = SDL_CreateRenderer(pv->win, -1,
                                 SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!pv->ren)
        pv->ren = SDL_CreateRenderer(pv->win, -1, SDL_RENDERER_ACCELERATED);
    if (!pv->ren) {
        fprintf(stderr, "SDL_CreateRenderer 失败: %s\n", SDL_GetError());
        goto fail;
    }

    /* STREAMING 声明这块纹理会被整帧反复重写，让 SDL 选合适的存放方式
     * SDL_PIXELFORMAT_RGB24 的内存字节序正是 R,G,B，与 decoder 的输出一致 */
    pv->tex = SDL_CreateTexture(pv->ren, SDL_PIXELFORMAT_RGB24,
                                SDL_TEXTUREACCESS_STREAMING,
                                (int)width, (int)height);
    if (!pv->tex) {
        fprintf(stderr, "SDL_CreateTexture 失败: %s\n", SDL_GetError());
        goto fail;
    }
    return pv;

fail:
    preview_free(pv);
    free(pv);
    return NULL;
}

preview_action preview_show(preview *pv, const void *rgb)
{
    SDL_Event ev;
    preview_action action = PREVIEW_CONTINUE;

    if (!pv || !rgb) return PREVIEW_QUIT;

    /* 必须抽空事件队列，不抽窗口会变成「无响应」，点关闭按钮也没反应 */
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            action = PREVIEW_QUIT;
        } else if (ev.type == SDL_KEYDOWN) {
            /* repeat 非 0 表示按住不放产生的重复事件，
             * 不滤掉的话按住空格会一口气生成一堆照片 */
            if (ev.key.repeat) continue;
            if (ev.key.keysym.sym == SDLK_ESCAPE) action = PREVIEW_QUIT;
            if (ev.key.keysym.sym == SDLK_SPACE)  action = PREVIEW_SNAPSHOT;
        }
    }
    if (action == PREVIEW_QUIT) return action;

    /* 整帧上传，pitch = 每行字节数；RGB24 紧密排列行间无填充，就是 宽*3 */
    if (SDL_UpdateTexture(pv->tex, NULL, rgb, (int)pv->width * 3) != 0) {
        fprintf(stderr, "SDL_UpdateTexture 失败: %s\n", SDL_GetError());
        return PREVIEW_QUIT;
    }
    SDL_RenderClear(pv->ren);
    SDL_RenderCopy(pv->ren, pv->tex, NULL, NULL);
    SDL_RenderPresent(pv->ren);   // 到这一步画面才真的出现在屏幕上

    return action;
}

void preview_close(preview *pv)
{
    if (!pv) return;
    preview_free(pv);
    free(pv);
}
