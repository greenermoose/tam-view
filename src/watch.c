#include "view.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SAMPLE_HALF (TAM_SOURCE_BLOCK / 2)

static bool same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static bool same_revision(const struct stat *a, const struct stat *b)
{
    return same_file(a, b) && a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static int read_bytes(int fd, unsigned char *out, size_t count, off_t offset)
{
    while (count) {
        ssize_t n = pread(fd, out, count, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EAGAIN; // Truncated while reading.
            return -1;
        }
        out += n;
        offset += n;
        count -= (size_t)n;
    }
    return 0;
}

static int sample_file(int fd, off_t size, unsigned char *out)
{
    if (size <= TAM_SOURCE_BLOCK)
        return read_bytes(fd, out, (size_t)size, 0);
    if (read_bytes(fd, out, SAMPLE_HALF, 0) < 0)
        return -1;
    return read_bytes(fd, out + SAMPLE_HALF, SAMPLE_HALF, size - SAMPLE_HALF);
}

int watch_check(struct file_watch *watch, const char *path, int fd,
                int *replacement)
{
    *replacement = -1;
    struct stat path_stat, before, after;
    if (stat(path, &path_stat) < 0)
        return -1;
    if (watch->sample && same_revision(&watch->stat, &path_stat))
        return WATCH_UNCHANGED;
    if (fstat(fd, &before) < 0)
        return -1;
    int candidate = -1;
    if (!same_file(&before, &path_stat)) {
        candidate = tam_source_open_file(path);
        if (candidate < 0)
            return -1;
        fd = candidate;
        if (fstat(fd, &before) < 0)
            goto failed;
    }

    int change = WATCH_REWRITE;
    unsigned char sample[TAM_SOURCE_BLOCK];
    if (watch->sample && same_file(&watch->stat, &before) &&
        before.st_size > watch->stat.st_size) {
        if (sample_file(fd, watch->stat.st_size, sample) < 0)
            goto failed;
        if (!memcmp(sample, watch->sample, watch->sample_size))
            change = WATCH_APPEND;
    }
    if (sample_file(fd, before.st_size, sample) < 0 || fstat(fd, &after) < 0)
        goto failed;
    if (!same_revision(&before, &after)) {
        errno = EAGAIN;
        goto failed;
    }
    if (!watch->sample) {
        watch->sample = malloc(TAM_SOURCE_BLOCK);
        if (!watch->sample)
            goto failed;
    }
    watch->sample_size = before.st_size < TAM_SOURCE_BLOCK
                         ? (size_t)before.st_size : TAM_SOURCE_BLOCK;
    memcpy(watch->sample, sample, watch->sample_size);
    watch->stat = before;
    *replacement = candidate;
    return change;

failed:;
    int error = errno;
    if (candidate >= 0)
        close(candidate);
    errno = error;
    return -1;
}

void watch_close(struct file_watch *watch)
{
    free(watch->sample);
}
