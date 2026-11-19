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
    fputs("Usage: tam-view [--raw] [--] [FILE|-]\n"
          "Page through a text file or piped input. The final page stays visible.\n"
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

int main(int argc, char **argv)
{
    const char *path = NULL;
    bool options = true;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (options && !strcmp(arg, "--")) {
            options = false;
        } else if (options && !strcmp(arg, "--raw")) {
            continue;
        } else if (options && !strcmp(arg, "--help")) {
            usage(stdout);
            return 0;
        } else if (options && !strcmp(arg, "--version")) {
            puts("tam-view " VIEW_VERSION);
            return 0;
        } else if (path || (options && arg[0] == '-' && arg[1])) {
            usage(stderr);
            return 2;
        } else {
            path = arg;
        }
    }

    int input = STDIN_FILENO;
    if (!path || !strcmp(path, "-")) {
        if (isatty(input)) {
            usage(stderr);
            return 2;
        }
        path = "standard input";
    } else {
        input = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (input < 0)
            return diagnostic(path);

        struct stat st;
        if (fstat(input, &st) < 0)
            return diagnostic(path);
        if (!S_ISREG(st.st_mode)) {
            errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
            return diagnostic(path);
        }
    }

    int result;
    if (!isatty(STDOUT_FILENO)) {
        result = copy(input);
    } else {
        setlocale(LC_CTYPE, "");
        struct source s;
        if (source_open(&s, input) < 0)
            return diagnostic(path);
        const char *term = getenv("TERM");
        int tty = term && strcmp(term, "dumb")
                    ? open("/dev/tty", O_RDWR | O_CLOEXEC) : -1;
        if (tty >= 0) {
            result = view_interactive(&s, tty, path);
            close(tty);
        } else {
            result = view_dump(&s, 80);
        }
        int error = errno;
        source_close(&s);
        errno = error;
    }

    int error = errno;
    if (input != STDIN_FILENO)
        close(input);
    errno = error;
    return result < 0 ? diagnostic(path) : result;
}
