#pragma once
#ifdef _WIN32

#define IPC_PRIVATE 0

int shmget_win(unsigned long key, unsigned long size, int create);
int shmget_pri_win(unsigned long key, unsigned long size, int create);
void *shmat_win(int shmid);
void shmdt_win(void *pBuf);
void shmclose_win(int shmid);
unsigned int shmgetkey(int shmid);


typedef struct timeval_win {
    long tv_sec;
    long tv_usec;
} timeval;

int gettimeofday_win(struct timeval_win* tp, void* tzp);

typedef long long time_t;
time_t time_win(time_t *t);

#endif
