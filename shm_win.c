#ifdef _WIN32
#include <windows.h>
#include <winnt.h>
#include <stdio.h>
#include <conio.h>
#include <wchar.h>
#include "shm_win.h"
#include <sddl.h>

#define SHM_QUEUE_FILE L"SHM-QUEUE-DATA-"
#define SHM_QUEUE_META L"SHM-QUEUE-META-"
#define SHM_QUEUE_PRI_FILE L"SHM-QUEUE-PRI-DATA-"
#define SHM_QUEUE_PRI_META L"SHM-QUEUE-PRI-META-"


struct shmmeta_info
{
    unsigned int size;
    unsigned int reserved[63];
};

#define MAX_SHM_NUM 128

static HANDLE allHandles[MAX_SHM_NUM] = {NULL};
static HANDLE metaHandles[MAX_SHM_NUM] = {NULL};
static void *metaBuffers[MAX_SHM_NUM] = {NULL};
static unsigned int allSizes[MAX_SHM_NUM] = {0};
static unsigned int shmKeys[MAX_SHM_NUM] = {0};


static HANDLE shmget_by_name(const WCHAR *filename, unsigned long key, unsigned long size, int create)
{
    WCHAR shm_name[256];
    wsprintfW(shm_name, L"%ws%lu", filename, key);

    HANDLE hMapFile;

    if (create)
    {
        SECURITY_ATTRIBUTES sa;
        SECURITY_DESCRIPTOR sd;
        BOOL bRet = InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        if (!bRet)
        {
            wprintf(L"InitializeSecurityDescriptor failed (error=%d).\n", GetLastError());
            return NULL;
        }
        bRet = SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
        if (!bRet)
        {
            wprintf(L"SetSecurityDescriptorDacl failed (error=%d).\n", GetLastError());
            return NULL;
        }
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;

        hMapFile = CreateFileMappingW(
                    INVALID_HANDLE_VALUE,    // use paging file
                    &sa,                    // default security
                    PAGE_READWRITE,          // read/write access
                    0,                       // maximum object size (high-order DWORD)
                    size,                    // maximum object size (low-order DWORD)
                    shm_name);               // name of mapping object
        if (hMapFile == NULL)
        {
            wprintf(L"Could not create file mapping object %ws (error=%d).\n", shm_name, GetLastError());
            return NULL;
        }
    }
    else
    {
        hMapFile = OpenFileMappingW(
                        FILE_MAP_ALL_ACCESS,   // read/write access
                        FALSE,                 // do not inherit the name
                        shm_name);             // name of mapping object
        if (hMapFile == NULL)
        {
            wprintf(L"Could not open file mapping object %ws (%d).\n", shm_name, GetLastError());
            return NULL;
        }
    }
    return hMapFile;
}

void *shmat_win(int shmid)
{
    if (shmid < 0 || shmid >= MAX_SHM_NUM || allHandles[shmid] == 0)
    {
        wprintf(L"ShmID %d is invalid.\n", shmid);
        return (void*)-1;
    }
    void *pBuf = (void *) MapViewOfFile(allHandles[shmid],   // handle to map object
                        FILE_MAP_ALL_ACCESS, // read/write permission
                        0,
                        0,
                        allSizes[shmid]);

    if (pBuf == NULL)
    {
        wprintf(L"Could not map view of file (%d).\n", GetLastError());
        return (void*)-1;
    }
    wprintf(L"Attached shmid %d with size %d successfully\n", shmid, allSizes[shmid]);
    return pBuf;
}

static int shmget_ext(unsigned long key, unsigned long size, int create, BOOL isprivate)
{
    if (key == 0)
    {
        isprivate = TRUE;
        key = 0x70000000UL + (((unsigned long)time_win(NULL)) & 0xffff0000) + ((((unsigned long)rand()) & 0x0fff) << 16);
    }

    const WCHAR* metakey = isprivate ? SHM_QUEUE_PRI_META : SHM_QUEUE_META;
    const WCHAR* datakey = isprivate ? SHM_QUEUE_PRI_FILE : SHM_QUEUE_FILE;

    HANDLE h = shmget_by_name(metakey, key, sizeof(struct shmmeta_info), create);
    if (h == NULL)
        return -1;

    struct shmmeta_info *meta = (struct shmmeta_info *)MapViewOfFile(h,   // handle to map object
                        FILE_MAP_ALL_ACCESS, // read/write permission
                        0,
                        0,
                        sizeof(struct shmmeta_info));
    if (meta == NULL)
    {
        wprintf(L"Could not map view of meta shm (%d).\n", GetLastError());
        CloseHandle(h);
        return -1;
    }

    if (create) // save size
    {
        meta->size = size;
        wprintf(L"Write size of %d to meta shm\n", size);
    }
    else // open only
    {
        wprintf(L"Read size of %d from meta shm\n", meta->size);
        if (size == 0)
            size = meta->size;
        else if (size != meta->size)
        {
            wprintf(L"Error: shm size %d is different from shm meta of %d.\n", size, meta->size);
            UnmapViewOfFile(meta);
            CloseHandle(h);
            return -1;
        }
    }

    HANDLE h2 = shmget_by_name(datakey, key, size, create);
    if (h2 == NULL)
    {
        CloseHandle(h);
        UnmapViewOfFile(meta);
        return -1;
    }

    int shmid;
    for (shmid=0; shmid < MAX_SHM_NUM; shmid++)
    {
        if (InterlockedCompareExchange64(allHandles+shmid, h2, 0) == 0)
        {
            allSizes[shmid] = size;
            metaHandles[shmid] = h;
            metaBuffers[shmid] = meta;
            shmKeys[shmid] = key;
            return shmid;
        }
    }

    wprintf(L"Too many shm handles, max is %d.\n", MAX_SHM_NUM);
    CloseHandle(h2);
    UnmapViewOfFile(meta);
    CloseHandle(h);
    return -1;
}

int shmget_pri_win(unsigned long key, unsigned long size, int create)
{
    return shmget_ext(key, size, create, TRUE);
}

int shmget_win(unsigned long key, unsigned long size, int create)
{
    return shmget_ext(key, size, create, FALSE);
}

unsigned int shmgetkey(int shmid)
{
    if (shmid < 0 || shmid >= MAX_SHM_NUM)
    {
        return 0;
    }
    return shmKeys[shmid];
}

void shmdt_win(void *pBuf)
{
    UnmapViewOfFile(pBuf);
}

void shmclose_win(int shmid)
{
    if (shmid < 0 || shmid >= MAX_SHM_NUM || allHandles[shmid] == 0)
    {
        return;
    }

    if (metaBuffers[shmid])
        UnmapViewOfFile(metaBuffers[shmid]);
    if (metaHandles[shmid])
        CloseHandle(metaHandles[shmid]);
    CloseHandle(allHandles[shmid]);
    allSizes[shmid] = 0;
    allHandles[shmid] = 0;
    metaBuffers[shmid] = NULL;
    metaHandles[shmid] = NULL;
}

int gettimeofday_win(struct timeval_win* tp, void* tzp)
{
    // Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
    // This magic number is the number of 100 nanosecond intervals since January 1, 1601 (UTC)
    // until 00:00:00 January 1, 1970 
    static const unsigned long long EPOCH = ((unsigned long long)116444736000000000ULL);

    SYSTEMTIME  system_time;
    FILETIME    file_time;
    unsigned long long time;

    GetSystemTime(&system_time);
    SystemTimeToFileTime(&system_time, &file_time);
    time = ((unsigned long long)file_time.dwLowDateTime);
    time += ((unsigned long long)file_time.dwHighDateTime) << 32;

    tp->tv_sec = (long)((time - EPOCH) / 10000000L);
    tp->tv_usec = (long)(system_time.wMilliseconds * 1000);
    return 0;
}

time_t time_win(time_t* t)
{
    static const unsigned long long EPOCH = ((unsigned long long)116444736000000000ULL);

    SYSTEMTIME  system_time;
    FILETIME    file_time;
    unsigned long long    time;

    GetSystemTime(&system_time);
    SystemTimeToFileTime(&system_time, &file_time);
    time = ((unsigned long long)file_time.dwLowDateTime);
    time += ((unsigned long long)file_time.dwHighDateTime) << 32;

    time_t sec = (long)((time - EPOCH) / 10000000L);
    if (t)
        *t = sec;
    return sec;
}

#endif
