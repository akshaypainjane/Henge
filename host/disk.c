#include "../core/machine.h"
#include "disk.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int hv_disk_open(HvDisk *d, const char *path, uint64_t offset, int readonly, bool uncached_writes) {
    d->write_fd = -1;
    d->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (d->fd < 0) return -1;
    struct stat st;
    if (fstat(d->fd, &st) != 0 || (uint64_t)st.st_size < offset) {
        hv_disk_close(d);
        return -1;
    }
    d->offset = offset;
    d->size = (uint64_t)st.st_size - offset;
    d->readonly = readonly;
    d->flush_mode = HV_DISK_FLUSH_LEGACY;
    if (uncached_writes && !readonly) {
        /* Keep reads cached. If a second descriptor cannot bypass the cache
           or the path was replaced between opens, use the original one. */
        int fd = open(path, O_WRONLY);
        if (fd >= 0) {
            struct stat writer;
            if (fstat(fd, &writer) == 0 && writer.st_dev == st.st_dev && writer.st_ino == st.st_ino &&
                fcntl(fd, F_NOCACHE, 1) == 0)
                d->write_fd = fd;
            else
                close(fd);
        }
    }
    return 0;
}

void hv_disk_close(HvDisk *d) {
    if (d->write_fd >= 0) close(d->write_fd);
    if (d->fd >= 0) close(d->fd);
    d->fd = d->write_fd = -1;
}

int hv_disk_read(HvDisk *d, uint64_t off, void *buf, uint64_t len) {
    if (off > d->size || len > d->size - off) return -1;
    uint8_t *p = buf;
    while (len) {
        ssize_t n = pread(d->fd, p, len, (off_t)(d->offset + off));
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

int hv_disk_write(HvDisk *d, uint64_t off, const void *buf, uint64_t len) {
    if (d->readonly || off > d->size || len > d->size - off) return -1;
    int fd = d->write_fd >= 0 ? d->write_fd : d->fd;
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = pwrite(fd, p, len, (off_t)(d->offset + off));
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

/* CLI experiment HV_FLUSH: 0 fsync, 1 host cache only, 2 write barrier.
   The app sets the policy per disk and always requests synchronization. */
int hv_disk_flush(HvDisk *d) {
    if (d->readonly) return 0;
    HvDiskFlushMode mode = d->flush_mode;
    if (mode == HV_DISK_FLUSH_LEGACY) {
        int legacy = hv_knob[HV_K_FLUSH];
        if (legacy == 1) return 0;
        mode = legacy == 2 ? HV_DISK_FLUSH_BARRIER : HV_DISK_FLUSH_SYNC;
    }
    int result;
    if (mode == HV_DISK_FLUSH_BARRIER) {
        do { result = fcntl(d->fd, F_BARRIERFSYNC); } while (result == -1 && errno == EINTR);
        if (result != -1) return 0;
        /* Preserve real I/O failures. A backing filesystem can reject the
           barrier command even when it implements regular fsync. */
        if (errno != ENOTSUP && errno != ENOSYS && errno != ENOTTY && errno != EINVAL)
            return -1;
    }
    do { result = fsync(d->fd); } while (result == -1 && errno == EINTR);
    return result;
}
