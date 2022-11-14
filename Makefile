
sqtest: shm_queue.h test.c shm_queue.c opt_time.h
	gcc -o sqtest test.c shm_queue.c -g
