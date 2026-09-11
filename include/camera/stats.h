#ifndef STATS_H_
#define STATS_H_

#include <stdint.h>   /* uint64_t */

/* 统计：算 FPS 和丢帧率。
 *
 * 时间由调用方传进来，而不是模块自己去读时钟。好处是同一份逻辑能在测试里
 * 喂假时间、算出确定的数字，不必靠 sleep 等真时间。要用真时间就传
 * stats_now_ns()。
 *
 * 丢帧数也不是自己数的——由调用方从 frame_queue 取（fq_dropped()）报上来。
 * 这样 stats 不依赖 frame_queue，谁有数据谁提供。 */

typedef struct stats stats;

/* 单调时钟，纳秒。CLOCK_MONOTONIC 不受系统时间被改动的影响，只适合算间隔 */
uint64_t stats_now_ns(void);

stats *stats_create(void);
void   stats_destroy(stats *s);

/* 每处理完一帧调一次。
 *   now_ns        : 当前时间（用 stats_now_ns()）
 *   dropped_total : 采集端累计丢帧数（用 fq_dropped()），单调不减 */
void stats_tick(stats *s, uint64_t now_ns, unsigned long dropped_total);

/* 按周期滚动输出一行。距上次输出不足 interval_ms 就什么都不做、返回 0；
 * 到点了就打印并返回 1。interval_ms <= 0 表示不滚动，只在最后汇总。 */
int stats_report(stats *s, uint64_t now_ns, int interval_ms);

/* 汇总出来的数字，可读也可打印 */
typedef struct {
    unsigned long frames;     /* 已处理的帧数 */
    unsigned long dropped;    /* 累计丢帧数 */
    double        fps;        /* 全程平均 = frames / 经过的秒数 */
    double        drop_rate;  /* dropped / (frames + dropped)，范围 [0,1] */
} stats_snapshot;

/* 取当前汇总（不做任何打印） */
void stats_get(const stats *s, uint64_t now_ns, stats_snapshot *out);

/* 打印收尾汇总，内部就是调 stats_get */
void stats_summary(const stats *s, uint64_t now_ns);

#endif
