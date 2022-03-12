#ifndef CONDITION_H_
#define CONDITION_H_

#include <pthread.h>
#include <cstdio>

typedef struct condition {
    /**
     * 互斥锁
     */
    pthread_mutex_t pmutex;
    /**
     * 条件变量
     */
    pthread_cond_t  pcond;
} condition_t;

/**
 * 初始化
 */
int condition_init(condition_t* cond);

/**
 * 加锁
 */
int condition_lock(condition_t* cond);
/**
 * 解锁
 */
int condition_unlock(condition_t* cond);

/**
 * 条件等待
 * 
 * pthread_cond_wait(cond, mutex)的功能有3个:
 * 1) 调用者线程首先释放mutex
 * 2) 然后阻塞, 等待被别的线程唤醒
 * 3) 当调用者线程被唤醒后，调用者线程会再次获取mutex
 */
int condition_wait(condition_t* cond);

/**
 * 计时等待
 */
int condition_timedwait(condition_t* cond, const timespec* abstime);

/**
 * 激活一个等待该条件的线程
 * 
 * 1) 作用: 发送一个信号给另外一个处于阻塞等待状态的线程, 使其脱离阻塞状态继续执行
 * 2) 如果没有线程处在阻塞状态, 那么pthread_cond_signal也会成功返回, 所以需要判断下idle thread的数量
 * 3) 最多只会给一个线程发信号，不会有「惊群现象」
 * 4) 首先根据线程优先级的高低确定发送给哪个线程信号, 如果优先级相同则优先发给等待最久的线程
 * 5) 重点: pthread_cond_wait必须放在lock和unlock之间, 因为他要根据共享变量的状态决定是否要等待; 但是pthread_cond_signal既可以放在lock和unlock之间，也可以放在lock和unlock之后
 */
int condition_signal(condition_t *cond);
/**
 * 唤醒所有等待线程
 */
int condition_broadcast(condition_t *cond);

/**
 * 销毁
 */
int condition_destroy(condition_t *cond);

#endif  // CONDITION_H_