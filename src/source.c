#include "view.h"

#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

int source_open(struct source *s, int input)
{
    *s = (struct source){.input = input, .fd = input, .block = -1};
    struct stat st;
    if (fstat(input, &st) < 0)
        return -1;
    if (S_ISREG(st.st_mode)) {
        s->eof = true;
        off_t start = lseek(input, 0, SEEK_CUR);
        if (start < 0)
            return -1;
        // An already-positioned stdin must start at its current byte.
        if (start == 0) {
            s->size = st.st_size;
            return 0;
        }
        s->eof = false;
    }

    s->spool = tmpfile();
    if (!s->spool)
        return -1;
    s->fd = fileno(s->spool);
    return 0;
}

void source_close(struct source *s)
{
    if (s->spool)
        fclose(s->spool);
}

int source_pump(struct source *s)
{
    unsigned char buf[VIEW_BLOCK];
    ssize_t count = read(s->input, buf, sizeof(buf));
    if (count < 0)
        return errno == EINTR || errno == EAGAIN ? 0 : -1;
    if (count == 0) {
        s->eof = true;
        return 0;
    }

    if (s->size > INT64_MAX - count) {
        errno = EFBIG;
        return -1;
    }
    for (ssize_t done = 0; done < count;) {
        ssize_t n = pwrite(s->fd, buf + done, (size_t)(count - done),
                           s->size + done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        done += n;
    }
    s->size += count;
    return 0;
}

int source_byte(struct source *s, off_t offset)
{
    if (s->spool && offset >= s->size)
        return s->eof ? VIEW_EOF : VIEW_WAIT;

    if (s->block >= 0 && offset >= s->block &&
        (uintmax_t)(offset - s->block) < s->used)
        return s->data[offset - s->block];

    // Align reads for backward scans. A partial block is retried from the
    // requested byte if its previous read ended before that byte.
    off_t start = offset - offset % VIEW_BLOCK;
    if (start == s->block)
        start = offset;

    ssize_t count;
    do {
        count = pread(s->fd, s->data, sizeof(s->data), start);
    } while (count < 0 && errno == EINTR);
    s->block = start;
    s->used = count > 0 ? (size_t)count : 0;
    if (count < 0)
        return VIEW_ERROR;
    if ((offset - start) >= count)
        return VIEW_EOF;
    return s->data[offset - start];
}

off_t source_size(struct source *s)
{
    struct stat st;
    if (s->spool)
        return s->size;
    return fstat(s->fd, &st) == 0 ? st.st_size : -1;
}

int source_refresh(struct source *s, off_t *new_size)
{
    if (s->spool) {
        *new_size = s->size;
        return 0;
    }
    struct stat st;
    if (fstat(s->fd, &st) < 0)
        return -1;
    if (st.st_size != s->size) {
        s->block = -1;
        s->used = 0;
        s->size = st.st_size;
    }
    *new_size = st.st_size;
    return 0;
}
