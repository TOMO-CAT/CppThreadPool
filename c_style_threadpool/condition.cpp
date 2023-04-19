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
int condition_signal(condition_t* cond) {
  return pthread_cond_signal(&cond->pcond);
}

// 唤醒所有等待线程
int condition_broadcast(condition_t* cond) {
  return pthread_cond_broadcast(&cond->pcond);
}

// 销毁
int condition_destroy(condition_t* cond) {
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
