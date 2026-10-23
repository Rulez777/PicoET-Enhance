// Shared file boundary for root controls and the root-owned service payload.
#ifndef PICOET_ROOT_FILE_H
#define PICOET_ROOT_FILE_H
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Do not follow a replaced /data/local/tmp leaf or block on a planted FIFO.
// Callers must not pass O_TRUNC: validate the descriptor before changing data.
static inline int picoet_root_file(int fd)
{
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
        !(st.st_mode & 0022) && st.st_nlink == 1) return fd;
    close(fd);
    errno = EPERM;
    return -1;
}
static inline int picoet_open_root_file(const char *path, int flags, mode_t mode)
{
    return picoet_root_file(open(path, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, mode));
}
static inline int picoet_open_runtime(void)
{
    int fd = open("/data/local/tmp/picoet-runtime", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0 &&
        (st.st_mode & 0777) == 0700) return fd;
    close(fd);
    errno = EPERM;
    return -1;
}
#endif
