#include "view.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fputs("Usage: tam-view [--raw] [--] [FILE|- ...]\n"
          "Page through text files or piped input. The final page stays visible.\n"
          "Press h for keys, or q to quit.\n", out);
}

static int copy(int fd)
{
    char buf[VIEW_BLOCK];
    for (;;) {
        ssize_t count = read(fd, buf, sizeof(buf));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return count < 0 ? -1 : 0;
        for (ssize_t done = 0; done < count;) {
            ssize_t n = write(STDOUT_FILENO, buf + done, (size_t)(count - done));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return -1;
            done += n;
        }
    }
}

static int diagnostic(const char *context)
{
    int error = errno;
    char safe[513];
    view_label(safe, sizeof(safe), 512, context);
    fprintf(stderr, "tam-view: %s: %s\n", safe, strerror(error));
    return 1;
}

static int open_input(const char *path, const char **label_out)
{
    if (!path || !strcmp(path, "-")) {
        if (isatty(STDIN_FILENO)) {
            usage(stderr);
            return -2;
        }
        if (label_out)
            *label_out = "standard input";
        return STDIN_FILENO;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        return -1;
    }
    if (label_out)
        *label_out = path;
    return fd;
}

int main(int argc, char **argv)
{
    struct view_input *inputs = calloc((size_t)(argc > 1 ? argc : 1),
                                       sizeof(*inputs));
    if (!inputs)
        return diagnostic("memory");

    size_t file_count = 0;
    bool options = true;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (options && !strcmp(arg, "--")) {
            options = false;
        } else if (options && !strcmp(arg, "--raw")) {
            continue;
        } else if (options && !strcmp(arg, "--help")) {
            free(inputs);
            usage(stdout);
            return 0;
        } else if (options && !strcmp(arg, "--version")) {
            free(inputs);
            puts("tam-view " VIEW_VERSION);
            return 0;
        } else if (options && arg[0] == '-' && arg[1]) {
            free(inputs);
            usage(stderr);
            return 2;
        } else {
            inputs[file_count++] = (struct view_input){.path = arg, .fd = -1};
        }
    }
    if (file_count == 0)
        inputs[file_count++] = (struct view_input){.path = "-", .fd = -1};

    if (!isatty(STDOUT_FILENO)) {
        int status = 0;
        for (size_t i = 0; i < file_count; i++) {
            const char *label = inputs[i].path;
            int fd = open_input(inputs[i].path, &label);
            if (fd == -2) {
                status = 2;
                break;
            }
            if (fd < 0 || copy(fd) < 0) {
                int err = errno;
                if (fd >= 0 && fd != STDIN_FILENO)
                    close(fd);
                errno = err;
                status = diagnostic(label);
                break;
            }
            if (fd != STDIN_FILENO)
                close(fd);
        }
        free(inputs);
        return status;
    }

    const char *first_label = inputs[0].path;
    int input = open_input(inputs[0].path, &first_label);
    if (input == -2) {
        free(inputs);
        return 2;
    }
    if (input < 0) {
        free(inputs);
        return diagnostic(first_label);
    }
    inputs[0].path = first_label;
    inputs[0].fd = input;

    setlocale(LC_CTYPE, "");
    const char *term = getenv("TERM");
    int tty = term && strcmp(term, "dumb")
                ? open("/dev/tty", O_RDWR | O_CLOEXEC) : -1;
    int result = 0;
    if (tty >= 0) {
        result = view_interactive(inputs, file_count, tty);
        close(tty);
    } else {
        for (size_t i = 0; i < file_count; i++) {
            int fd = i == 0 ? input : open_input(inputs[i].path, &first_label);
            if (fd < 0) {
                result = -1;
                first_label = inputs[i].path;
                break;
            }
            struct source s;
            if (source_open(&s, fd) < 0) {
                int err = errno;
                if (fd != STDIN_FILENO && fd != input)
                    close(fd);
                errno = err;
                result = -1;
                first_label = inputs[i].path;
                break;
            }
            result = view_dump(&s, 80);
            int err = errno;
            source_close(&s);
            if (fd != STDIN_FILENO && fd != input)
                close(fd);
            errno = err;
            if (result < 0) {
                first_label = inputs[i].path;
                break;
            }
        }
    }

    int error = errno;
    if (input != STDIN_FILENO)
        close(input);
    free(inputs);
    errno = error;
    return result < 0 ? diagnostic(first_label) : result;
}
