/* stats 单元测试：时间是自己喂进去的，所以每个数字都能精确断言——
 * 不用 sleep 等真时间，也不会因为机器忙而忽大忽小。
 * 断言 18 个。 */
#include <stdio.h>
#include "camera/stats.h"

#define MS(x) ((uint64_t)(x) * 1000000ULL)

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond) do {                                          \
    tests_run++;                                                  \
    if (!(cond)) {                                                \
        tests_failed++;                                           \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                             \
} while (0)

/* 浮点比较：这些用例的期望值都是能精确表示的，容差给极小即可 */
static int near(double a, double b)
{
    double d = a - b;
    return d < 1e-9 && d > -1e-9;
}

int main(void)
{
    /* ---------- 1. 传 NULL 不该崩 ---------- */
    stats_destroy(NULL);
    stats_tick(NULL, MS(100), 0);
    stats_get(NULL, MS(100), NULL);
    CHECK(stats_report(NULL, MS(100), 100) == 0);
    stats_summary(NULL, MS(100));

    /* ---------- 2. 时钟单调 ---------- */
    uint64_t t1 = stats_now_ns();
    uint64_t t2 = stats_now_ns();
    CHECK(t2 >= t1);

    stats *st = stats_create();
    CHECK(st != NULL);

    stats_snapshot snap;

    /* ---------- 3. 一帧没记时，一切为 0 ---------- */
    stats_get(st, MS(1000), &snap);
    CHECK(snap.frames == 0 && snap.dropped == 0);
    CHECK(near(snap.fps, 0.0));
    CHECK(near(snap.drop_rate, 0.0));
    CHECK(stats_report(st, MS(1000), 500) == 0);   /* 还没收到过帧，不输出 */

    /* ---------- 4. 6 帧跨 1000ms → 平均 6.0 fps ----------
     * 时间点 0/200/400/600/800/1000ms，起点是第一帧的 0，终点取 1000ms */
    for (int i = 0; i <= 5; i++)
        stats_tick(st, MS(200 * i), 0);

    stats_get(st, MS(1000), &snap);
    CHECK(snap.frames == 6);
    CHECK(near(snap.fps, 6.0));
    CHECK(near(snap.drop_rate, 0.0));

    /* ---------- 5. 丢帧率 ----------
     * 再来一帧（frames 变 7）并把累计丢帧报成 2 →
     * 分母是 7 + 2 = 9，比率 2/9 */
    stats_tick(st, MS(1000), 2);
    stats_get(st, MS(1000), &snap);
    CHECK(snap.frames == 7);
    CHECK(snap.dropped == 2);
    CHECK(near(snap.drop_rate, 2.0 / 9.0));

    /* ---------- 6. 滚动输出的两个分支 ----------
     * window_ns 在第一次 tick 时被设成 0，所以从 0 算起 */
    CHECK(stats_report(st, MS(1000), 500) == 1);   /* 距 0 已 1000ms ≥ 500 → 输出 */
    CHECK(stats_report(st, MS(1000), 500) == 0);   /* 刚输出完，窗口清零 → 不输出 */
    CHECK(stats_report(st, MS(1200), 500) == 0);   /* 才过 200ms < 500 */
    CHECK(stats_report(st, MS(1600), 500) == 1);   /* 过了 600ms ≥ 500 → 输出 */

    /* interval <= 0 表示不滚动 */
    CHECK(stats_report(st, MS(9999),  0) == 0);
    CHECK(stats_report(st, MS(9999), -1) == 0);

    /* ---------- 7. 收尾汇总不崩 ---------- */
    stats_summary(st, MS(2000));
    CHECK(1);

    stats_get(st, MS(2000), NULL);   /* out 传 NULL 也要安全 */
    stats_destroy(st);

    printf("== %d 个断言, %d 个失败 ==\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
