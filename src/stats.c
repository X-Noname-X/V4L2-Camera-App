/* stats：FPS 和丢帧率的记账本。
 * 不透明类型——struct stats 只在本文件里定义，外部只能拿到指针。 */
#define _POSIX_C_SOURCE 200809L   /* 让 clock_gettime / CLOCK_MONOTONIC 可见 */

#include "camera/stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NS_PER_SEC 1000000000ULL

struct stats {
    uint64_t      start_ns;         // 第一帧的时间，全程平均的起点
    uint64_t      window_ns;        // 上次滚动输出的时间
    unsigned long frames;           // 累计处理的帧数
    unsigned long frames_window;    // 上次输出时的帧数
    unsigned long dropped;          // 最近一次 tick 报上来的累计丢帧数
    unsigned long dropped_window;   // 上次输出时的丢帧数
    int           started;          // 收到过第一帧没有
};

uint64_t stats_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

stats *stats_create(void)
{
    return calloc(1, sizeof(stats));   /* 全 0 就是合法的初始状态 */
}

void stats_destroy(stats *s)
{
    free(s);   /* free(NULL) 是安全的，所以不用判空 */
}

void stats_tick(stats *s, uint64_t now_ns, unsigned long dropped_total)
{
    if (!s) return;

    /* 第一帧只记起点，不累计间隔——否则第一帧会拖着一个巨大的 delta */
    if (!s->started) {
        s->started   = 1;
        s->start_ns  = now_ns;
        s->window_ns = now_ns;
    }

    s->frames++;
    s->dropped = dropped_total;
}

void stats_get(const stats *s, uint64_t now_ns, stats_snapshot *out)
{
    if (!out) return;

    out->frames = out->dropped = 0;
    out->fps = out->drop_rate = 0.0;
    if (!s) return;

    out->frames  = s->frames;
    out->dropped = s->dropped;

    if (!s->started || now_ns <= s->start_ns) return;   /* 还没开始或时钟没走，保持 0 */

    double secs = (double)(now_ns - s->start_ns) / (double)NS_PER_SEC;
    out->fps = (double)s->frames / secs;

    /* 分母用「成功的 + 丢掉的」而不是 pushed：对调用方来说只需知道
     * 有多少帧真到了手上、有多少帧没了，这个比例最直观 */
    unsigned long total = out->frames + out->dropped;
    if (total) out->drop_rate = (double)out->dropped / (double)total;
}

int stats_report(stats *s, uint64_t now_ns, int interval_ms)
{
    if (!s || !s->started || interval_ms <= 0) return 0;
    if (now_ns - s->window_ns < (uint64_t)interval_ms * 1000000ULL) return 0;

    double secs = (double)(now_ns - s->window_ns) / (double)NS_PER_SEC;
    unsigned long df = s->frames - s->frames_window;
    unsigned long dd = s->dropped - s->dropped_window;

    stats_snapshot snap;
    stats_get(s, now_ns, &snap);

    printf("[stats] %6.1f fps | 本窗口 %lu 帧、丢 %lu | "
           "累计 %lu 帧、丢 %lu（%.1f%%）\n",
           secs > 0.0 ? (double)df / secs : 0.0,
           df, dd,
           snap.frames, snap.dropped, snap.drop_rate * 100.0);

    s->window_ns       = now_ns;
    s->frames_window   = s->frames;
    s->dropped_window  = s->dropped;
    return 1;
}

void stats_summary(const stats *s, uint64_t now_ns)
{
    if (!s) return;

    stats_snapshot snap;
    stats_get(s, now_ns, &snap);

    double secs = s->started ? (double)(now_ns - s->start_ns) / (double)NS_PER_SEC : 0.0;

    printf("统计：平均 %.1f fps（%lu 帧 / %.2f 秒），丢帧 %lu（%.1f%%）\n",
           snap.fps, snap.frames, secs, snap.dropped, snap.drop_rate * 100.0);
}
