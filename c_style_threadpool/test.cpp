#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "threadpool.h"

#define THREADPOOL_MAX_NUM 30

void* MyTaskFunc(void* arg) {
  printf("Info: thread %ld is working on task %d\n", (u_int64_t)pthread_self(), *reinterpret_cast<int*>(arg));
  sleep(1);
  free(arg);
  return NULL;
}

int main(int argc, char* argv[]) {
  threadpool_t pool;
  threadpool_init(&pool, THREADPOOL_MAX_NUM);

  // add task to task queue
  for (int i = 0; i < 100; i++) {
    int* arg = reinterpret_cast<int*>(malloc(sizeof(int)));
    *arg = i;
    threadpool_add_task(&pool, MyTaskFunc, arg);
  }
  threadpool_destroy(&pool);
  return 0;
}
