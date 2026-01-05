// QNX Platform Compatibility Header
#pragma once

#ifdef __QNXNTO__

#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

// QNX specific configuration
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0  // Not supported on QNX, use 0 as fallback
#endif

// QNX uses /dev/shmem instead of /dev/shm
#define LIBRPC_SHM_DIR "/dev/shmem"

// ==========================================
// flock() emulation using fcntl()
// WARNING: This is NOT 100% compatible with BSD flock()
// 1. Locks are per-process, not per-file-descriptor.
// 2. Closing ANY fd for the file releases the lock.
// 3. Locks are not inherited by child processes.
// ==========================================

#ifndef LOCK_SH
#define LOCK_SH 1  // Shared lock
#endif
#ifndef LOCK_EX
#define LOCK_EX 2  // Exclusive lock
#endif
#ifndef LOCK_NB
#define LOCK_NB 4  // Don't block when locking
#endif
#ifndef LOCK_UN
#define LOCK_UN 8  // Unlock
#endif

inline int flock(int fd, int operation) {
    struct flock lock;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0; // Lock the whole file

    if (operation & LOCK_UN) {
        lock.l_type = F_UNLCK;
        return fcntl(fd, F_SETLK, &lock);
    }

    if (operation & LOCK_SH) {
        lock.l_type = F_RDLCK;
    } else if (operation & LOCK_EX) {
        lock.l_type = F_WRLCK;
    } else {
        errno = EINVAL;
        return -1;
    }

    int cmd = (operation & LOCK_NB) ? F_SETLK : F_SETLKW;
    return fcntl(fd, cmd, &lock);
}

#else

// Linux/POSIX configuration
#define LIBRPC_SHM_DIR "/dev/shm"

#endif  // __QNXNTO__
