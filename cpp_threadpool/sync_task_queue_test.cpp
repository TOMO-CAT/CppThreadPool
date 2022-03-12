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