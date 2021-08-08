**[中文描述]**

一个基于CAS的无锁共享内存队列的实现，使用fifo进行事件通知，消费方可以同时轮询网络socket FD和队列生成的FIFO FD，满足实时性的消费需求。

 **特点**：
 
  -   1，高性能，实测800w/s以上；
  -   2，实时通知，可用epoll侦听；
  -   3，高并发，支持同时多写多读。

 **主要功能**：
 
  -   1，高性能访问，读写可以达到解决访问内存的性能；
  -   2，高并发访问，无锁解决读写、读读、写写冲突；
  -   3，带fifo fd通知，可以select或epoll通知句柄；
  -   4，基于共享内存创建；
  -   5，大小变化时自动销毁并重新创建共享内存；
  -   6，使用分块存储策略，便于高性能访问；
  -   7，发现错误块时跳过错误的块，快速自动恢复；

**[English Description]**

This is a very fast lock-free generic data queue implemented based on share memory (shm), it is lock-free so that multiple writers and readers can access the same queue synchronously without need of locking.

**Usage example**

```C
//
// Below is a test program, please compile with:
// gcc -o sqtest -DSQ_FOR_TEST shm_queue.c
//

static char m[1024*1024];

void test_put(struct shm_queue *queue, int proc_count, int count, char *msg)
{
    int msg_len = strlen(msg);
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
            sleep(10);
            continue;
            //exit(0); // error
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
        while(put_count<record_count && sq_put(queue, m, record_size)>=0) put_count ++;
        while(sq_get(queue, m, sizeof(m), &tv)>0) get_count ++;
    }
    printf(“put %u, get %u finished\n”, put_count, get_count);
}

int main(int argc, char *argv[])
{
    struct shm_queue *queue;
    long key;

    if(argc<3)
    {
        badarg:
        printf(“usage: \n”);
        printf(“ %s open \n”, argv[0]);
        printf(“ %s create \n”, argv[0]);
        printf(“ %s press \n”, argv[0]);
        printf(“\n”);
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

```
