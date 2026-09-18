#ifndef PREVIEW_H_
#define PREVIEW_H_

/* 本地预览窗口：把解码后的 RGB24 帧显示到 SDL2 窗口，按空格可拍一张
 *
 * 这只是本仓库「看得见画面」用的调试工具，不是库的一部分
 * 真正跑在 LCD 上的显示代码应该在 LCD 仓库里——那边链接 libcamera 拿到
 * RGB24，自己决定怎么画到屏幕上，等那边能跑了，这个文件就可以删掉 */

typedef struct preview preview;

/* 用户在这一帧上的意图 */
typedef enum {
    PREVIEW_CONTINUE = 0,   // 继续
    PREVIEW_QUIT     = 1,   // 关窗口或按了 ESC
    PREVIEW_SNAPSHOT = 2,   // 按了空格：调用方应当存下当前这一帧
} preview_action;

/* 开窗口；宽高要用采集实际协商出的值，传错了画面会被拉伸 */
preview *preview_open(unsigned width, unsigned height);

/* 显示一帧 RGB24（width*height*3 字节，行间无填充），并处理窗口事件 */
preview_action preview_show(preview *pv, const void *rgb);

void preview_close(preview *pv);

#endif
