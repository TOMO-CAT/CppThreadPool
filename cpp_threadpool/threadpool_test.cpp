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