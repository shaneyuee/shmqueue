package main

import (
	"bufio"
	"fmt"
	"os"
	"shmqueue"
	"strconv"
	"time"
	"unsafe"
)

func main() {
    var hascmd = false
    var open_by_id = false
    var create bool = true
    var key uint64 = 0
    var shmid int = 0
    var elementSize int = 0
    var elementCount int = 0
    var shmq unsafe.Pointer = nil

    for k,v:= range os.Args {
        fmt.Printf("Args[%v]=[%v]\n",k,v)
	if k == 0 { continue }

        if k == 1 {
            hascmd = true
            if v == "create" {
               create = true
            } else if v == "open" {
               create = false
               open_by_id = false
            } else if v == "openid" {
               create = false
               open_by_id = true
            } else {
               fmt.Printf("Bad command: %s\n", v)
               return
            }
        }

        if k == 2 {
            if open_by_id {
                i, _ := strconv.ParseInt(v, 0, 32)
                shmid = int(i)
            } else {
                i, _ := strconv.ParseInt(v, 0, 64)
                key = uint64(i)
            }
        }

        if k == 3 {
            i,_ := strconv.ParseInt(v, 0, 32)
            elementSize = int(i)
        }

        if k == 4 {
            i,_ := strconv.ParseInt(v, 0, 32)
            elementCount = int(i)
        }
    }

    if !hascmd {
        fmt.Printf("usages:\n")
        fmt.Printf("./test create <shm_key> <element_size> <element_count>\n")
        fmt.Printf("./test open <shm_key>\n")
        fmt.Printf("./test openid <shm_id>\n")
        return
    }

    if create {
        if key == 0 || elementSize <= 10 || elementCount <= 10 {
            fmt.Printf("Bad arguments.\n")
            return
        }
        shmq = shmqueue.SQCreate(key, elementSize, elementCount)
    } else if shmid != 0 {
        shmq = shmqueue.SQOpenByShmID(shmid)
    } else if key != 0 {
        shmq = shmqueue.SQOpen(key)
    } else {
        fmt.Printf("Bad arguments.\n")
        return
    }

    opr := "open"
    if create {
        opr = "create"
    }

    if shmq == nil {
        fmt.Printf("Failed to %s shm queue, error: %v\n", opr, shmqueue.SQErrorStr(shmq))
        return
    }

    fmt.Printf("%s shm queue successfully, shm_id is %d\n\n", opr, shmqueue.SQGetShmID(shmq))
    fmt.Printf("Available commands:\n")
    fmt.Printf("    read\n")
    fmt.Printf("    write <text>\n")
    fmt.Printf("    usage\n")
    fmt.Printf("Please input command to continue:\n")

    reader := bufio.NewReader(os.Stdin)
    buf := make([]byte, 10240)

    for {
        result, err := reader.ReadString('\n');
        if err != nil { break }

        if result == "read" {
            ret, t := shmqueue.SQGet(shmq, buf)
            if ret == 0 {
                fmt.Printf("Queue is empty!\n")
            } else if ret < 0 {
                fmt.Printf("Read error: %v!\n", shmqueue.SQErrorStr(shmq))
            } else {
                tm := time.UnixMilli(int64(t))
                fmt.Printf("[DATA][%v]%v\n", tm, string(buf[:]))
            }
        } else if result[0:6] == "write " || result[0:6] == "write\t" {
            data := []byte(result[6:])
            ret := shmqueue.SQPut(shmq, data)
            if ret == -2 {
                fmt.Printf("Queue is full!\n")
            } else if ret < 0 {
                fmt.Printf("Write error: %v!\n", shmqueue.SQErrorStr(shmq))
            } else {
                fmt.Printf("Write successfully.\n")
            }
        } else if result == "usage" {
            fmt.Printf("Current usage: %v%%\n", shmqueue.SQGetUsage(shmq))
        } else {
            fmt.Printf("Bad command!\n")
        }
    }
}

