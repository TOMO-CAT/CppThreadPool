# C++线程池

## 线程池概念

> 假设完成一项任务需要的时间=创建线程时间T1+线程执行任务时间T2+销毁线程时间T3，如果T1+T3的时间远大于T2，通常就可以考虑采取线程池来提高服务器的性能

`thread pool`就是线程的一种使用模式，一个线程池中维护着多个线程等待接收管理者分配的可并发执行的任务。

* 避免了处理短时间任务时创建与销毁线程的代价
* 既保证内核的充分利用，又能防止过度调度
* 可用线程数量应该取决于可用的并发处理器、处理器内核、内存、网络sockets的数量

## 线程池组成部分

* 线程池管理器（thread pool）：创建、销毁线程池
* 工作线程（pool wroker）：在没有任务时处于等待状态，循环读取并执行任务队列中的任务
* 任务（task）：抽象一个任务，主要规定任务的入口、任务执行完后的收尾工作、任务的执行状态等
* 任务队列（task queue）：存放没有处理的任务，提供一种缓冲机制

## C风格ThreadPool

#### 1. 抽象一个任务

将待处理的任务抽象成task结构：

```c++
typedef struct task {
    void* (*run)(void* args);  // abstract a job function that need to run
    void* arg;                 // argument of the run function
    struct task* next;         // point to the next task in task queue
} task_t;
```

#### 2. 任务队列

* `threadpool`中用`first`和`last`指针指向首尾两个任务
* `task`结构体保证每个`task`都能指向任务队列中下一个`task`

```c++
typedef struct task {
    void* (*run)(void* args);  // abstract a job function that need to run
    void* arg;                 // argument of the run function
    struct task* next;         // point to the next task in task queue
} task_t;

typedef struct threadpool {
    condition_t ready;  // condition & mutex
    task_t* first;      // fist task in task queue
    task_t* last;       // last task in task queue
    int counter;        // total task number
    int idle;           // idle task number
    int max_threads;    // max task number
    int quit;           // the quit flag
} threadpool_t;

```

#### 3. 线程安全的问题

设计了`condition_t`类来实现安全并发：

```c++
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
```

提供对应的接口：

```c++
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
```

#### 4. 线程池的实现

###### 4.1 初始化一个线程池

仅仅是初始化了`condition`和`mutex`，还有一些线程池的属性。**但是任务队列是空的，而且此时也一个线程都没有**。

```c++
// initialize the thread pool
void threadpool_init(threadpool_t* pool, int threads_num) {
    int n_status = condition_init(&pool ->ready);
    if (n_status == 0) {
        printf("Info: initialize the thread pool successfully!\n");
    } else {
        printf("Error: initialize the thread pool failed, status:%d\n", n_status);
    }
    pool->first = NULL;
    pool->last = NULL;
    pool->counter = 0;
    pool->idle = 0;
    pool->max_threads = threads_num;
    pool->quit = 0;
}
```

###### 4.2 向线程池中添加任务，并分配给它一个线程

首先构建`task`结构体，然后将其加入任务队列。

* 如果当前有空闲线程那么直接调用空闲线程执行函数
* 如果无空闲线程且当前线程数未满时创建一个新的线程执行任务
* 如果无空闲线程且当前线程数已满时，任务会呆在任务队列中等待线程池释放出空闲线程

```c++
// add a task to thread pool
void threadpool_add_task(threadpool_t* pool, void* (*run)(void *arg), void* arg) {
    // create a task
    task_t* new_task = reinterpret_cast<task_t*>(malloc(sizeof(task_t)));
    new_task->run = run;
    new_task->arg = arg;
    new_task->next = NULL;

    // lock the condition
    condition_lock(&pool->ready);

    // add the task to task queue
    if (pool->first == NULL) {
        pool->first = new_task;
    } else {  // else add to the last task
        pool->last->next = new_task;
    }
    pool->last = new_task;

    /*
     * after you add a task to task queue, you need to allocate it to a thread:
     * (1)if idle thread num > 0: awake a idle thread
     * (2)if idle thread num = 0 & thread num does not reach maximum: create a new thread to run the task
     */
    if (pool->idle > 0) {
        // awake a thread that wait for longest time
        condition_signal(&pool->ready);
    } else if (pool->counter < pool->max_threads) {
        // define a tid to get the thread identifier that we are going to create
        pthread_t tid;
        /*
         * pthread_create():
         * (1)thread identifier
         * (2)set the pthread attribute
         * (3)the function that thread is going to run
         * (4)the args of run func
         *
         *  A realistic limit of thread num is 200 to 400 threads
         *  https://www.ibm.com/support/knowledgecenter/en/SSLTBW_2.3.0/com.ibm.zos.v2r3.bpxbd00/ptcrea.htm
         */
        pthread_create(&tid, NULL, thread_routine, pool);
        pool->counter++;
    } else {  // when (idle == 0 & counter = max_threads), then wait
        printf("Warning: no idle thread, please wait...\n");
    }

    condition_unlock(&pool->ready);
}
```

#### 5. 线程的执行过程

###### 5.1 如果任务队列为空

```c++
// when task queue is empty, then block 2 second to get the new task
// If timeout, then destroy the thread
while (pool->first == NULL && !pool->quit) {
    printf("Info: thread %ld is waiting for a task\n", (u_int64_t)pthread_self());
    // get the system time
    clock_gettime(CLOCK_REALTIME, &abs_name);
    abs_name.tv_sec += 2;
    int status;
    status = condition_timedwait(&pool->ready, &abs_name);  // block for 2 second
    if (status == ETIMEDOUT) {
        printf("Info: thread %ld wait timed out\n", (u_int64_t)pthread_self());
        timeout = true;
        break;
    }
}

...

// if visit task queue timeout(means no task in queue), quit destory the thread
if (timeout) {
    pool->counter--;
    condition_unlock(&pool->ready);
    break;  // destroy the thread
}
```

###### 5.2 如果任务队列非空

```c++
// when the thread run the task, we should unlock the thread pool
if (pool->first != NULL) {
    // get the task from task queue
    task_t* t = pool->first;
    pool->first = t->next;
    // unlock the thread pool to make other threads visit task queue
    condition_unlock(&pool->ready);

    // run the task run func
    t->run(t->arg);
    free(t);

    // lock
    condition_lock(&pool->ready);
}
```

###### 5.3 没有任务且收到退出信号

```c++
// when task queue is clean and quit flag is 1, then destroy the thread
if (pool->quit && pool->first == NULL) {
    pool->counter--;
    // 若线程池中线程数为0，通知等待线程（主线程）全部任务已经完成
    if (pool->counter == 0) {
        condition_signal(&pool->ready);
    }
    condition_unlock(&pool->ready);
    break;  // destroy the thread
}
```

#### 6. 代码

condition.h：

```c++
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
```

condition.cpp：

```c++
#include "condition.h"

// 初始化
int condition_init(condition_t* cond) {
    int status;
    status = pthread_mutex_init(&cond->pmutex, NULL);
    if (status != 0) {
        printf("Error: pthread_mutex_init failed, return value:%d\n", status);
        return status;
    }
    status = pthread_cond_init(&cond->pcond, NULL);
    if (status != 0) {
        printf("Error: pthread_cond_init failed, return value:%d\n", status);
        return status;
    }
    return 0;
}

// 加锁
int condition_lock(condition_t* cond) {
    return pthread_mutex_lock(&cond->pmutex);
}

// 解锁
int condition_unlock(condition_t* cond) {
    return pthread_mutex_unlock(&cond->pmutex);
}

// 条件等待
int condition_wait(condition_t* cond) {
    return pthread_cond_wait(&cond->pcond, &cond->pmutex);
}

// 计时等待
int condition_timedwait(condition_t* cond, const timespec* abstime) {
    return pthread_cond_timedwait(&cond->pcond, &cond->pmutex, abstime);
}

// 激活一个等待该条件的线程
int condition_signal(condition_t *cond) {
    return pthread_cond_signal(&cond->pcond);
}

// 唤醒所有等待线程
int condition_broadcast(condition_t *cond) {
    return pthread_cond_broadcast(&cond->pcond);
}

// 销毁
int condition_destroy(condition_t *cond) {
    int status;
    status = pthread_mutex_destroy(&cond->pmutex);
    if (status != 0) {
        return status;
    }

    status = pthread_cond_destroy(&cond->pcond);
    if (status != 0) {
        return status;
    }
    return 0;
}
```

threadpool.h：

```c++
#ifndef THREAD_POLL_H_
#define THREAD_POLL_H_

#include "condition.h"

typedef struct task {
    void* (*run)(void* args);  // abstract a job function that need to run
    void* arg;                 // argument of the run function
    struct task* next;         // point to the next task in task queue
} task_t;

typedef struct threadpool {
    condition_t ready;  // condition & mutex
    task_t* first;      // fist task in task queue
    task_t* last;       // last task in task queue
    int counter;        // total task number
    int idle;           // idle task number
    int max_threads;    // max task number
    int quit;           // the quit flag
} threadpool_t;

/**
 * initialize threadpool
 */ 
void threadpool_init(threadpool_t* pool, int threads_num);

/**
 * add a task to threadpool
 */
void threadpool_add_task(threadpool_t* pool, void* (*run)(void *args), void* arg);

/**
 * destroy threadpool
 */
void threadpool_destroy(threadpool_t* pool);

#endif  // THREAD_POLL_H_
```

Threadpool.cpp：

```c++
#include <pthread.h>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include "threadpool.h"

void *thread_routine(void *arg);

// initialize the thread pool
void threadpool_init(threadpool_t* pool, int threads_num) {
    int n_status = condition_init(&pool ->ready);
    if (n_status == 0) {
        printf("Info: initialize the thread pool successfully!\n");
    } else {
        printf("Error: initialize the thread pool failed, status:%d\n", n_status);
    }
    pool->first = NULL;
    pool->last = NULL;
    pool->counter = 0;
    pool->idle = 0;
    pool->max_threads = threads_num;
    pool->quit = 0;
}

// add a task to thread pool
void threadpool_add_task(threadpool_t* pool, void* (*run)(void *arg), void* arg) {
    // create a task
    task_t* new_task = reinterpret_cast<task_t*>(malloc(sizeof(task_t)));
    new_task->run = run;
    new_task->arg = arg;
    new_task->next = NULL;

    // lock the condition
    condition_lock(&pool->ready);

    // add the task to task queue
    if (pool->first == NULL) {
        pool->first = new_task;
    } else {  // else add to the last task
        pool->last->next = new_task;
    }
    pool->last = new_task;

    /*
     * after you add a task to task queue, you need to allocate it to a thread:
     * (1)if idle thread num > 0: awake a idle thread
     * (2)if idle thread num = 0 & thread num does not reach maximum: create a new thread to run the task
     */
    if (pool->idle > 0) {
        // awake a thread that wait for longest time
        condition_signal(&pool->ready);
    } else if (pool->counter < pool->max_threads) {
        // define a tid to get the thread identifier that we are going to create
        pthread_t tid;
        /*
         * pthread_create():
         * (1)thread identifier
         * (2)set the pthread attribute
         * (3)the function that thread is going to run
         * (4)the args of run func
         *
         *  A realistic limit of thread num is 200 to 400 threads
         *  https://www.ibm.com/support/knowledgecenter/en/SSLTBW_2.3.0/com.ibm.zos.v2r3.bpxbd00/ptcrea.htm
         */
        pthread_create(&tid, NULL, thread_routine, pool);
        pool->counter++;
    } else {  // when (idle == 0 & counter = max_threads), then wait
        printf("Warning: no idle thread, please wait...\n");
    }

    condition_unlock(&pool->ready);
}

// create a thread to run the task run func
// and the void *arg means the arg passed by pthread_create: pool
void *thread_routine(void *arg) {
    struct timespec abs_name;
    bool timeout;
    printf("Info: create thread, and the thread id is: %ld\n", (u_int64_t)pthread_self());
    threadpool_t *pool = reinterpret_cast<threadpool_t *>(arg);

    // keep visiting the task queue
    while (true) {
        timeout = false;
        condition_lock(&pool->ready);
        pool->idle++;

        // when task queue is empty, then block 2 second to get the new task
        // If timeout, then destroy the thread
        while (pool->first == NULL && !pool->quit) {
            printf("Info: thread %ld is waiting for a task\n", (u_int64_t)pthread_self());
            // get the system time
            clock_gettime(CLOCK_REALTIME, &abs_name);
            abs_name.tv_sec += 2;
            int status;
            status = condition_timedwait(&pool->ready, &abs_name);  // block for 2 second
            if (status == ETIMEDOUT) {
                printf("Info: thread %ld wait timed out\n", (u_int64_t)pthread_self());
                timeout = true;
                break;
            }
        }

        pool->idle--;
        // when the thread run the task, we should unlock the thread pool
        if (pool->first != NULL) {
            // get the task from task queue
            task_t* t = pool->first;
            pool->first = t->next;
            // unlock the thread pool to make other threads visit task queue
            condition_unlock(&pool->ready);

            // run the task run func
            t->run(t->arg);
            free(t);

            // lock
            condition_lock(&pool->ready);
        }

        // when task queue is clean and quit flag is 1, then destroy the thread
        if (pool->quit && pool->first == NULL) {
            pool->counter--;
            // 若线程池中线程数为0，通知等待线程（主线程）全部任务已经完成
            if (pool->counter == 0) {
                condition_signal(&pool->ready);
            }
            condition_unlock(&pool->ready);
            break;  // destroy the thread
        }

        // if visit task queue timeout(means no task in queue), quit destory the thread
        if (timeout) {
            pool->counter--;
            condition_unlock(&pool->ready);
            break;  // destroy the thread
        }

        condition_unlock(&pool->ready);
    }

    // if break, destroy the thread
    printf("Info: thread %ld quit\n", (u_int64_t)pthread_self());
    return NULL;
}

/*
 * destroy a thread pool:
 * 1) awake all the idle thread
 * 2) wait for the running thread to finish
 */
void threadpool_destroy(threadpool_t *pool) {
    if (pool->quit) {
        return;
    }

    condition_lock(&pool->ready);
    pool->quit = 1;
    if (pool->counter > 0) {
        if (pool->idle > 0) {
            condition_broadcast(&pool->ready);
        }
        while (pool->counter > 0) {
            condition_wait(&pool->ready);
        }
    }
    condition_unlock(&pool->ready);
    condition_destroy(&pool->ready);
}
```

test.cpp：

```c++
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "threadpool.h"

#define  THREADPOOL_MAX_NUM 30

void* mytask(void *arg) {
    printf("Info: thread %ld is working on task %d\n", (u_int64_t)pthread_self(), *reinterpret_cast<int*>(arg));
    sleep(1);
    free(arg);
    return NULL;
}

int main(int argc, char* argv[]) {
    threadpool_t pool;
    threadpool_init(&pool, THREADPOOL_MAX_NUM);

    // add task to task queue
    for (int i=0; i < 100; i++) {
        int *arg = reinterpret_cast<int *>(malloc(sizeof(int)));
        *arg = i;
        threadpool_add_task(&pool, mytask, arg);
    }
    threadpool_destroy(&pool);
    return 0;
}
```

编译运行：

```bash
$g++ -g test.cpp threadpool.cpp condition.cpp -o test -std=c++11 -lpthread
$./test
Info: initialize the thread pool successfully!
Info: create thread, and the thread id is: 139898193295104
Info: create thread, and the thread id is: 139898176509696
Info: thread 139898176509696 is working on task 0
Info: create thread, and the thread id is: 139898168116992
Info: create thread, and the thread id is: 139898184902400
Info: create thread, and the thread id is: 139898134546176
Info: create thread, and the thread id is: 139898126153472
Info: create thread, and the thread id is: 139898117760768
Info: thread 139898117760768 is working on task 1
Info: create thread, and the thread id is: 139898100975360
Info: create thread, and the thread id is: 139898092582656
Info: create thread, and the thread id is: 139898084189952
Info: create thread, and the thread id is: 139898159724288
Info: create thread, and the thread id is: 139898109368064
Info: create thread, and the thread id is: 139898067404544
Info: create thread, and the thread id is: 139898059011840
Info: create thread, and the thread id is: 139898050619136
Info: create thread, and the thread id is: 139898042226432
Info: create thread, and the thread id is: 139898033833728
Info: create thread, and the thread id is: 139898025441024
Info: create thread, and the thread id is: 139898017048320
Info: create thread, and the thread id is: 139898008655616
Info: create thread, and the thread id is: 139898075797248
Info: create thread, and the thread id is: 139898000262912
Info: create thread, and the thread id is: 139898142938880
Info: create thread, and the thread id is: 139898151331584
Info: thread 139898159724288 is working on task 2
Info: thread 139898151331584 is working on task 3
Info: create thread, and the thread id is: 139897991870208
Info: create thread, and the thread id is: 139897966692096
Info: create thread, and the thread id is: 139897958299392
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Info: create thread, and the thread id is: 139897949906688
Info: create thread, and the thread id is: 139897983477504
Info: create thread, and the thread id is: 139897975084800
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Info: thread 139898067404544 is working on task 4
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Info: thread 139898168116992 is working on task 5
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Warning: no idle thread, please wait...
Info: thread 139898142938880 is working on task 6
Warning: no idle thread, please wait...
Info: thread 139898042226432 is working on task 7
Warning: no idle thread, please wait...
Info: thread 139897949906688 is working on task 8
Info: thread 139898184902400 is working on task 11
Info: thread 139898134546176 is working on task 13
Info: thread 139898017048320 is working on task 14
Info: thread 139898008655616 is working on task 16
Info: thread 139898193295104 is working on task 18
Info: thread 139898000262912 is working on task 20
Info: thread 139898100975360 is working on task 21
Info: thread 139897983477504 is working on task 9
Info: thread 139897975084800 is working on task 10
Info: thread 139898092582656 is working on task 26
Info: thread 139898050619136 is working on task 24
Info: thread 139897991870208 is working on task 28
Info: thread 139898025441024 is working on task 12
Info: thread 139898084189952 is working on task 15
Info: thread 139898109368064 is working on task 17
Info: thread 139897966692096 is working on task 25
Info: thread 139898075797248 is working on task 19
Info: thread 139898059011840 is working on task 22
Info: thread 139897958299392 is working on task 27
Info: thread 139898033833728 is working on task 29
Info: thread 139898126153472 is working on task 23
Info: thread 139898176509696 is working on task 30
Info: thread 139898117760768 is working on task 31
Info: thread 139898159724288 is working on task 32
Info: thread 139898151331584 is working on task 33
Info: thread 139898067404544 is working on task 34
Info: thread 139898168116992 is working on task 35
Info: thread 139898142938880 is working on task 36
Info: thread 139898042226432 is working on task 37
Info: thread 139897949906688 is working on task 38
Info: thread 139898184902400 is working on task 39
Info: thread 139898000262912 is working on task 40
Info: thread 139898017048320 is working on task 41
Info: thread 139898008655616 is working on task 42
Info: thread 139898134546176 is working on task 43
Info: thread 139898193295104 is working on task 44
Info: thread 139898050619136 is working on task 49
Info: thread 139897991870208 is working on task 50
Info: thread 139898025441024 is working on task 51
Info: thread 139898084189952 is working on task 52
Info: thread 139898109368064 is working on task 53
Info: thread 139898075797248 is working on task 54
Info: thread 139897975084800 is working on task 48
Info: thread 139898100975360 is working on task 45
Info: thread 139898033833728 is working on task 57
Info: thread 139897983477504 is working on task 47
Info: thread 139897966692096 is working on task 55
Info: thread 139897958299392 is working on task 56
Info: thread 139898092582656 is working on task 46
Info: thread 139898126153472 is working on task 58
Info: thread 139898059011840 is working on task 59
Info: thread 139898176509696 is working on task 60
Info: thread 139898117760768 is working on task 61
Info: thread 139898159724288 is working on task 62
Info: thread 139898151331584 is working on task 63
Info: thread 139898067404544 is working on task 64
Info: thread 139898168116992 is working on task 65
Info: thread 139898142938880 is working on task 66
Info: thread 139898042226432 is working on task 67
Info: thread 139897949906688 is working on task 69
Info: thread 139898184902400 is working on task 68
Info: thread 139898000262912 is working on task 70
Info: thread 139898008655616 is working on task 71
Info: thread 139898017048320 is working on task 72
Info: thread 139898050619136 is working on task 73
Info: thread 139898134546176 is working on task 74
Info: thread 139898109368064 is working on task 78
Info: thread 139898100975360 is working on task 80
Info: thread 139897975084800 is working on task 82
Info: thread 139898075797248 is working on task 81
Info: thread 139897966692096 is working on task 85
Info: thread 139897958299392 is working on task 86
Info: thread 139898126153472 is working on task 88
Info: thread 139898059011840 is working on task 89
Info: thread 139898092582656 is working on task 87
Info: thread 139898025441024 is working on task 76
Info: thread 139898084189952 is working on task 77
Info: thread 139898193295104 is working on task 75
Info: thread 139897991870208 is working on task 79
Info: thread 139898033833728 is working on task 83
Info: thread 139897983477504 is working on task 84
Info: thread 139898176509696 is working on task 90
Info: thread 139898117760768 is working on task 91
Info: thread 139898159724288 is working on task 92
Info: thread 139898151331584 is working on task 93
Info: thread 139898067404544 is working on task 94
Info: thread 139898168116992 is working on task 95
Info: thread 139898142938880 is working on task 96
Info: thread 139898042226432 is working on task 97
Info: thread 139897949906688 is working on task 98
Info: thread 139898184902400 is working on task 99
Info: thread 139898000262912 quit
Info: thread 139898008655616 quit
Info: thread 139898017048320 quit
Info: thread 139898050619136 quit
Info: thread 139898134546176 quit
Info: thread 139898109368064 quit
Info: thread 139898100975360 quit
Info: thread 139897966692096 quit
Info: thread 139898126153472 quit
Info: thread 139897975084800 quit
Info: thread 139898075797248 quit
Info: thread 139897958299392 quit
Info: thread 139898059011840 quit
Info: thread 139898092582656 quit
Info: thread 139898025441024 quit
Info: thread 139898084189952 quit
Info: thread 139898193295104 quit
Info: thread 139897991870208 quit
Info: thread 139898033833728 quit
Info: thread 139897983477504 quit
Info: thread 139898176509696 quit
Info: thread 139898117760768 quit
Info: thread 139898159724288 quit
Info: thread 139898151331584 quit
Info: thread 139898067404544 quit
Info: thread 139898168116992 quit
Info: thread 139898142938880 quit
Info: thread 139898042226432 quit
Info: thread 139897949906688 quit
Info: thread 139898184902400 quit
```

## C++03特性的ThreadPool

#### 1. 基于条件变量的线程池

threadpool.h：

```c++
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stdio.h>
#include <pthread.h>
#include <functional>
#include <vector>
#include <queue>

class ThreadPool {
 public:
    typedef void *(WrokerFunc)(void* arg);

    struct Task {
        WrokerFunc* run;
        void* arg;
    };

    explicit ThreadPool(int thread_num);
    ~ThreadPool();
    void addTask(WrokerFunc* func, void* arg);

 private:
    std::queue<Task*> task_queue_;
    std::vector<pthread_t> thread_list_;
    bool is_running_;  // note: is_running_不用原子变量或者锁操作可能存在卡死问题
    pthread_mutex_t mutex_;
    pthread_cond_t condition_;

    static void* thread_routine(void* pool_ptr);
    void thread_worker();
};


// =========================implementation=========================
inline ThreadPool::ThreadPool(int thread_num) : is_running_(true) {
    pthread_mutex_init(&mutex_, NULL);
    pthread_cond_init(&condition_, NULL);

    for (int i = 0; i < thread_num; i++) {
        pthread_t pid;
        pthread_create(&pid, NULL, thread_routine, this);
        thread_list_.push_back(pid);
    }
}

inline ThreadPool::~ThreadPool() {
    pthread_mutex_lock(&mutex_);
    is_running_ = false;
    pthread_mutex_unlock(&mutex_);

    pthread_cond_broadcast(&condition_);  // wakeup all threads that block to get task
    for (int i = 0; i < thread_list_.size(); i++) {
        pthread_join(thread_list_[i], NULL);
    }

    pthread_cond_destroy(&condition_);
    pthread_mutex_destroy(&mutex_);
}

inline void ThreadPool::addTask(WrokerFunc* func, void* arg) {
    Task* task = new Task();
    task->run = func;
    task->arg = arg;

    pthread_mutex_lock(&mutex_);
    task_queue_.push(task);
    pthread_mutex_unlock(&mutex_);
    pthread_cond_signal(&condition_);
}

inline void* ThreadPool::thread_routine(void* pool_ptr) {
    ThreadPool* pool = static_cast<ThreadPool*>(pool_ptr);
    pool->thread_worker();
}

inline void ThreadPool::thread_worker() {
    Task* task = NULL;

    while (true) {
        pthread_mutex_lock(&mutex_);
        if (!is_running_) {
            pthread_mutex_unlock(&mutex_);
            break;
        }

        if (task_queue_.empty()) {
            pthread_cond_wait(&condition_, &mutex_);  // 获取不到任务时阻塞, 直到有新的任务入队
            if (task_queue_.empty()) {
                pthread_mutex_unlock(&mutex_);
                continue;
            }
        }
        task = task_queue_.front();
        task_queue_.pop();
        pthread_mutex_unlock(&mutex_);

        (*(task->run))(task->arg);
        delete task;
        task = NULL;
    }

    // 线程池终止时(is_running_ = false)确保任务队列为空后退出
    while (true) {
        pthread_mutex_lock(&mutex_);
        if (task_queue_.empty()) {
            pthread_mutex_unlock(&mutex_);
            break;
        }
        task = task_queue_.front();
        task_queue_.pop();
        pthread_mutex_unlock(&mutex_);
        delete task;
        task = NULL;
    }

    printf("Info: thread[%lu] exit\n", pthread_self());
}


#endif  // THREAD_POOL_H
```

测试代码threadpool_test.cpp：

```c++
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <vector>
#include "threadpool.h"

void* MyTaskFunc(void* arg) {
    int* i = static_cast<int*>(arg);
    printf("[MyTaskFunc]: thread[%lu] is working on %d\n", pthread_self(), *i);
    return NULL;
}

int main() {
    ThreadPool pool(10);

    for (int i = 0; i < 100; i++) {
        int* arg = new int(i);
        pool.addTask(&MyTaskFunc, arg);
    }

    return 0;
}
```

编译运行：

```bash
$g++ -g threadpool_test.cpp -o threadpool_test -lpthread
$./threadpool_test 
[MyTaskFunc]: thread[140224777099008] is working on 0
[MyTaskFunc]: thread[140224793884416] is working on 8
[MyTaskFunc]: thread[140224844240640] is working on 2
Info: thread[140224844240640] exit
[MyTaskFunc]: thread[140224827455232] is working on 4
Info: thread[140224827455232] exit
[MyTaskFunc]: thread[140224810669824] is working on 6
Info: thread[140224810669824] exit
[MyTaskFunc]: thread[140224777099008] is working on 10
Info: thread[140224777099008] exit
[MyTaskFunc]: thread[140224852633344] is working on 1
Info: thread[140224852633344] exit
[MyTaskFunc]: thread[140224835847936] is working on 3
Info: thread[140224835847936] exit
[MyTaskFunc]: thread[140224802277120] is working on 7
Info: thread[140224802277120] exit
Info: thread[140224793884416] exit
[MyTaskFunc]: thread[140224819062528] is working on 5
Info: thread[140224819062528] exit
[MyTaskFunc]: thread[140224785491712] is working on 9
Info: thread[140224785491712] exit
```

#### 2. 基于信号量的同步任务队列

上述的线程池无法很好地支持同步任务，因此我们基于信号量实现了SyncTaskQueue。

sync_task_queue.h：

```c++
#ifndef SYNC_TASK_QUEUE_H
#define SYNC_TASK_QUEUE_H

#include <semaphore.h>
#include <vector>
#include "threadpool.h"

class SyncTaskQueue {
 public:
    struct SyncTask {
        ThreadPool::Task task;
        sem_t* sem;
    };

    explicit SyncTaskQueue(ThreadPool* pool_ptr);
    ~SyncTaskQueue();
    void addTask(ThreadPool::WrokerFunc* func, void* sync_task_ptr);
    void wait();

 private:
    ThreadPool* threadpool_;
    sem_t sem_;
    int sync_task_num_;  // 务必保证单线程读写, 否则需要加锁

    static ThreadPool::WrokerFunc workerFuncWrapper;
};

inline SyncTaskQueue::SyncTaskQueue(ThreadPool* pool_ptr) : threadpool_(pool_ptr), sync_task_num_(0) {
    sem_init(&sem_, 0, 0);
}

inline SyncTaskQueue::~SyncTaskQueue() {
    // make sure that all task has been finished before destory
    if (sync_task_num_ > 0) {
        wait();
    }
    sem_destroy(&sem_);
}

inline void SyncTaskQueue::addTask(ThreadPool::WrokerFunc* func, void* arg) {
    sync_task_num_++;

    // wrapper the worker function with sem
    SyncTask* sync_task = new SyncTask();
    sync_task->sem = &(this->sem_);
    sync_task->task.run = func;
    sync_task->task.arg = arg;
    threadpool_->addTask(&workerFuncWrapper, sync_task);
}

inline void SyncTaskQueue::wait() {
    while (sync_task_num_) {
        int sem_value = 0;
        sem_wait(&sem_);
        sync_task_num_--;
    }
}

inline void* SyncTaskQueue::workerFuncWrapper(void* sync_task_ptr) {
    SyncTask* sync_task = static_cast<SyncTask*>(sync_task_ptr);
    (*(sync_task->task.run))(sync_task->task.arg);
    sem_post(sync_task->sem);
    delete sync_task;
}


#endif  // SYNC_TASK_QUEUE_H
```

测试文件sync_task_queue_test.cpp：

```c++
#include <unistd.h>
#include "sync_task_queue.h"

void* MyTaskFunc(void* arg) {
    int i = *static_cast<int*>(arg);
    printf("[MyTaskFunc]: thread[%lu] is working on %d\n", pthread_self(), i);
    sleep(2);
    return NULL;
}

int main() {
    ThreadPool threadpool(20);
    SyncTaskQueue sync_task_queue(&threadpool);

    for (int i = 0; i < 15; i++) {
        int* arg = new int(i);
        sync_task_queue.addTask(&MyTaskFunc, arg);
    }

    printf("====================================wait for result===================================\n");
    sync_task_queue.wait();
}
```

编译运行：

```bash
$g++ -g sync_task_queue_test.cpp -o sync_task_queue_test -lpthread
$./sync_task_queue_test 
[MyTaskFunc]: thread[140349199148800] is working on 0
[MyTaskFunc]: thread[140349266290432] is working on 12
[MyTaskFunc]: thread[140349358610176] is working on 2
[MyTaskFunc]: thread[140349341824768] is working on 3
[MyTaskFunc]: thread[140349333432064] is working on 4
[MyTaskFunc]: thread[140349325039360] is working on 5
[MyTaskFunc]: thread[140349308253952] is working on 6
[MyTaskFunc]: thread[140349299861248] is working on 8
[MyTaskFunc]: thread[140349316646656] is working on 7
[MyTaskFunc]: thread[140349283075840] is working on 9
[MyTaskFunc]: thread[140349291468544] is working on 10
[MyTaskFunc]: thread[140349274683136] is working on 11
[MyTaskFunc]: thread[140349257897728] is working on 13
[MyTaskFunc]: thread[140349249505024] is working on 14
[MyTaskFunc]: thread[140349350217472] is working on 1
====================================wait for result===================================
Info: thread[140349232719616] exit
Info: thread[140349241112320] exit
Info: thread[140349266290432] exit
Info: thread[140349207541504] exit
Info: thread[140349299861248] exit
Info: thread[140349283075840] exit
Info: thread[140349291468544] exit
Info: thread[140349249505024] exit
Info: thread[140349274683136] exit
Info: thread[140349333432064] exit
Info: thread[140349325039360] exit
Info: thread[140349308253952] exit
Info: thread[140349224326912] exit
Info: thread[140349316646656] exit
Info: thread[140349199148800] exit
Info: thread[140349350217472] exit
Info: thread[140349257897728] exit
Info: thread[140349215934208] exit
Info: thread[140349358610176] exit
Info: thread[140349341824768] exit
```

## C++11特性的ThreadPool

传统C++线程池仅能接受特殊的Task（执行函数需要满足特殊的格式），使用C++11特性的线程池可以更好地支持任意类型参数的Task。

#### 1. 代码

threadpool.h：

> 参考自：https://github.com/progschj/ThreadPool/blob/master/ThreadPool.h
>
> 另外一种实现：https://github.com/mtrebi/thread-pool/blob/master/include/ThreadPool.h

```c++
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <utility>
#include <functional>
#include <stdexcept>

class ThreadPool {
 public:
    explicit ThreadPool(size_t);
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type>;
    ~ThreadPool();

 private:
    // need to keep track of threads so we can join them
    std::vector<std::thread> workers_;
    // the task queue
    std::queue<std::function<void()>> tasks_;

    // synchronization
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_;
};

// the constructor just launches some amount of workers
inline ThreadPool::ThreadPool(size_t threads) : stop_(false) {
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back(
            [this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex_);
                        this->condition_.wait(lock,
                            [this] { return this->stop_ || !this->tasks_.empty(); });
                        if (this->stop_ && this->tasks_.empty()) {
                            return;
                        }
                        task = std::move(this->tasks_.front());
                        this->tasks_.pop();
                    }
                    task();
                }
            });
    }
}

// add new work item to the pool
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<typename std::result_of<F(Args...)>::type> {
    using return_type = typename std::result_of<F(Args...)>::type;

    auto task = std::make_shared< std::packaged_task<return_type()>> (
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);

        // don't allow enqueueing after stopping the pool
        if (stop_) {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }

        tasks_.emplace([task](){ (*task)(); });
    }
    condition_.notify_one();
    return res;
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (std::thread &worker : workers_) {
        worker.join();
    }
}

#endif  // THREAD_POOL_H
```

test.cpp：

```c++
#include <unistd.h>
#include "threadpool.h"

void mytask(int i) {
    printf("Info: thread %ld is working on task %d\n", (u_int64_t)pthread_self(), i);
    sleep(1);
    return;
}

int main() {
    ThreadPool threadpool(20);
    for (int i = 0; i < 100; ++i) {
        threadpool.enqueue(mytask, i);
    }
    return 0;
}
```

编译运行：

```bash
$g++ -g test.cpp -o test -std=c++11 -lpthread
$./test
Info: thread 139679726323456 is working on task 1
Info: thread 139679709538048 is working on task 3
Info: thread 139679717930752 is working on task 2
Info: thread 139679734716160 is working on task 0
Info: thread 139679600432896 is working on task 15
Info: thread 139679684359936 is working on task 6
Info: thread 139679617218304 is working on task 14
Info: thread 139679634003712 is working on task 12
Info: thread 139679667574528 is working on task 8
Info: thread 139679625611008 is working on task 13
Info: thread 139679575254784 is working on task 19
Info: thread 139679659181824 is working on task 9
Info: thread 139679701145344 is working on task 4
Info: thread 139679692752640 is working on task 5
Info: thread 139679592040192 is working on task 17
Info: thread 139679583647488 is working on task 18
Info: thread 139679675967232 is working on task 7
Info: thread 139679642396416 is working on task 11
Info: thread 139679608825600 is working on task 16
Info: thread 139679650789120 is working on task 10
Info: thread 139679684359936 is working on task 21
Info: thread 139679617218304 is working on task 24
Info: thread 139679600432896 is working on task 28
Info: thread 139679709538048 is working on task 23
Info: thread 139679659181824 is working on task 30
Info: thread 139679692752640 is working on task 32
Info: thread 139679583647488 is working on task 34
Info: thread 139679608825600 is working on task 35
Info: thread 139679592040192 is working on task 33
Info: thread 139679634003712 is working on task 25
Info: thread 139679625611008 is working on task 29
Info: thread 139679726323456 is working on task 20
Info: thread 139679701145344 is working on task 31
Info: thread 139679650789120 is working on task 36
Info: thread 139679667574528 is working on task 27
Info: thread 139679575254784 is working on task 37
Info: thread 139679734716160 is working on task 26
Info: thread 139679675967232 is working on task 38
Info: thread 139679717930752 is working on task 22
Info: thread 139679642396416 is working on task 39
Info: thread 139679684359936 is working on task 40
Info: thread 139679692752640 is working on task 45
Info: thread 139679625611008 is working on task 51
Info: thread 139679583647488 is working on task 43
Info: thread 139679659181824 is working on task 44
Info: thread 139679575254784 is working on task 55
Info: thread 139679592040192 is working on task 47
Info: thread 139679617218304 is working on task 41
Info: thread 139679717930752 is working on task 57
Info: thread 139679726323456 is working on task 49
Info: thread 139679634003712 is working on task 50
Info: thread 139679650789120 is working on task 52
Info: thread 139679675967232 is working on task 59
Info: thread 139679667574528 is working on task 54
Info: thread 139679608825600 is working on task 46
Info: thread 139679734716160 is working on task 56
Info: thread 139679600432896 is working on task 48
Info: thread 139679642396416 is working on task 58
Info: thread 139679709538048 is working on task 42
Info: thread 139679701145344 is working on task 53
Info: thread 139679684359936 is working on task 60
Info: thread 139679625611008 is working on task 62
Info: thread 139679692752640 is working on task 61
Info: thread 139679583647488 is working on task 63
Info: thread 139679659181824 is working on task 64
Info: thread 139679575254784 is working on task 65
Info: thread 139679592040192 is working on task 66
Info: thread 139679617218304 is working on task 67
Info: thread 139679717930752 is working on task 68
Info: thread 139679726323456 is working on task 69
Info: thread 139679650789120 is working on task 71
Info: thread 139679634003712 is working on task 70
Info: thread 139679675967232 is working on task 72
Info: thread 139679667574528 is working on task 73
Info: thread 139679608825600 is working on task 74
Info: thread 139679734716160 is working on task 75
Info: thread 139679642396416 is working on task 77
Info: thread 139679709538048 is working on task 78
Info: thread 139679701145344 is working on task 79
Info: thread 139679600432896 is working on task 76
Info: thread 139679684359936 is working on task 80
Info: thread 139679625611008 is working on task 81
Info: thread 139679583647488 is working on task 83
Info: thread 139679692752640 is working on task 82
Info: thread 139679659181824 is working on task 84
Info: thread 139679575254784 is working on task 85
Info: thread 139679717930752 is working on task 88
Info: thread 139679617218304 is working on task 87
Info: thread 139679592040192 is working on task 86
Info: thread 139679634003712 is working on task 91
Info: thread 139679650789120 is working on task 90
Info: thread 139679667574528 is working on task 93
Info: thread 139679675967232 is working on task 92
Info: thread 139679608825600 is working on task 94
Info: thread 139679726323456 is working on task 89
Info: thread 139679734716160 is working on task 95
Info: thread 139679709538048 is working on task 96
Info: thread 139679642396416 is working on task 97
Info: thread 139679701145344 is working on task 98
Info: thread 139679600432896 is working on task 99
```

#### 2. 使用方式

###### 2.1 全局线程池 + 异步任务

创建一个ThreadPool的全局变量，将所有需要异步执行的任务丢到该线程池中即可：

```c++
#include <unistd.h>
#include "threadpool.h"

// 全局异步线程池
ThreadPool g_threadpool2(20);

int main() {
    // 执行异步任务
    g_threadpool2.enqueue(
        [] {
            sleep(1);
            printf("async task done\n");
        });
    return 0;
}
```

编译运行：

```bash
$g++ -g test.cpp -o test -std=c++11 -lpthread
$./test 
async task done
```

###### 2.2 全局线程池 + 同步任务

创建一个ThreadPool的全局变量并添加同步任务，通过`std::future`的`wait()`方法阻塞等待同步结果，也可以使用`get()`方法获取到函数返回值。

```c++
#include <unistd.h>
#include <memory>
#include "threadpool.h"

// 全局异步线程池
ThreadPool g_threadpool2(20);

int main() {
    // 创建同步任务
    auto res = g_threadpool2.enqueue(
        [] {
            sleep(1);
            printf("sync task done\n");
        });

    // 阻塞等待同步结果
    res.wait();

    return 0;
}
```

编译运行：

```bash
$g++ -g test.cpp -o test -std=c++11 -lpthread
$./test 
sync task done
```

###### 2.3 局部线程池实现并发同步

创建一个临时ThreadPool，利用其析构函数完成并发同步任务：

> 需要注意的是，这种用法已经脱离了线程池的初衷（避免处理短时间任务时创建与销毁线程的代价），它的主要用途是实现「多线程并发」，常用于并发多个IO请求并等待同步结果。

考虑这个场景：代码中仅在某种特殊场景（极少触发）下需要并发请求多个http链接，一方面我们不希望这些请求影响到进程的业务线程池，另一方面我们又不想单独为这个场景创建一个全局线程池使其大部分时间都在空跑。

2.3这种用法解决了我们「临时创建线程+执行并行任务+销毁线程」的局部并发问题，避免我们直接在用户代码处直接创建线程。

```bash
#include <unistd.h>
#include <memory>
#include "threadpool.h"


int main() {
    // 创建并发度为5的局部线程池
    std::shared_ptr<ThreadPool> threadpool = std::make_shared<ThreadPool>(5);

    // 创建30个异步任务
    for (int i = 0; i < 30; i++) {
        threadpool->enqueue(
            [i] {
                sleep(1);
                printf("Info: thread %ld is working on task %d\n", (u_int64_t)pthread_self(), i);
            });
    }

    // 阻塞直至获取同步结果
    threadpool.reset();

    return 0;
}
```

编译运行：

```bash
$g++ -g test.cpp -o test -std=c++11 -lpthread
$./test 
Info: thread 139811129124608 is working on task 4
Info: thread 139811145910016 is working on task 2
Info: thread 139811137517312 is working on task 3
Info: thread 139811162695424 is working on task 0
Info: thread 139811154302720 is working on task 1
Info: thread 139811129124608 is working on task 5
Info: thread 139811137517312 is working on task 7
Info: thread 139811145910016 is working on task 6
Info: thread 139811162695424 is working on task 8
Info: thread 139811154302720 is working on task 9
Info: thread 139811129124608 is working on task 10
Info: thread 139811137517312 is working on task 11
Info: thread 139811162695424 is working on task 13
Info: thread 139811154302720 is working on task 14
Info: thread 139811145910016 is working on task 12
Info: thread 139811129124608 is working on task 15
Info: thread 139811137517312 is working on task 18
Info: thread 139811145910016 is working on task 19
Info: thread 139811162695424 is working on task 16
Info: thread 139811154302720 is working on task 17
Info: thread 139811129124608 is working on task 21
Info: thread 139811162695424 is working on task 23
Info: thread 139811154302720 is working on task 24
Info: thread 139811145910016 is working on task 22
Info: thread 139811137517312 is working on task 20
Info: thread 139811162695424 is working on task 25
Info: thread 139811154302720 is working on task 26
Info: thread 139811129124608 is working on task 27
Info: thread 139811137517312 is working on task 29
Info: thread 139811145910016 is working on task 28
```

## Reference

[1] https://www.cnblogs.com/ailumiyana/p/10016965.html

[2] https://github.com/progschj/ThreadPool/blob/master/ThreadPool.h

[3] https://blog.csdn.net/qq_34771252/article/details/90319537

[4] https://github.com/lizhenghn123/zl_threadpool/tree/master/ThreadPoolCpp03