build:
	mkdir -p output
	g++ -g threadpool_test.cpp -o output/threadpool_test -lpthread
	g++ -g sync_task_queue_test.cpp -o output/sync_task_queue_test -lpthread

run1:
	./output/threadpool_test

run2:
	./output/sync_task_queue_test

clean:
	rm -rf ./output
