/* frame_queue 单元测试：无外部测试框架，手写最小断言 */
#define _POSIX_C_SOURCE 200809L   /* pthread 相关 */

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "camera/frame_queue.h"

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond) do {                                          \
    tests_run++;                                                  \
    if (!(cond)) {                                                \
        tests_failed++;                                           \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                             \
} while (0)

/* 1. 创建/销毁 + 单帧往返 */
static void test_create_and_roundtrip(void)
{
    frame_queue *q = fq_create(4, sizeof(int), FQ_DROP_OLDEST);
    CHECK(q != NULL);

    int in = 42, out = 0;
    size_t sz = sizeof(out);
    CHECK(fq_push(q, &in, sizeof(in)) == 0);
    CHECK(fq_pop(q, &out, &sz, -1) == 0);
    CHECK(out == 42);
    CHECK(sz == sizeof(int));

    fq_destroy(q);
}

/* 2. FIFO 顺序：先进先出 */
static void test_fifo_order(void)
{
    frame_queue *q = fq_create(3, sizeof(int), FQ_DROP_OLDEST);
    CHECK(q != NULL);

    for (int i = 1; i <= 3; i++)
        CHECK(fq_push(q, &i, sizeof(i)) == 0);

    for (int i = 1; i <= 3; i++) {
        int v = 0; size_t sz = sizeof(v);
        CHECK(fq_pop(q, &v, &sz, -1) == 0);
        CHECK(v == i);
    }

    fq_destroy(q);
}

/* 3. 满时丢最旧：容量 2，塞 3 帧，应剩 2、3 */
static void test_drop_oldest(void)
{
    frame_queue *q = fq_create(2, sizeof(int), FQ_DROP_OLDEST);
    int a = 1, b = 2, c = 3, v = 0;
    size_t sz;

    CHECK(fq_push(q, &a, sizeof(a)) == 0);
    CHECK(fq_push(q, &b, sizeof(b)) == 0);
    CHECK(fq_push(q, &c, sizeof(c)) == 0);   /* 满，丢 1，剩 2、3 */

    sz = sizeof(v); CHECK(fq_pop(q, &v, &sz, -1) == 0); CHECK(v == 2);
    sz = sizeof(v); CHECK(fq_pop(q, &v, &sz, -1) == 0); CHECK(v == 3);
    sz = sizeof(v); CHECK(fq_pop(q, &v, &sz, 0) == -1);   /* 已空 */

    fq_destroy(q);
}

/* 4. 满时丢最新：容量 2，塞 3 帧，应剩 1、2 */
static void test_drop_newest(void)
{
    frame_queue *q = fq_create(2, sizeof(int), FQ_DROP_NEWEST);
    int a = 1, b = 2, c = 3, v = 0;
    size_t sz;

    CHECK(fq_push(q, &a, sizeof(a)) == 0);
    CHECK(fq_push(q, &b, sizeof(b)) == 0);
    CHECK(fq_push(q, &c, sizeof(c)) == 0);   /* 满，丢 3，剩 1、2 */

    sz = sizeof(v); CHECK(fq_pop(q, &v, &sz, -1) == 0); CHECK(v == 1);
    sz = sizeof(v); CHECK(fq_pop(q, &v, &sz, -1) == 0); CHECK(v == 2);

    fq_destroy(q);
}

/* 5. 空队列的超时 / 非阻塞返回 */
static void test_empty_timeout(void)
{
    frame_queue *q = fq_create(2, sizeof(int), FQ_DROP_OLDEST);
    int v = 0; size_t sz = sizeof(v);

    CHECK(fq_pop(q, &v, &sz, 0) == -1);     /* 非阻塞，立即返回 -1 */
    CHECK(fq_pop(q, &v, &sz, 100) == -1);   /* 100ms 超时，走 timedwait 路径 */

    fq_destroy(q);
}

/* 6. 非法参数 */
static void test_invalid_args(void)
{
    CHECK(fq_create(0, sizeof(int), FQ_DROP_OLDEST) == NULL);   /* 容量 0 */
    CHECK(fq_create(4, 0, FQ_DROP_OLDEST) == NULL);             /* 帧大小 0 */

    frame_queue *q = fq_create(2, sizeof(int), FQ_DROP_OLDEST);
    CHECK(q != NULL);

    int v = 1;
    CHECK(fq_push(q, NULL, sizeof(v)) == -1);        /* data 为空 */
    CHECK(fq_push(q, &v, sizeof(v) + 1) == -1);      /* 大小不符 */

    size_t small = 2;                                /* out 缓冲区太小 */
    CHECK(fq_pop(q, &v, &small, 0) == -1);

    fq_destroy(q);
}

/* 7. 统计计数：pushed / dropped（供 stats 算丢帧率） */
static void test_counters(void)
{
    /* 传 NULL 返回 0 而不是崩 */
    CHECK(fq_pushed(NULL) == 0);
    CHECK(fq_dropped(NULL) == 0);

    frame_queue *q = fq_create(2, sizeof(int), FQ_DROP_OLDEST);
    CHECK(q != NULL);
    CHECK(fq_pushed(q) == 0 && fq_dropped(q) == 0);   /* 新建时归零 */

    int a = 1, b = 2, c = 3, d = 4;

    /* 没满之前不该有丢帧 */
    CHECK(fq_push(q, &a, sizeof(a)) == 0);
    CHECK(fq_push(q, &b, sizeof(b)) == 0);
    CHECK(fq_pushed(q) == 2);
    CHECK(fq_dropped(q) == 0);

    /* 满了之后每多送一帧就丢一帧：送 4 次共 2 次溢出 */
    CHECK(fq_push(q, &c, sizeof(c)) == 0);
    CHECK(fq_push(q, &d, sizeof(d)) == 0);
    CHECK(fq_pushed(q) == 4);
    CHECK(fq_dropped(q) == 2);

    /* 参数不合法时在加锁前就返回了，不该被算进去 */
    CHECK(fq_push(q, NULL, sizeof(a)) == -1);
    CHECK(fq_push(q, &a, sizeof(a) + 1) == -1);
    CHECK(fq_pushed(q) == 4);
    CHECK(fq_dropped(q) == 2);   /* 仍是 2，那两次失败调用没被算进来 */

    fq_destroy(q);
}

/* 8. 并发：生产者线程 + 主线程消费者 */
static void *producer_thread(void *arg)
{
    frame_queue *q = (frame_queue *)arg;
    for (int i = 0; i < 100; i++)
        fq_push(q, &i, sizeof(i));
    return NULL;
}

static void test_concurrent(void)
{
    /* 容量 128 > 100，确保不丢帧 */
    frame_queue *q = fq_create(128, sizeof(int), FQ_DROP_OLDEST);
    CHECK(q != NULL);

    pthread_t th;
    CHECK(pthread_create(&th, NULL, producer_thread, q) == 0);

    for (int i = 0; i < 100; i++) {
        int v = -1; size_t sz = sizeof(v);
        CHECK(fq_pop(q, &v, &sz, 1000) == 0);   /* 1000ms 兜底，防死锁 */
        CHECK(v == i);
    }

    pthread_join(th, NULL);
    fq_destroy(q);
}

int main(void)
{
    test_create_and_roundtrip();
    test_fifo_order();
    test_drop_oldest();
    test_drop_newest();
    test_empty_timeout();
    test_invalid_args();
    test_counters();
    test_concurrent();

    printf("== %d 个断言, %d 个失败 ==\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
