/*
 * shm_queue.c
 * Implementation of a shm queue
 *
 *  Created on: 2014-5-5
 *      Author: Shaneyu <shaneyu@tencent.com>
 *
 *  Based on implementation of transaction queue
 *
 *  Revision history:
 *  2014-07-05		shaneyu		Add registration/signal support
 *  2014-07-15  	shaneyu		Use fifo for data notification
 *  2014-07-21		shaneyu		Resolve multiple write conflicts
 *  2022-10-31		shaneyu		Add anonymous shm support
 */
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#include "shm_queue.h"
#if defined(__x86_64__) || defined(__x86_32__)
#include "opt_time.h"
#else
#define opt_gettimeofday gettimeofday
#define opt_time time
#endif

#define TOKEN_NO_DATA    0
#define TOKEN_SKIPPED    0xdb030000 // token to mark the node is skipped
#define TOKEN_HAS_DATA   0x0000db03 // token to mark the valid start of a node

#define SQ_MAX_READER_PROC_NUM	64 // maximum allowable processes to be signaled when data arrives
#define SQ_MAX_CONFLICT_TRIES	10 // maximum number of reading attempts for read-write conflict detection

#ifdef __x86_64__
#define CAS32(ptr, val_old, val_new)({ char ret; __asm__ __volatile__("lock; cmpxchgl %2,%0; setz %1": "+m"(*ptr), "=q"(ret): "r"(val_new),"a"(val_old): "memory"); ret;})
#define wmb() __asm__ __volatile__("sfence":::"memory")
#define rmb() __asm__ __volatile__("lfence":::"memory")
#else
#define CAS32(ptr, val_old, val_new) __sync_bool_compare_and_swap(ptr, val_old, val_new)
#define wmb() __sync_synchronize()
#define rmb() __sync_synchronize()
#endif
struct sq_head_t;

//
// time structs in 32/64 bit environments are different
// these code makes time_t/timeval 32bit compatible, so that
// the writer compiled in 32bit can comunicate with the reader
// in 64bit environment, and vice versa.
//
#define time32_t int32_t
struct timeval32
{
	time32_t tv_sec;
	time32_t tv_usec;
};

struct shm_queue
{
	struct sq_head_t *head;
	int sig_idx; // current reading process index in process array
	int poll_fd; // event fd, reading
	int poll_fdset[SQ_MAX_READER_PROC_NUM]; // fd list of registered processes, writting
	time32_t fifo_times[SQ_MAX_READER_PROC_NUM]; // fifo creation timestamps
	uint64_t shm_key;
	int rw_conflict; // read-write conflict counter
	int shm_id;
	char errmsg[256];
};

static char errmsg[256];

const char *sq_errorstr(struct shm_queue *sq)
{
	return sq? sq->errmsg : errmsg;
}

struct sq_node_head_t
{
	u32_t start_token; // 0x0000db03, if the head position is corrupted, find next start token
	u32_t datalen; // length of stored data in this node
	struct timeval32 enqueue_time;

	// the actual data are stored here
	unsigned char data[0];

} __attribute__((packed));

struct sq_head_t
{
	int ele_size;
	int ele_count;

	volatile int head_pos; // head position in the queue, pointer for reading
	volatile int tail_pos; // tail position in the queue, pointer for writting

	int sig_node_num; // send signal to processes when data node excceeds this count
	int sig_process_num; // send signal to up to this number of processes each time

	volatile int pidnum; // number of processes currently registered for signal delivery
	volatile pid_t pidset[SQ_MAX_READER_PROC_NUM]; // registered pid list
	volatile uint8_t sigmask[(SQ_MAX_READER_PROC_NUM+7)/8]; // bit map for pid waiting on signal

	volatile int siglock[SQ_MAX_READER_PROC_NUM]; // fifo write lock
	volatile int signr[SQ_MAX_READER_PROC_NUM]; // nr of writers waiting on fifo write

	uint8_t reserved[1024*1024*4]; // 4MB of reserved space

	struct sq_node_head_t nodes[0];
};

// Increase head/tail by val
#define SQ_ADD_HEAD(queue, val) 	(((queue)->head_pos+(val))%((queue)->ele_count+1))
#define SQ_ADD_TAIL(queue, val) 	(((queue)->tail_pos+(val))%((queue)->ele_count+1))

// Next position after head/tail
#define SQ_NEXT_HEAD(queue) 	SQ_ADD_HEAD(queue, 1)
#define SQ_NEXT_TAIL(queue) 	SQ_ADD_TAIL(queue, 1)

#define SQ_ADD_POS(queue, pos, val)     (((pos)+(val))%((queue)->ele_count+1))

#define SQ_IS_QUEUE_FULL(queue) 	(SQ_NEXT_TAIL(queue)==(queue)->head_pos)
#define SQ_IS_QUEUE_EMPTY(queue)	((queue)->tail_pos==(queue)->head_pos)

#define SQ_EMPTY_NODES(queue) 	(((queue)->head_pos+(queue)->ele_count-(queue)->tail_pos) % ((queue)->ele_count+1))
#define SQ_USED_NODES(queue) 	((queue)->ele_count - SQ_EMPTY_NODES(queue))

#define SQ_EMPTY_NODES2(queue, head) (((head)+(queue)->ele_count-(queue)->tail_pos) % ((queue)->ele_count+1)) 
#define SQ_USED_NODES2(queue, head) ((queue)->ele_count - SQ_EMPTY_NODES2(queue, head))

// The size of a node
#define SQ_NODE_SIZE_ELEMENT(ele_size)	(sizeof(struct sq_node_head_t)+ele_size)
#define SQ_NODE_SIZE(queue)            	(SQ_NODE_SIZE_ELEMENT((queue)->ele_size))

// Convert an index to a node_head pointer
#define SQ_GET(queue, idx) ((struct sq_node_head_t *)(((char*)(queue)->nodes) + (idx)*SQ_NODE_SIZE(queue)))

// Estimate how many nodes are needed by this length
#define SQ_NUM_NEEDED_NODES(queue, datalen) 	((datalen) + sizeof(struct sq_node_head_t) + SQ_NODE_SIZE(queue) -1) / SQ_NODE_SIZE(queue)

static inline int is_pid_valid(pid_t pid)
{
	if(pid==0) return 0;

	char piddir[256];
	snprintf(piddir, sizeof(piddir), "/proc/%u", pid);
	DIR *d = opendir(piddir);
	if(d==NULL)
		return 0;
	closedir(d);
	return 1;
}

// Turn on/off signaling for current process
// Parameters:
//      sq  - shm_queue pointer returned by sq_open
//      sigindex - returned by sq_register_signal()
// Returns 0 on success, -1 if parameter is bad
static int sq_set_sig_on(struct sq_head_t *sq, int sigindex)
{
	if((uint32_t)sigindex<(uint32_t)sq->pidnum)
	{
		__sync_fetch_and_or(sq->sigmask+(sigindex/8), (uint8_t)1<<(sigindex%8));
		return 0;
	}
	return -1;
}

static int sq_set_sig_off(struct sq_head_t *sq, int sigindex)
{
	if((uint32_t)sigindex<(uint32_t)sq->pidnum)
	{
		__sync_fetch_and_and(sq->sigmask+(sigindex/8), (uint8_t)~(1U<<(sigindex%8)));
		return 0;
	}
	return -1;
}

int sq_sigon(struct shm_queue *sq)
{
	if(sq_set_sig_on(sq->head, sq->sig_idx))
	{
		snprintf(errmsg, sizeof(errmsg), "sigindex is invalid");
		return -1;
	}
	return 0;
}

int sq_sigoff(struct shm_queue *sq)
{
	if(sq_set_sig_off(sq->head, sq->sig_idx))
	{
		snprintf(errmsg, sizeof(errmsg), "sigindex is invalid");
		return -1;
	}
	return 0;
}

int sq_get_shmid(struct shm_queue *sq)
{
	if (sq == NULL) return -1;
	return sq->shm_id;
}


static inline void verify_and_remove_bad_pids(struct sq_head_t *sq)
{
	int i;
	int oldpidnum = (int)sq->pidnum;
	int newpidnum = oldpidnum;
	// test and remove invalid pids so that they won't be signaled
	if(newpidnum<0 || newpidnum>SQ_MAX_READER_PROC_NUM)
	{
		newpidnum = SQ_MAX_READER_PROC_NUM;
		if(!CAS32(&sq->pidnum, oldpidnum, newpidnum))
			return;
	}
	for(i=newpidnum-1; i>=0 && !is_pid_valid((pid_t)sq->pidset[i]); i--)
	{
		sq_set_sig_off(sq, i);
		if(!CAS32(&sq->pidnum, i+1, i)) // conflict detected
			break;
	}
	for(i--; i>=0; i--)
	{
		pid_t oldpid = (pid_t)sq->pidset[i];
		if(!is_pid_valid(oldpid))
		{
			sq_set_sig_off(sq, i);
			CAS32(&sq->pidset[i], oldpid, 0); // if conflict occurs, simply ignore it
		}
	}
}


static int create_fifo(uint64_t shm_key, int idx, BOOL is_reading)
{
	char fifo[256];
	snprintf(fifo, sizeof(fifo), "/tmp/shmqueue_fifo_0x%llX_%d", (unsigned long long)shm_key, idx);
	int ret = mkfifo(fifo, 0666);
	if(ret)
	{
		if(errno!=EEXIST)
		{
			perror("mkfifo");
			return -1;
		}
	}
	// In order to avoid reader process always receiving EOF on select(),
	// we need to set open mode to O_RDWR instead of O_RDONLY,
	// please see http://stackoverflow.com/questions/14594508/fifo-pipe-is-always-readable-in-select
	// Thanks Leonxing for pointing out this issue!
	ret = open(fifo, (is_reading? O_RDWR : O_WRONLY) | O_NONBLOCK, 0666);
	if(ret==-1)
	{
		perror("open fifo");
		return -2;
	}

	return ret;
}

// Register the current process ID, so that it will be able to receive signal
// Note: you don't need to unregister the current process ID, it will be removed
// automatically next time register_signal is called if it no longer exists
// Parameters:
//      sq  - shm_queue pointer returned by sq_open
// Returns a signal index for sq_sigon/sq_sigoff, or < 0 on failure
int sq_get_eventfd(struct shm_queue *queue)
{
	if(queue->sig_idx>=0 && queue->poll_fd>0)
		return queue->poll_fd;

	int sigidx = -1;
	struct sq_head_t *sq = queue->head;
	pid_t pid = getpid();
	verify_and_remove_bad_pids(sq);

	int i;
	for(i=0; i<sq->pidnum; i++)
	{
		if(sq->pidset[i]==pid)
		{
			sigidx = i;
			goto ret;
		}
	}

	for(i=0; i<sq->pidnum; i++)
	{
		if(!sq->pidset[i])
		{
			// if i is taken by someone else, try next
			// else set pidset[i] to our pid and return i
			if(CAS32(&sq->pidset[i], 0, pid))
			{
				sigidx = i;
				goto ret;
			}
		}
	}

	while(1) // CAS loop
	{
		int pidnum = (int)sq->pidnum;
		if(pidnum>=SQ_MAX_READER_PROC_NUM)
		{
			snprintf(queue->errmsg, sizeof(queue->errmsg),
				"pid num exceeds maximum of %u", SQ_MAX_READER_PROC_NUM);
			return -1;
		}
		int oldpid = sq->pidset[pidnum];
		if(CAS32(&sq->pidnum, pidnum, pidnum+1) && CAS32(&sq->pidset[pidnum], oldpid, pid))
		{
			sigidx = pidnum;
			break;
		}
	}
ret:
	queue->sig_idx = sigidx;
	queue->poll_fd = create_fifo(queue->shm_key, sigidx, 1);
	if(queue->poll_fd<0)
	{
		snprintf(queue->errmsg, sizeof(queue->errmsg),
			"%s fifo failed: %s",
			queue->poll_fd==-1? "create":"open",
			strerror(errno));
		queue->sig_idx = -1;
		queue->poll_fd = 0;
		return -1;
	}
	return queue->poll_fd;
}

int sq_consume_event(struct shm_queue *sq)
{
	return sq_consume_event_ext(sq, 0); // default nr_events
}

int sq_consume_event_ext(struct shm_queue *sq, int nr_events)
{
	if(nr_events<=0)
		nr_events = 64;
	else if(nr_events>1024)
		nr_events = 1024;

	if(sq->poll_fd>0)
	{
		char c[nr_events];
		read(sq->poll_fd, c, nr_events);
		return 0;
	}
	snprintf(sq->errmsg, sizeof(sq->errmsg), "bad poll fd");
	return -1;
}

// shm operation wrapper
static char *attach_shm(long iKey, long iSize, int *bCreate, int *pShmId)
{
	int shmid = 0, creating = *bCreate, created = 0;
	char* shm;

	// If *pShmId is valid, use it
	if(pShmId && *pShmId > 0)
		shmid = *pShmId;

	if(shmid==0 && (shmid=shmget(iKey, 0, 0)) < 0)
	{
		printf("shmget(key=%ld, size=%ld): %s\n", iKey, iSize, strerror(errno));
		if(!creating || (shmid=shmget(iKey, iSize, 0666|IPC_CREAT)) < 0)
		{
			return NULL;
		}
		created = 1;
	}
	else if(creating)
	{
		// verify existing size
		struct shmid_ds ds;
		if(shmctl(shmid, IPC_STAT, &ds) < 0)
		{
			printf("shmctl(key=%ld): %s\n", iKey, strerror(errno));
			return NULL;
		}
		if(ds.shm_segsz != iSize)
		{
			printf("shm key=%ld size mismatched(existing %lu, creating %ld), remove and try again\n", iKey, (unsigned long)ds.shm_segsz, iSize);
			if(shmctl(shmid, IPC_RMID, NULL))
			{
				perror("shm rm");
				return NULL;
			}
			shmid = shmget(iKey, iSize, 0666|IPC_CREAT);
			if(shmid<0)
			{
				perror("re-shmget");
				return NULL;
			}
			created = 1;
		}
	}

	if((shm=shmat(shmid, NULL ,0))==(char *)-1)
	{
		perror("shmat");
		return NULL;
	}

	if (pShmId && shmid != *pShmId)
		*pShmId = shmid;
	*bCreate = created;

/*
	// avoid swapping, need root privillege
	if(mlock(shm, iSize)<0)
	{
		perror("mlock");
		shmdt(shm);
		return NULL;
	}
*/
	return shm;
}

// shm operation wrapper
static struct sq_head_t *open_shm_queue(long shm_key, long ele_size, long ele_count, int create, int *shm_id)
{
	long allocate_size;
	struct sq_head_t *shm;

	if(create)
	{
		ele_size = (((ele_size + 7)>>3) << 3); // align to 8 bytes
		// We need an extra element for ending control
		allocate_size = sizeof(struct sq_head_t) + SQ_NODE_SIZE_ELEMENT(ele_size)*(ele_count+1);
		// Align to 4MB boundary
		allocate_size = (allocate_size + (4UL<<20) - 1) & (~((4UL<<20)-1));
		printf("shm size needed for queue - %lu.\n", allocate_size);
	}
	else
	{
		allocate_size = 0;
	}

	int created = create;
	if (!(shm = (struct sq_head_t *)attach_shm(shm_key, allocate_size, &created, shm_id)))
	{
		return NULL;
	}

	if(created)
	{
		memset(shm, 0, allocate_size);
		shm->ele_size = ele_size;
		shm->ele_count = ele_count;
	}
	else if(create) // verify parameters if open for writing
	{
		if(shm->ele_size!=ele_size || shm->ele_count!=ele_count)
		{
			printf("shm parameters mismatched: \n");
			printf("    given:  ele_size=%ld, ele_count=%ld\n", ele_size, ele_count);
			printf("    in shm: ele_size=%d, ele_count=%d\n", shm->ele_size, shm->ele_count);
			shmdt(shm);
			return NULL;
		}
	}

	return shm;
}

static int signal_process(struct shm_queue *sq, int sigidx);

// Set signal parameters to enable signaling on data write
// Parameters:
//      sq           - shm_queue pointer
//      sig_ele_num  - only send signal when data element count exceeds sig_ele_num
//      sig_proc_num - send signal to up to this number of processes once
// Returns 0 on success, < 0 on failure
static int sq_set_sigparam(struct shm_queue *queue, int sig_ele_num, int sig_proc_num)
{
	struct sq_head_t *sq = queue->head;
	sq->sig_node_num = sig_ele_num;
	sq->sig_process_num = sig_proc_num;
	verify_and_remove_bad_pids(sq);

	if(sq->pidnum>0) // print the registered pids
	{
		int i;
		printf("Registered pids: ");
		for(i=0; i<sq->pidnum; i++)
		{
			if(i) printf(", ");
			printf("%u", (uint32_t)sq->pidset[i]);
			// when the writer process terminates, the fifo reader
			// will keep receiving fifo_closed event in polling, but a read will return no data
			// to avoid the reader from constant wakening from poll, the writer needs to write some data
			// to the fifo
			signal_process(queue, i);
		}
	}

	return 0;
}

#define SQ_LOCK_FILE	"/tmp/.shm_queue_lock"

static int exc_lock(int iUnlocking, int *fd, u64_t shm_key)
{
	char sLockFile[256];
	snprintf(sLockFile, sizeof(sLockFile), "%s_%llu", SQ_LOCK_FILE, (unsigned long long)shm_key);

	if(*fd <= 0)
		*fd = open(sLockFile, O_CREAT, 0666);
	if(*fd < 0)
	{
		printf("open lock file %s failed: %s\n", SQ_LOCK_FILE, strerror(errno));
		return -1;
	}

	int ret = flock(*fd, iUnlocking? LOCK_UN:LOCK_EX);
	if(ret < 0)
	{
		printf("%s file %s failed: %s\n", iUnlocking? "Unlock":"Lock", SQ_LOCK_FILE, strerror(errno));
		return -2;
	}
	return 0;
}


// Create a shm queue
// Parameters:
//     shm_key      - shm key, may be IPC_PRIVATE
//     ele_size     - preallocated size for each element
//     ele_count    - preallocated number of elements
//     sig_ele_num  - only send signal when data element count exceeds sig_ele_num
//     sig_proc_num - send signal to up to this number of processes each time
// Returns a shm queue pointer or NULL if failed
struct shm_queue *sq_create(u64_t shm_key, int ele_size, int ele_count, int sig_ele_num, int sig_proc_num)
{
	int fd = -1;
	signal(SIGPIPE, SIG_IGN);

	exc_lock(0, &fd, shm_key); // lock, if failed, printf and ignore

	struct shm_queue *queue = calloc(1, sizeof(struct shm_queue));
	if(queue==NULL)
	{
		snprintf(errmsg, sizeof(errmsg), "Out of memory");
		exc_lock(1, &fd, shm_key); // ulock
		return NULL;
	}

	if(ele_size<=0 || ele_count<=RESERVE_BLOCK_COUNT || shm_key<0) // invalid parameter
	{
		free(queue);
		if(ele_count<=RESERVE_BLOCK_COUNT)
			snprintf(errmsg, sizeof(errmsg), "Bad argument: ele_count(%d) should be greater than RESERVE_BLOCK_COUNT(%d)", ele_count, RESERVE_BLOCK_COUNT);
		else
			snprintf(errmsg, sizeof(errmsg), "Bad argument");
		exc_lock(1, &fd, shm_key); // ulock
		return NULL;
	}

	queue->shm_key = shm_key;
	queue->shm_id = 0;
	queue->head = open_shm_queue(shm_key, ele_size, ele_count, 1, &queue->shm_id);
	if(queue->head==NULL)
	{
		free(queue);
		snprintf(errmsg, sizeof(errmsg), "Get shm failed");
		exc_lock(1, &fd, shm_key); // ulock
		return NULL;
	}
	sq_set_sigparam(queue, sig_ele_num, sig_proc_num);

	exc_lock(1, &fd, shm_key); // ulock
	return queue;
}

// Open an existing shm queue for reading data
struct shm_queue *sq_open(u64_t shm_key)
{
	signal(SIGPIPE, SIG_IGN);

	struct shm_queue *queue = calloc(1, sizeof(struct shm_queue));
	if(queue==NULL)
	{
		snprintf(errmsg, sizeof(errmsg), "Out of memory");
		return NULL;
	}
	queue->shm_key = shm_key;
	queue->sig_idx = -1;
	queue->shm_id = 0;
	queue->head = open_shm_queue(shm_key, 0, 0, 0, &queue->shm_id);
	if(queue->head==NULL)
	{
		free(queue);
		snprintf(errmsg, sizeof(errmsg), "Open shm failed");
		return NULL;
	}
	return queue;
}

struct shm_queue *sq_open_by_shmid(int shm_id)
{
	signal(SIGPIPE, SIG_IGN);

	struct shm_queue *queue = calloc(1, sizeof(struct shm_queue));
	if(queue==NULL)
	{
		snprintf(errmsg, sizeof(errmsg), "Out of memory");
		return NULL;
	}
	queue->shm_key = 0;
	queue->sig_idx = -1;
	queue->shm_id = shm_id;
	queue->head = open_shm_queue(0, 0, 0, 0, &queue->shm_id);
	if(queue->head==NULL)
	{
		free(queue);
		snprintf(errmsg, sizeof(errmsg), "Open shm failed");
		return NULL;
	}
	return queue;
}


// Destroy shm_queue created by sq_create()
void sq_destroy(struct shm_queue *queue)
{
	shmdt(queue->head);
	free(queue);
}

// Destroy shm_queue and remove shm
void sq_destroy_and_remove(struct shm_queue *queue)
{
	shmdt(queue->head);
	shmctl(queue->shm_id, IPC_RMID, 0);
	free(queue);
}


static int signal_process(struct shm_queue *sq, int sigidx)
{
	if(sq->poll_fdset[sigidx]<=0)
	{
		// avoid constant fifo creation, in case create_fifo() fails every time
		time_t t = opt_time(NULL);
		if(sq->fifo_times[sigidx]==0 || sq->fifo_times[sigidx]+60<=t)
		{
			sq->fifo_times[sigidx] = t;
			sq->poll_fdset[sigidx] = create_fifo(sq->shm_key, sigidx, 0);
		}
	}

	if(sq->poll_fdset[sigidx]>0)
	{
		if(CAS32(&sq->head->siglock[sigidx], 0, 1) || // we are the only writer
			sq->head->signr[sigidx]>10) // deadlock detection: too many writers waiting, overwrite
		{
			char c[1];
			sq->head->signr[sigidx] = 0;
			write(sq->poll_fdset[sigidx], c, sizeof(c));
			sq->head->siglock[sigidx] = 0; // unlock
		}
		else // contest for writting failed
		{
			(void)__sync_fetch_and_add(&sq->head->signr[sigidx], 1);
		}

		return 0;
	}
	return -1;
}


// Add data to end of shm queue
// Returns 0 on success or
//     -1 - invalid parameter
//     -2 - shm queue is full
int sq_put(struct shm_queue *sq, void *data, int datalen)
{
	u32_t idx;
	struct sq_node_head_t *node;
	int nr_nodes;
	int old_tail, new_tail;
	struct sq_head_t *queue = sq->head;

	if(queue==NULL || data==NULL || datalen<=0 || datalen>MAX_SQ_DATA_LENGTH)
	{
		snprintf(sq->errmsg, sizeof(sq->errmsg), "Bad argument");
		return -1;
	}


	while(1)
	{
		rmb(); // sync read
		old_tail = queue->tail_pos;

		// calculate the number of nodes needed
		nr_nodes = SQ_NUM_NEEDED_NODES(queue, datalen);
	
		if(SQ_EMPTY_NODES(queue)<nr_nodes+RESERVE_BLOCK_COUNT)
		{
			snprintf(sq->errmsg, sizeof(sq->errmsg), "Not enough for new data");
			return -2;
		}
	
		idx = old_tail;
		node = SQ_GET(queue, idx);
		new_tail = SQ_ADD_TAIL(queue, nr_nodes);
	
		if(new_tail < old_tail) // wrapped back
		{
			// We need a set of continuous nodes
			// So skip the empty nodes at the end, and begin allocation at index 0
			idx = 0;
			new_tail = nr_nodes;
			node = SQ_GET(queue, 0);
	
			if(queue->head_pos-1 < nr_nodes)
			{
				snprintf(sq->errmsg, sizeof(sq->errmsg), "Not enough for new data");
				return -2; // not enough empty nodes
			}
		}

		if(!CAS32(&queue->tail_pos, old_tail, new_tail)) // CAS contest fail, try again
			continue;

		if(idx==0 && old_tail) // it's been wrapped around
		{
			// mark all the skipped blocks as being skipped
			// so that the reader process can identify whether it is
			// skipped or is being written
			struct sq_node_head_t *n;
			do
			{
				n = SQ_GET(queue, old_tail);
				n->start_token = TOKEN_SKIPPED;
				old_tail = SQ_ADD_POS(queue, old_tail, 1);
			}
			while(old_tail);
		}

		// initialize the new node
		node->datalen = datalen;
		struct timeval tv;
		opt_gettimeofday(&tv, NULL);
		node->enqueue_time.tv_sec = tv.tv_sec;
		node->enqueue_time.tv_usec = tv.tv_usec;
		memcpy(node->data, data, datalen);
		node->start_token = TOKEN_HAS_DATA; // mark data ready for reading
		wmb(); // sync write with other processors
		break;
	}

//	printf("sig_node_num=%d, used_nodes=%d, sig_process_num=%d\n", queue->sig_node_num, SQ_USED_NODES(queue), queue->sig_process_num);
	// now signal the reader wait on queue
	if(queue->sig_node_num && SQ_USED_NODES(queue)>=queue->sig_node_num) // element num reached
	{
		int i, nr;
		// signal at most queue->sig_process_num processes
		for(i=0,nr=0; i<(int)queue->pidnum && nr<queue->sig_process_num; i++)
		{
			if(queue->pidset[i] && queue->sigmask[i/8] & 1<<(i%8))
			{
				signal_process(sq, i);
				nr ++;
				sq_set_sig_off(queue, i); // avoids being signaled again
			}
		}
	}
	return 0;
}

int sq_get_usage(struct shm_queue *sq)
{
	if(sq==NULL || sq->head==NULL) return 0;
	struct sq_head_t *queue = sq->head;
	return queue->ele_count? ((SQ_USED_NODES(queue))*100)/queue->ele_count : 0;
}

int sq_get_used_blocks(struct shm_queue *sq)
{
	if(sq==NULL || sq->head==NULL) return 0;
	struct sq_head_t *queue = sq->head;
	return SQ_USED_NODES(queue);
}

// Retrieve data
// On success, buf is filled with the first queue data
// Returns the data length or
//     0  - no data in queue
//     -1 - invalid parameter
int sq_get(struct shm_queue *sq, void *buf, int buf_sz, struct timeval *enqueue_time)
{
	struct sq_node_head_t *node;

	int nr_nodes, datalen;
	int old_head, new_head, head;
	struct sq_head_t *queue = sq->head;

	if(queue==NULL || buf==NULL || buf_sz<1)
	{
		snprintf(sq->errmsg, sizeof(sq->errmsg), "Bad argument");
		return -1;
	}

	rmb();
	head = old_head = queue->head_pos;
	do
	{
		if(queue->tail_pos==head) // end of queue
		{
			if(head!=old_head && CAS32(&queue->head_pos, old_head, head))
			{
				wmb();
				new_head = head;
				datalen = 0;
				break;
			}
			// head_pos not advanced or changed by someone else, simply returns
			sq->rw_conflict = 0;
			return 0;
		}

		node = SQ_GET(queue, head);
		if(node->start_token!=TOKEN_HAS_DATA) // read-write conflict or corruption of data
		{
			// if read-write conflict happens, we (the reader) will
			// try at most SQ_MAX_CONFLICT_TRIES times for the
			// writer to finish, and if the writer is unable to
			// finish in SQ_MAX_CONFLICT_TRIES consecutive reads,
			// we will treat it as node corruption
			if(node->start_token==TOKEN_NO_DATA && sq->rw_conflict<SQ_MAX_CONFLICT_TRIES)
			{
				// Attension:
				// this node may have been read by some other process,
				// if so, the header position should have been updated
				rmb();
				if(old_head!=queue->head_pos)
				{
					// read by others, start all over again
					head = old_head = queue->head_pos;
					continue;
				}
				sq->rw_conflict++;
				return 0; // returns no data
			}
			// check start_token once again in case the writer may have already finished writting for now
			// in this case, we should not deem it corrupted
			// special thanks to jiffychen for pointing out this situation.
			rmb();
			if(node->start_token!=TOKEN_HAS_DATA)
			{
				// treat it as data corruption and skip this corrupted node
				head = SQ_ADD_POS(queue, head, 1);
				continue;
			}
		}
		datalen = node->datalen;
		nr_nodes = SQ_NUM_NEEDED_NODES(queue, datalen);
		if(SQ_USED_NODES2(queue, head) < nr_nodes)
		{
			head = SQ_ADD_POS(queue, head, 1);
			continue;
		}
		new_head = SQ_ADD_POS(queue, head, nr_nodes);
		if(CAS32(&queue->head_pos, old_head, new_head))
		{
			wmb();
			if(enqueue_time)
			{
				enqueue_time->tv_sec = node->enqueue_time.tv_sec;
				enqueue_time->tv_usec = node->enqueue_time.tv_usec;
			}
			if(datalen > buf_sz)
			{
				snprintf(sq->errmsg, sizeof(sq->errmsg), "Data length(%u) exceeds supplied buffer size of %u", datalen, buf_sz);
				return -2;
			}
			memcpy(buf, node->data, datalen);
			break;
		}
		else // head_pos changed by someone else, start over
		{
			old_head = queue->head_pos;
			head = old_head;
		}
	} while(1);

	while(old_head!=new_head)
	{
		node = SQ_GET(queue, old_head);
		// reset start_token so that this node will not be treated as a starting node of data
		node->start_token = 0;
		old_head = SQ_ADD_POS(queue, old_head, 1);
	}

	wmb();
	sq->rw_conflict = 0;
	return datalen;
}

#if SQ_FOR_TEST

#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>

//
// Below is a test program, please compile with:
// gcc -o sqtest -DSQ_FOR_TEST shm_queue.c
//

static char m[1024*1024];

void test_put(struct shm_queue *queue, int proc_count, int count, char *msg)
{
	int i;
	int pid = 0;

	for(i=1; i<=proc_count; i++)
	{
		if(fork()==0)
		{
			pid=i;
			break;
		}
	}

	for(i=0; pid && i<count; i++)
	{
		sprintf(m, "[%d:%d] %s", pid, i, msg);
		if(sq_put(queue, m, strlen(m))<0)
		{
			printf("put msg[%d] failed: %s\n", i, sq_errorstr(queue));
			return;
		}
	}
	if(pid) exit(0);
	while(wait(NULL)>0);
	printf("put successfully\n");
}

void test_get(struct shm_queue *queue, int proc_count, int count)
{
	int i;
	int pid = 0;

	for(i=1; i<=proc_count; i++)
	{
		if(fork()==0)
		{
			pid=i;
			break;
		}
	}

	if(pid)
	{
		// Note: each process has a uniq event fd, which is bond to process id
		// If your server forks several processes, this function should be called after fork()
		int event_fd = sq_get_eventfd(queue);
		printf("child %d event_fd=%d\n", pid, event_fd);

		for(i=0; i<count; i++)
		{
			fd_set fdset;
			FD_ZERO(&fdset);
			FD_SET(event_fd, &fdset);
			struct timeval to;
			to.tv_sec = 100;
			to.tv_usec = 0;

			sq_sigon(queue); // now we are entering into sleeping sys call
			int ret = select(event_fd+1, &fdset, NULL, NULL, &to);
			sq_sigoff(queue); // no longer needs signal
			if(ret<0)
			{
				printf("select failed: %s\n", strerror(errno));
				continue;
			}

			if(FD_ISSET(event_fd, &fdset))
				sq_consume_event(queue);

			struct timeval tv;
			int l = sq_get(queue, m, sizeof(m), &tv);
			if(l<0)
			{
				printf("sq_get failed: %s\n", sq_errorstr(queue));
				break;
			}
			if(l==0)// no data
			{
				i --;
				continue;
			}
			// if we are able to retrieve data from queue, always
			// try it without sleeping
			m[l] = 0;
			printf("pid[%d] msg[%d] len[%d]: %s\n", pid, i, l, m);
		}
		exit(0);
	}
	while(wait(NULL)>0);
}

void press_test(struct shm_queue *queue, uint32_t record_count, uint32_t record_size)
{
	struct timeval tv;
	int put_count=0,get_count=0;
	for(; put_count<record_count; )
	{
		while(put_count<record_count && sq_put(queue, m, record_size)==0) put_count ++;
		while(sq_get(queue, m, sizeof(m), &tv)>0) get_count ++;
	}
	printf("put %u, get %u finished\n", put_count, get_count);
}

int main(int argc, char *argv[])
{
	struct shm_queue *queue;
	long key;

	if(argc<3)
	{
badarg:
		printf("usage: \n");
		printf("     %s open <key>\n", argv[0]);
		printf("     %s create <key> <element_size> <element_count>\n", argv[0]);
		printf("     %s press <key> <record_count> <record_size>\n", argv[0]);
		printf("\n");
		return -1;
	}

	if(strncasecmp(argv[2], "0x", 2)==0)
		key = strtoul(argv[2]+2, NULL, 16);
	else
		key = strtoul(argv[2], NULL, 10); 

	if(strcmp(argv[1], "open")==0 || strcmp(argv[1], "press")==0)
	{
		queue = sq_open(key);
	}
	else if(strcmp(argv[1], "create")==0 && argc==5)
	{
		queue = sq_create(key, strtoul(argv[3], NULL, 10), strtoul(argv[4], NULL, 10), 1, 2);
	}
	else
	{
		goto badarg;
	}

	if(queue==NULL)
	{
		printf("failed to open shm queue: %s\n", sq_errorstr(NULL));
		return -1;
	}

	if(strcmp(argv[1], "press")==0)
	{
		if(argc!=5) goto badarg;
		press_test(queue, strtoul(argv[3], NULL, 10), strtoul(argv[4], NULL, 10));
		return 0;
	}

	while(1)
	{
		static char cmd[1024*1024];
		printf("available commands: \n");
		printf("  put <concurrent_proc_count> <msg_count> <msg>\n");
		printf("  get <concurrent_proc_count> <msg_count>\n");
		printf("  quit\n");
		printf("cmd>"); fflush(stdout);
		if(gets(cmd)==NULL)
			return 0;
		if(strncmp(cmd, "put ", 4)==0)
		{
			char *pstr = cmd + 4;
			while(isspace(*pstr)) pstr ++;
			int proc_count = atoi(pstr);
			if(proc_count<1) proc_count = 1;
			while(isdigit(*pstr)) pstr ++;
			while(isspace(*pstr)) pstr ++;
			int count = atoi(pstr);
			if(count<1) count = 1;
			while(isdigit(*pstr)) pstr ++;
			while(isspace(*pstr)) pstr ++;
			test_put(queue, proc_count, count, pstr);
		}
		else if(strncmp(cmd, "get ", 4)==0)
		{
			char *pstr = cmd + 4;
			while(isspace(*pstr)) pstr ++;
			int proc_count = atoi(pstr);
			if(proc_count<1) proc_count = 1;
			while(isdigit(*pstr)) pstr ++;
			while(isspace(*pstr)) pstr ++;
			int count = atoi(pstr);
			if(count<1) count = 1;
			test_get(queue, proc_count, count);
		}
		else if(strncmp(cmd, "quit", 4)==0 || strncmp(cmd, "exit", 4)==0)
		{
			return 0;
		}
	}
	return 0;
}

#endif
