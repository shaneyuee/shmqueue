package shmqueue

import "unsafe"

//#cgo LDFLAGS: -L./ -lshmqueue
//#include <sys/time.h>
//#include "shm_queue.h"
import "C"

// 
// Create a shm queue
// Parameters:
//     shm_key      - shm key, may be IPC_PRIVATE for anonymous shm
//     ele_size     - preallocated size for each element
//     ele_count    - preallocated number of elements, this count should be greater than RESERVE_BLOCK_COUNT,
//                    and the real usable element count is (ele_count-RESERVE_BLOCK_COUNT)
//     sig_ele_num  - only send signal when data element count exceeds sig_ele_num
//     sig_proc_num - send signal to up to this number of processes each time
// Returns a shm queue pointer or NULL if failed, on failure, call sq_errorstr(NULL) to retrieve the reason.
// struct shm_queue *sq_create(u64_t shm_key, int ele_size, int ele_count, int sig_ele_num, int sig_proc_num);

func SQCreate(key uint64, eleSize int, eleCount int) unsafe.Pointer {
	result := unsafe.Pointer(C.sq_create(C.ulonglong(key), C.int(eleSize), C.int(eleCount), C.int(0), C.int(0)))
	return result
}

// Open an existing shm queue for reading data
// struct shm_queue *sq_open(u64_t shm_key);

func SQOpen(key uint64) unsafe.Pointer {
	result := unsafe.Pointer(C.sq_open(C.ulonglong(key)))
	return result
}

// For anonymous shm, two processes can communicate throught shm_id,
// please follow these steps:
//   1) One process (A) creates a queue by sq_create() with key=IPC_PRIVATE
//   2) A gets shm_id by sq_get_shmid()
//   3) A transfers shm_id to another process (B)
//   4) B opens the shm_queue by sq_open_by_shmid()
//   5) Now A and B can communicate by each shm_queue pointer
// int sq_get_shmid(struct shm_queue *sq);
// struct shm_queue *sq_open_by_shmid(int shm_id);

func SQGetShmID(queue unsafe.Pointer) int {
	return int(C.sq_get_shmid((*C.struct_shm_queue)(queue)))
}

func SQOpenByShmID(shmID int) unsafe.Pointer {
	result := unsafe.Pointer(C.sq_open_by_shmid(C.int(shmID)))
	return result
}

// Destroy queue created by sq_create(), data in shm is left untouched
// void sq_destroy(struct shm_queue *queue);

func SQDestroy(queue unsafe.Pointer) {
	C.sq_destroy((*C.struct_shm_queue)(queue))
}

// Destroy shm_queue and remove shm
// void sq_destroy_and_remove(struct shm_queue *queue);

func SQDestroyAndRemove(queue unsafe.Pointer) {
	C.sq_destroy_and_remove((*C.struct_shm_queue)(queue))
}

// Add data to end of shm queue
// this function is multi-thread/multi-process safe
// Returns 0 on success or
//     -1 - invalid parameter
//     -2 - shm queue is full
// int sq_put(struct shm_queue *queue, void *data, int datalen);

func SQPut(queue unsafe.Pointer, buf []byte) int {
	cBuf := unsafe.Pointer(&buf[0])
	cLen := C.int(len(buf))
	return int(C.sq_put((*C.struct_shm_queue)(queue), cBuf, cLen))
}

// Retrieve data
// On success, buf is filled with the first queue data
// this function is multi-thread/multi-process safe
// Returns the data length or
//      0 - no data in queue
//     -1 - invalid parameter
// int sq_get(struct shm_queue *queue, void *buf, int buf_sz, struct timeval *enqueue_time);

// enqueueTime returns the timestamp in milliseconds
func SQGet(queue unsafe.Pointer, buf []byte) (result int, enqueueTime uint64) {
	cBuf := unsafe.Pointer(&buf[0])
	cLen := C.int(len(buf))
	var eqTime C.struct_timeval
	result = int(C.sq_get((*C.struct_shm_queue)(queue), cBuf, cLen, (*C.struct_timeval)(unsafe.Pointer(&eqTime))))
	enqueueTime = (uint64(eqTime.tv_sec) * 1000) + (uint64(eqTime.tv_usec) / 1000)
	return
}

// Get usage rate
// Returns a number from 0 to 99
// int sq_get_usage(struct shm_queue *queue);

func SQGetUsage(queue unsafe.Pointer) int {
	return int(C.sq_get_usage((*C.struct_shm_queue)(queue)))
}

// If a queue operation failed, call this function to get an error reason
// Error msg for sq_create()/sq_open() can be retrieved by calling sq_errorstr(NULL)
// const char *sq_errorstr(struct shm_queue *queue);

func SQErrorStr(queue unsafe.Pointer) string {
	return C.GoString(C.sq_errorstr((*C.struct_shm_queue)(queue)))
}

