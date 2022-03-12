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