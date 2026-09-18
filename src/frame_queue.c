#include "camera/frame_queue.h"
#include <stdint.h>    // uint8_t
#include <stdlib.h>    // malloc / free
#include <pthread.h>   // 互斥锁 / 条件变量
#include <string.h>    // memcpy
#include <stdatomic.h> // _Atomic，供跨线程读计数用
#include <time.h>
#include <errno.h>

struct frame_queue
{
    size_t capacity;
    size_t max_size;   // 每个槽能装的最大字节数（帧可以更短，比如 MJPEG）
    fq_policy policy;
    uint8_t *buf;      // capacity * max_size，所有槽一块连续内存
    size_t *sizes;     // 每个槽里实际存了多少字节
    size_t head;       // 下一个要写入的"逻辑帧号"（单调递增，取模映射到槽位）
    size_t tail;       // 下一个要读出的"逻辑帧号"（队首）
    size_t count;      // 当前缓冲帧数，恒等于 head - tail，范围 [0,capacity]
    _Atomic unsigned long dropped;  // 因队列满被丢掉的帧数（累计）
    pthread_mutex_t lock;
    pthread_cond_t not_empty;  // 队列由空变非空时，唤醒等待中的消费者
};

frame_queue *fq_create(size_t capacity, size_t max_size, fq_policy policy)
{
    frame_queue *fq;
    if (capacity == 0 || max_size == 0) return NULL;
    fq = malloc(sizeof(*fq));
    if (!fq) return NULL;

    fq->buf = malloc(capacity * max_size);
    fq->sizes = malloc(capacity * sizeof(*fq->sizes));
    if (!fq->buf || !fq->sizes){
        free(fq->buf);     // 两块缓冲任一分失败都要回滚，否则漏一块
        free(fq->sizes);
        free(fq);
        return NULL;
    }

    fq->capacity = capacity;
    fq->max_size = max_size;
    fq->policy = policy;
    fq->head = 0;
    fq->tail = 0;
    fq->count = 0;
    atomic_store(&fq->dropped, 0);

    // lock 保护共享数据，保证同一时刻只有一个线程能改它
    pthread_mutex_init(&fq->lock, NULL);
    // not_empty 让线程等某个条件成立，以及叫醒等待的线程
    pthread_cond_init(&fq->not_empty, NULL);

    return fq;
}

void fq_destroy(frame_queue *fq)
{
    if (!fq) return;
    pthread_cond_destroy(&fq->not_empty);
    pthread_mutex_destroy(&fq->lock);
    free(fq->buf);
    free(fq->sizes);
    free(fq);
}

size_t fq_max_size(const frame_queue *fq)
{
    return fq ? fq->max_size : 0;
}

/* 丢帧计数写在锁里（push 那条路径），读在主线程，所以用 _Atomic 读，
 * 避免跨线程读一个正在被改的普通变量 */
unsigned long fq_dropped(const frame_queue *fq)
{
    return fq ? atomic_load(&fq->dropped) : 0;
}

int fq_push(frame_queue *fq, const void *data, size_t size)
{
    if (!fq || !data || size == 0 || size > fq->max_size) return -1;
    pthread_mutex_lock(&fq->lock);
    if (fq->count == fq->capacity) {
        /* 满了，两种策略都得丢掉一帧：DROP_OLDEST 丢队首那帧，
         * DROP_NEWEST 丢刚送来的这帧，无论哪种净效果都是少了一帧 */
        fq->dropped++;
        switch (fq->policy)
        {
        case FQ_DROP_OLDEST:
            fq->tail++;
            fq->count--;
            break;
        case FQ_DROP_NEWEST:
            pthread_mutex_unlock(&fq->lock);
            return 0;
        default:
            break;
        }
    }
    size_t slot = fq->head % fq->capacity;
    memcpy(fq->buf + slot * fq->max_size, data, size);
    fq->sizes[slot] = size;
    fq->head++;
    fq->count++;
    pthread_cond_signal(&fq->not_empty);   // 叫醒可能在等空的消费者
    pthread_mutex_unlock(&fq->lock);
    return 0;
}

int fq_pop(frame_queue *fq, void *out, size_t *size, int timeout_ms)
{
    if (!fq || !out || !size) return -1;
    pthread_mutex_lock(&fq->lock);
    while(fq->count == 0){
        // 睡眠时原子地放掉锁，醒来时拿回锁；无限等 or 等够 timeout_ms 就走
        if (timeout_ms < 0){
            pthread_cond_wait(&fq->not_empty, &fq->lock);
        }
        else if (timeout_ms == 0){
            pthread_mutex_unlock(&fq->lock);
            return -1;
        }
        else{
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {   // 纳秒进位
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            if (pthread_cond_timedwait(&fq->not_empty, &fq->lock, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&fq->lock);
                return -1;
            }
        }
    }
    /* 实际长度只有拿到槽位才知道（变长帧），所以这一步必须在锁内做 */
    size_t slot = fq->tail % fq->capacity;
    size_t n = fq->sizes[slot];
    if (*size < n) {                    // 调用方给的缓冲装不下这一帧
        pthread_mutex_unlock(&fq->lock);
        return -1;
    }
    memcpy(out, fq->buf + slot * fq->max_size, n);
    fq->tail++;
    fq->count--;
    *size = n;
    pthread_mutex_unlock(&fq->lock);
    return 0;
}
