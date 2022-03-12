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