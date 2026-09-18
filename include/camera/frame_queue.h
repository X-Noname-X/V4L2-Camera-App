#ifndef FRAME_QUEUE_H_
#define FRAME_QUEUE_H_

#include <stddef.h>   // size_t

/* 丢帧策略：队列已满时，如何处理新来的帧 */
typedef enum {
    FQ_DROP_OLDEST,  // 丢最旧的帧保留最新：只要最新画面的场景
    FQ_DROP_NEWEST   // 丢最新的帧保留最早：一帧不跳的录像场景
} fq_policy;

/* 不透明类型：内部结构只在 frame_queue.c 里定义，外部只能拿到指针 */
typedef struct frame_queue frame_queue;

/* 创建有界环形缓冲队列
 *   capacity : 容量（最多缓存的帧数）
 *   max_size : 每个槽能装的最大字节数，存入的帧可以比它短（MJPEG 每帧长度
 *              都不一样），但不能超过
 *   policy   : 队列满时的丢帧策略
 * 内存创建时一次性分配、循环复用，热路径零 malloc
 * 成功返回队列指针，失败返回 NULL */
frame_queue *fq_create(size_t capacity, size_t max_size, fq_policy policy);

/* 释放队列及其占用的全部内存 */
void fq_destroy(frame_queue *fq);

/* 入队一帧
 *   data : 帧数据
 *   size : 实际字节数，须在 (0, max_size] 之间
 * 成功返回 0，参数非法或太长返回 -1 */
int fq_push(frame_queue *fq, const void *data, size_t size);

/* 出队一帧到 out
 *   size       : 入参为 out 的容量，出参为这一帧的实际长度
 *   timeout_ms : <0 阻塞等待；=0 立即返回；>0 等待指定毫秒数
 * 成功返回 0，超时（队列空）或 out 装不下返回 -1 */
int fq_pop(frame_queue *fq, void *out, size_t *size, int timeout_ms);

/* 每个槽的最大字节数，供 cap_start 校验队列能不能装下驱动给的最大帧 */
size_t fq_max_size(const frame_queue *fq);

/* 因队列满被丢掉的帧数（累计），计数在锁内更新、原子读取，跨线程调用安全 */
unsigned long fq_dropped(const frame_queue *fq);

#endif // FRAME_QUEUE_H_
