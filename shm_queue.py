import os
import ctypes
import sys

TEST_MMAP = False

if TEST_MMAP:
    CODE_PATH = os.path.dirname(os.path.abspath(__file__))
    CODE_PARENT_PATH = os.path.dirname(CODE_PATH)
    CODE_PARENT_PATH = os.path.dirname(CODE_PARENT_PATH)
    CODE_PARENT_PATH = os.path.dirname(CODE_PARENT_PATH)
    ROOT_PATH = os.path.dirname(CODE_PARENT_PATH)

    sys.path.append(ROOT_PATH)
    sys.path.append(f"{ROOT_PATH}/DINet")

from DINet.utils.logger import custom_logger as LOG
import DINet.utils.res_util as res_util

if TEST_MMAP:
    LOG.enable_console_logging()

#封装C的共享内存队列， libshmqueue.so

class ShmQueue:
    
    def __init__(self):
        lib_path = os.path.join(res_util.get_asset_dir(), "shm", "libshmqueue.so")
        self.libshm = ctypes.CDLL(lib_path)
        self.shm_key = 0 # 0是匿名
        self.shm_id = 0
      
    #创建, 通过shm_key
    def create(self, shm_key, ele_size, ele_count, sig_ele_num, sig_proc_num):
        self.shm_key = shm_key
        sq_create = self.libshm.sq_create
        shm_queue_ptr = ctypes.POINTER(shm_queue)
        sq_create.argtypes = [ctypes.c_ulonglong, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        sq_create.restype = shm_queue_ptr
        queue_ptr = sq_create(ctypes.c_ulonglong(shm_key), ctypes.c_int(ele_size), ctypes.c_int(ele_count), ctypes.c_int(sig_ele_num), ctypes.c_int(sig_proc_num))
        return queue_ptr
    
    # 获取shmid
    # int sq_get_shmid(struct shm_queue *sq);
    def get_shmid(self, queue_ptr):
        sq_get_shmid = self.libshm.sq_get_shmid
        shm_queue_ptr = ctypes.POINTER(shm_queue)
        sq_get_shmid.argtypes = [shm_queue_ptr]
        sq_get_shmid.restype = ctypes.c_int

        return sq_get_shmid(queue_ptr)
    
    # 打开匿名共享内存， 通过shm_id
    def open_by_shmid(self, shm_id):
        self.shm_id = shm_id
        shm_queue_ptr = ctypes.POINTER(shm_queue)
        sq_open_by_shmid = self.libshm.sq_open_by_shmid
        sq_open_by_shmid.argtypes = [ctypes.c_int]
        sq_open_by_shmid.restype = shm_queue_ptr
        # sq_open.restype = ctypes.c_void_p
        queue_ptr = sq_open_by_shmid(shm_id)
        
        # 判断返回的指针是否为NULL, 要转为 None， 不能直接返回， 
        if queue_ptr:
            return queue_ptr
        else:
            return None
        
        
    # 打开 有key的共享内存
    def open(self, shm_key):
        self.shm_key = shm_key
        shm_queue_ptr = ctypes.POINTER(shm_queue)
        sq_open = self.libshm.sq_open
        sq_open.argtypes = [ctypes.c_ulonglong]
        sq_open.restype = shm_queue_ptr
        # sq_open.restype = ctypes.c_void_p
        queue_ptr = sq_open(shm_key)
        
        # 判断返回的指针是否为NULL, 要转为 None， 不能直接返回， 
        if queue_ptr:
            return queue_ptr
        else:
            return None
    
    #写数据， 成功返回 0 
    def put(self, queue_ptr, data, datalen):
        sq_put = self.libshm.sq_put
        shm_queue_ptr = ctypes.POINTER(shm_queue)
        sq_put.argtypes = [shm_queue_ptr, ctypes.c_void_p, ctypes.c_int]
        sq_put.restype = ctypes.c_int
        # sq_put.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        result = sq_put(queue_ptr, data, datalen)
        
        #  -1 - invalid parameter
        #  -2 - shm queue is full
        if result == -1:
            LOG.error(f"mem put, invalid parameter , id:{self.shm_id} or key:{self.shm_key}")
        if result == -2:
            LOG.error(f"mem put, shm queue is full , id:{self.shm_id} or key:{self.shm_key}")
            
        return result
    
       
    #  C读取函数
    #  // Retrieve data
    # // On success, buf is filled with the first queue data
    # // this function is multi-thread/multi-process safe
    # // Returns the data length or
    # //      0 - no data in queue
    # //     -1 - invalid parameter
    # int sq_get(struct shm_queue *queue, void *buf, int buf_sz, struct timeval *enqueue_time);

    # 读取数据， 可用于测试
    # 返回buffer，和 time
    def get(self, queue_ptr, buf_sz):
        sq_get = self.libshm.sq_get
        shm_queue_ptr = ctypes.POINTER(shm_queue)
        sq_get.argtypes = [shm_queue_ptr, ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(timeval)]
        sq_get.restype = ctypes.c_int
        
        buf = ctypes.create_string_buffer(ctypes.c_int(buf_sz).value)
        enqueue_time = timeval()
        result = sq_get(queue_ptr, buf, buf_sz, ctypes.byref(enqueue_time))
        if result > 0:
            data = buf.raw[:result]
            # LOG.error(f"mem get, OK, id or key:{self.shm_key}")
            return data
        elif result == 0:
            LOG.warning(f"mem get, no data in queue , id:{self.shm_id} or key:{self.shm_key}")
            return None
        elif result == -1:
            LOG.error(f"mem get, invalid parameter , id:{self.shm_id} or key:{self.shm_key}")
            return None
        
    
    #销毁
    def destroy(self, queue_ptr):
        sq_destroy = self.libshm.sq_destroy
        shm_queue_ptr = ctypes.POINTER(shm_queue)
        sq_destroy.argtypes = [shm_queue_ptr]
        sq_destroy(queue_ptr)
      
    #销毁 删除
    def destroy_and_remove(self, queue_ptr):
        sq_destroy_and_remove = self.libshm.sq_destroy_and_remove
        shm_queue_ptr = ctypes.POINTER(shm_queue)
        sq_destroy_and_remove.argtypes = [shm_queue_ptr]
        sq_destroy_and_remove(queue_ptr)

#搞一个空的类 来承接 c的struct shm_queue         
class shm_queue(ctypes.Structure):
    pass

# 定义C结构体timeval的等效结构体
class timeval(ctypes.Structure):
    _fields_ = [
        ('tv_sec', ctypes.c_long),
        ('tv_usec', ctypes.c_long)
    ]

# if __name__ == '__main__':
#     shm = ShmQueue()
#     shm_ptr = shm.create(0, 1024*1024, 1000, 1, 2)
#     # b_shm = shm.open(100)
    
#     id = shm.get_shmid(shm_ptr)
#     shm_ptr = shm.open_by_shmid(id)
    
#     data = b"abcd"
#     shm.put(shm_ptr, data, len(data))
    
#     data_result = shm.get(shm_ptr, 100000)
    
#     LOG.info(f" ======= {data_result}")
