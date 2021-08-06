/*
 * shm_queue.h
 * Declaration of a shm queue
 *
 *  Created on: 2014-5-5
 *      Author: Shaneyu <shaneyu@tencent.com>
 *
 *  Based on transaction pool, features:
 *  1) support single writer but multiple reader processes/threads
 *  2) support timestamping for each data
 *  3) support auto detecting and skipping corrupted elements
 *  4) support variable user data size
 *  5) use highly optimized gettimeofday() to speedup sys time
 */
#ifndef __SHM_QUEUE_HEADER__
#define __SHM_QUEUE_HEADER__

#ifndef BOOL
#define BOOL int
#endif

#ifndef NULL
#define NULL 0
#endif

// Switch on this macro for compiling a test program
#ifndef SQ_FOR_TEST
#define SQ_FOR_TEST	0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned short u16_t;
typedef unsigned int u32_t;
typedef unsigned long long u64_t;

// Maximum bytes allowed for a queue data
#define MAX_SQ_DATA_LENGTH	65536

// number of blocks that will be reserved to avoid write-read conflict
// if your project restricts the use of memory, you can adjust this number
// down to 1, but the probabily of write-read conflict will also be increased
#define RESERVE_BLOCK_COUNT	100

struct shm_queue;

// Create a shm queue
// Parameters:
//     shm_key      - shm key
//     ele_size     - preallocated size for each element
//     ele_count    - preallocated number of elements, this count should be greater than RESERVE_BLOCK_COUNT,
//                    and the real usable element count is (ele_count-RESERVE_BLOCK_COUNT)
//     sig_ele_num  - only send signal when data element count exceeds sig_ele_num
//     sig_proc_num - send signal to up to this number of processes each time
// Returns a shm queue pointer or NULL if failed
struct shm_queue *sq_create(u64_t shm_key, int ele_size, int ele_count, int sig_ele_num, int sig_proc_num);

// Open an existing shm queue for reading data
struct shm_queue *sq_open(u64_t shm_key);


// Register the current process ID, so that it will be able to recived event by
// polling the returned event_fd.
// Note: you don't need to unregister the current process ID, it will be removed
// automatically next time sq_get_eventfd() is called if it no longer exists
// Parameters:
//      sq  - shm_queue pointer returned by sq_open
// Returns an event fd for select/polling on success, or < 0 on failure
int sq_get_eventfd(struct shm_queue *sq);

// Once an event has been received, the user is responsible to call this
// function to reset the event counter, note that subsequent calls to select
// on sq will result in timeout for no data available
// Parameters:
//      sq  - shm_queue pointer returned by sq_open
// Returns 0 on success, or < 0 on failure
int sq_consume_event(struct shm_queue *sq);

// the same as sq_consume_event(), except that while sq_consume_event()
// consumes up to 64 events at once, the caller can specify the number of
// events to consume, this is usefull for one-poll-one-get situations
int sq_consume_event_ext(struct shm_queue *sq, int nr_events);


// Turn on/off event signaling for current process
// Parameters:
//      sq  - shm_queue pointer returned by sq_open
// Returns 0 on success, -1 if parameter is bad
int sq_sigon(struct shm_queue *sq);
int sq_sigoff(struct shm_queue *sq);

// Destroy queue created by sq_create()
void sq_destroy(struct shm_queue *queue);

// Add data to end of shm queue
// this function is multi-thread/multi-process safe
// Returns 0 on success or
//     -1 - invalid parameter
//     -2 - shm queue is full
int sq_put(struct shm_queue *queue, void *data, int datalen);

// Retrieve data
// On success, buf is filled with the first queue data
// this function is multi-thread/multi-process safe
// Returns the data length or
//      0 - no data in queue
//     -1 - invalid parameter
int sq_get(struct shm_queue *queue, void *buf, int buf_sz, struct timeval *enqueue_time);

// Get usage rate
// Returns a number from 0 to 99
int sq_get_usage(struct shm_queue *queue);

// Get number of used blocks
int sq_get_used_blocks(struct shm_queue *queue);

// If a queue operation failed, call this function to get an error reason
// Error msg for sq_create()/sq_open() can be retrieved by calling sq_errorstr(NULL)
const char *sq_errorstr(struct shm_queue *queue);

#ifdef __cplusplus
}
#endif

#endif

