#include "platform.h"
#include "runtime.h"
#include "syscall.h"

#define DARWIN_PROT_READ 1
#define DARWIN_PROT_WRITE 2
#define DARWIN_MAP_PRIVATE 2
#define DARWIN_MAP_ANONYMOUS 0x1000
#define DARWIN_O_WRONLY 0x0001
#define DARWIN_O_APPEND 0x0008
#define DARWIN_O_CREAT 0x0200
#define DARWIN_O_TRUNC 0x0400
#define DARWIN_O_EXCL 0x0800
#define DARWIN_SEEK_SET 0
#define DARWIN_SEEK_CUR 1
#define DARWIN_SEEK_END 2

long platform_write(int fd, const void *buffer, size_t count) {
    return darwin_syscall3(DARWIN_SYS_WRITE, (long)fd, (long)buffer, (long)count);
}

long platform_read(int fd, void *buffer, size_t count) {
    return darwin_syscall3(DARWIN_SYS_READ, (long)fd, (long)buffer, (long)count);
}

void *platform_allocate_pages(size_t size) {
    long mapped = darwin_syscall6(
        DARWIN_SYS_MMAP,
        0,
        (long)size,
        DARWIN_PROT_READ | DARWIN_PROT_WRITE,
        DARWIN_MAP_PRIVATE | DARWIN_MAP_ANONYMOUS,
        -1,
        0
    );

    return mapped < 0 ? 0 : (void *)mapped;
}

int platform_open_read(const char *path) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 0;
    }
    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, 0, 0);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_write_mode(const char *path, unsigned int mode, int truncate_existing) {
    long flags = DARWIN_O_WRONLY | DARWIN_O_CREAT;
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 1;
    }
    if (truncate_existing) {
        flags |= DARWIN_O_TRUNC;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, flags, (long)mode);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_write(const char *path, unsigned int mode) {
    return platform_open_write_mode(path, mode, 1);
}

int platform_open_create_exclusive(const char *path, unsigned int mode) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return -1;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, DARWIN_O_WRONLY | DARWIN_O_CREAT | DARWIN_O_EXCL, (long)mode);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_append(const char *path, unsigned int mode) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 1;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, DARWIN_O_WRONLY | DARWIN_O_CREAT | DARWIN_O_APPEND, (long)mode);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_append_existing(const char *path) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 1;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, DARWIN_O_WRONLY | DARWIN_O_APPEND, 0);
    return fd < 0 ? -1 : (int)fd;
}

long long platform_seek(int fd, long long offset, int whence) {
    long native_whence = DARWIN_SEEK_SET;
    long result;

    if (whence == PLATFORM_SEEK_CUR) {
        native_whence = DARWIN_SEEK_CUR;
    } else if (whence == PLATFORM_SEEK_END) {
        native_whence = DARWIN_SEEK_END;
    }

    result = darwin_syscall3(DARWIN_SYS_LSEEK, (long)fd, (long)offset, native_whence);
    return result < 0 ? -1 : (long long)result;
}

int platform_close(int fd) {
    if (fd == 0 || fd == 1) {
        return 0;
    }
    return darwin_syscall1(DARWIN_SYS_CLOSE, (long)fd) < 0 ? -1 : 0;
}

int platform_make_directory(const char *path, unsigned int mode) {
    return darwin_syscall2(DARWIN_SYS_MKDIR, (long)path, (long)mode) < 0 ? -1 : 0;
}

int platform_remove_file(const char *path) {
    return darwin_syscall1(DARWIN_SYS_UNLINK, (long)path) < 0 ? -1 : 0;
}

int platform_remove_directory(const char *path) {
    return darwin_syscall1(DARWIN_SYS_RMDIR, (long)path) < 0 ? -1 : 0;
}