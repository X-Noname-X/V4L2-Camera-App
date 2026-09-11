#define _POSIX_C_SOURCE 200809L   //启用 POSIX.1-2008，让 clock_gettime / CLOCK_REALTIME 可见
#include "camera/frame_queue.h"
#include <stdint.h>   // uint8_t
#include <stdlib.h>   // malloc / free
#include <pthread.h>  // 互斥锁 / 条件变量
#include <string.h>   // memcpy
#include <stdatomic.h>// _Atomic，供跨线程读计数用
#include <time.h>
#include <errno.h>

struct frame_queue
{
    size_t capacity;
    size_t frame_size;
    fq_policy policy;
    uint8_t *buf;
    size_t head;       // 下一个要写入的"逻辑帧号"（单调递增，取模映射到槽位）
    size_t tail;       // 下一个要读出的"逻辑帧号"（队首）
    size_t count;      // 当前缓冲帧数，恒等于 head - tail，范围 [0,capacity]
    _Atomic unsigned long pushed;   // 累计入队帧数：生产者一共送来多少
    _Atomic unsigned long dropped;  // 其中因队列满被丢掉的
    pthread_mutex_t lock;
    pthread_cond_t not_empty;  // 队列由空变非空时，唤醒等待中的消费者
};

frame_queue *fq_create(size_t capacity, size_t frame_size, fq_policy policy)
{
    frame_queue *fq;
    if (capacity == 0 || frame_size == 0) return NULL;
    fq = malloc(sizeof(*fq));
    if (!fq) return NULL;

    fq->buf = malloc(capacity * frame_size);
    if (!fq->buf){
        free(fq);   // 分配buf失败，需要将已经分到的fq还回去
        return NULL;
    }
    
    fq->capacity = capacity;
    fq->frame_size = frame_size;
    fq->policy = policy;
    fq->head = 0;
    fq->tail = 0;
    fq->count = 0;
    atomic_store(&fq->pushed, 0);
    atomic_store(&fq->dropped, 0);

    pthread_mutex_init(&fq->lock, NULL);  // 初始化一把互斥锁，用来保护共享数据，保证同一时刻只有一个线程能改它
    pthread_cond_init(&fq->not_empty, NULL);  // 初始化一个条件变量，用来让线程 等某个条件成立，以及 叫醒等待的线程

    return fq;
}

void fq_destroy(frame_queue *fq)
{
    pthread_cond_destroy(&fq->not_empty);
    pthread_mutex_destroy(&fq->lock);
    free(fq->buf);
    free(fq);
}

size_t fq_frame_size(const frame_queue *fq)
{
    return fq ? fq->frame_size : 0;
}

/* 统计计数：写在锁里（push 那条路径），读可能在别的线程（stats 走主线程），
 * 所以用 _Atomic 读，避免跨线程读一个正在被改的普通变量。 */
unsigned long fq_pushed(const frame_queue *fq)
{
    return fq ? atomic_load(&fq->pushed) : 0;
}

unsigned long fq_dropped(const frame_queue *fq)
{
    return fq ? atomic_load(&fq->dropped) : 0;
}

int fq_push(frame_queue *fq, const void *data, size_t size)
{
    if (!fq || !data || size != fq->frame_size) return -1;
    pthread_mutex_lock(&fq->lock);
    fq->pushed++;
    if (fq->count == fq->capacity) {
        /* 满了。两种策略都得丢掉一帧：DROP_OLDEST 丢队首那帧，
         * DROP_NEWEST 丢刚送来的这帧——无论哪种，净效果都是少了一帧。 */
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
    size_t offset = (fq->head % fq->capacity) * fq->frame_size;
    memcpy(fq->buf + offset, data, size);
    fq->head++;
    fq->count++;
    pthread_cond_signal(&fq->not_empty);  // 唤醒可能在等空的消费者：对应fq_pop中的pthread_cond_wait和pthread_cond_timedwait
    pthread_mutex_unlock(&fq->lock);
    return 0;
}

int fq_pop(frame_queue *fq, void *out, size_t *size, int timeout_ms)
{
    if (!fq || !out || !size || *size < fq->frame_size) return -1;
    pthread_mutex_lock(&fq->lock);
    while(fq->count == 0){
        if (timeout_ms < 0) pthread_cond_wait(&fq->not_empty, &fq->lock); // 1. 释放 fq->lock（把锁交出去）； 2. 当前线程睡着，等 fq->not_empty 被 signal
        else if (timeout_ms == 0){
            pthread_mutex_unlock(&fq->lock);
            return -1;
        }
        else{
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {   /* 纳秒进位 */
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            // 和pthread_cond_wait一样，只不过pthread_cond_wait会无限等，函数pthread_cond_timedwait只等timeout_ms时间就自己醒
            // 两者都会在睡眠时原子地放掉锁、醒来时拿回锁
            int rc = pthread_cond_timedwait(&fq->not_empty, &fq->lock, &ts);
            if (rc == ETIMEDOUT) {             /* 真超时了 */
                pthread_mutex_unlock(&fq->lock);
                return -1;
            }
        }
    }
    size_t offset = (fq->tail % fq->capacity) * fq->frame_size;
    memcpy(out, fq->buf + offset, fq->frame_size);
    fq->tail++;
    fq->count--;
    *size = fq->frame_size;
    pthread_mutex_unlock(&fq->lock);
    return 0;
}