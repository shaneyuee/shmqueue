
sqtest: shm_queue.h shm_queue.c opt_time.h
	gcc -o sqtest shm_queue.c -DSQ_FOR_TEST -g
