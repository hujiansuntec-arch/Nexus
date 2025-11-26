// QNX Platform Compatibility Header
#pragma once

#ifdef __QNXNTO__

// QNX specific configuration
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0  // Not supported on QNX, use 0 as fallback
#endif

// QNX uses /dev/shmem instead of /dev/shm
#define LIBRPC_SHM_DIR "/dev/shmem"

#else

// Linux/POSIX configuration
#define LIBRPC_SHM_DIR "/dev/shm"

#endif // __QNXNTO__
