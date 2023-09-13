all: lib test

lib: shm_queue.c shm_queue.h opt_time.h
	gcc -o libshmqueue.so shm_queue.c -shared -fPIC

test: lib test.c shm_queue.h
	gcc -o test test.c -L./ -lshmqueue -g

clean:
	rm -f test libshmqueue.so
