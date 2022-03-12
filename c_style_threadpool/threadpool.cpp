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