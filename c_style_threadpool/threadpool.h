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