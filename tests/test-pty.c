#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define ROWS 10
#define COLS 60

static pid_t active;

static void check(bool ok, const char *what, int line)
{
    if (ok)
        return;
    fprintf(stderr, "PTY test line %d: %s\n", line, what);
    if (active > 0) {
        kill(active, SIGKILL);
        waitpid(active, NULL, 0);
    }
    exit(1);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

struct screen {
    char cell[ROWS][COLS + 1];
    int rows, cols, row, col;
    int escape, number;
};

struct terminal {
    int master, slave;
    pid_t child;
    struct termios initial;
    struct screen screen;
};

static void byte(struct screen *s, unsigned char c)
{
    if (s->escape == 1) {
        CHECK(c == '[');
        s->escape = 2;
        s->number = 0;
    } else if (s->escape == 2) {
        if (c >= '0' && c <= '9') {
            s->number = 10 * s->number + (c - '0');
            return;
        }
        int n = s->number ? s->number : 1;
        if (c == 'A') {
            s->row -= n;
            if (s->row < 0)
                s->row = 0;
        } else if (c == 'B') {
            s->row += n;
            if (s->row >= s->rows)
                s->row = s->rows - 1;
        } else {
            // These are the only controls the viewer may write. In
            // particular, clearing or switching the screen fails the test.
            CHECK(c == 'K' && !s->number);
            memset(s->cell[s->row] + s->col, ' ', (size_t)(s->cols - s->col));
        }
        if (s->col == s->cols)
            s->col--;
        s->escape = 0;
    } else if (c == 27) {
        s->escape = 1;
    } else if (c == '\r') {
        s->col = 0;
    } else if (c == '\n') {
        if (++s->row == s->rows) {
            s->row--;
            memmove(s->cell[0], s->cell[1], (size_t)(s->rows - 1) * sizeof(s->cell[0]));
            memset(s->cell[s->row], ' ', (size_t)s->cols);
        }
    } else {
        CHECK(c >= ' ' && c <= '~');
        CHECK(s->col < s->cols);
        s->cell[s->row][s->col++] = (char)c;
    }
}

static void drain(struct terminal *t, int timeout)
{
    struct pollfd fd = {.fd = t->master, .events = POLLIN};
    int ready = poll(&fd, 1, timeout);
    if (ready < 0 && errno == EINTR)
        return;
    CHECK(ready >= 0);
    if (ready) {
        unsigned char buf[4096];
        ssize_t count = read(t->master, buf, sizeof(buf));
        CHECK(count > 0);
        for (ssize_t i = 0; i < count; i++)
            byte(&t->screen, buf[i]);
    }
}

static bool contains(struct terminal *t, int row, const char *expected)
{
    return !strncmp(t->screen.cell[row], expected, strlen(expected));
}

static void expect(struct terminal *t, int row, const char *expected)
{
    for (int i = 0; i < 60; i++) {
        if (contains(t, row, expected))
            return;
        drain(t, 50);
    }
    fprintf(stderr, "wanted row %d starting with [%s]\n", row, expected);
    for (int i = 0; i < t->screen.rows; i++)
        fprintf(stderr, "%d [%s]\n", i, t->screen.cell[i]);
    CHECK(false);
}

static void send_key(struct terminal *t, const char *keys)
{
    CHECK(write(t->master, keys, strlen(keys)) == (ssize_t)strlen(keys));
}

static void size(struct terminal *t, int rows, int cols)
{
    CHECK(rows <= ROWS && cols <= COLS);
    t->screen.rows = rows;
    t->screen.cols = cols;
    if (t->screen.row >= rows)
        t->screen.row = rows - 1;
    if (t->screen.col >= cols)
        t->screen.col = cols - 1;
    struct winsize ws = {.ws_row = (unsigned short)rows,
                         .ws_col = (unsigned short)cols};
    CHECK(ioctl(t->slave, TIOCSWINSZ, &ws) == 0);
}

static struct terminal start_args(const char *viewer, const char *path1,
                                  const char *path2, int input, int producer)
{
    struct terminal t = {0};
    t.master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(t.master >= 0);
    CHECK(grantpt(t.master) == 0 && unlockpt(t.master) == 0);
    const char *slave = ptsname(t.master);
    CHECK(slave);
    t.slave = open(slave, O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(t.slave >= 0 && tcgetattr(t.slave, &t.initial) == 0);
    for (int row = 0; row < ROWS; row++)
        memset(t.screen.cell[row], ' ', COLS);
    size(&t, 4, 44);

    t.child = fork();
    CHECK(t.child >= 0);
    if (t.child == 0) {
        CHECK(setsid() >= 0 && ioctl(t.slave, TIOCSCTTY, 0) == 0);
        CHECK(tcsetpgrp(t.slave, getpgrp()) == 0);
        CHECK(dup2(input >= 0 ? input : t.slave, STDIN_FILENO) >= 0);
        CHECK(dup2(t.slave, STDOUT_FILENO) >= 0);
        if (producer >= 0)
            close(producer);
        if (input >= 0)
            close(input);
        CHECK(setenv("TERM", "linux", 1) == 0);
        if (path2)
            execl(viewer, viewer, path1, path2, (char *)NULL);
        else
            execl(viewer, viewer, path1, (char *)NULL);
        _exit(127);
    }
    active = t.child;
    return t;
}

static struct terminal start(const char *viewer, const char *path,
                             int input, int producer)
{
    return start_args(viewer, path, NULL, input, producer);
}

static void restored(struct terminal *t)
{
    struct termios now;
    CHECK(tcgetattr(t->slave, &now) == 0);
    CHECK(now.c_iflag == t->initial.c_iflag && now.c_oflag == t->initial.c_oflag &&
          now.c_cflag == t->initial.c_cflag && now.c_lflag == t->initial.c_lflag);
    CHECK(memcmp(now.c_cc, t->initial.c_cc, sizeof(now.c_cc)) == 0);
}

static int wait_child(struct terminal *t, int options)
{
    int status;
    for (int i = 0; i < 60; i++) {
        pid_t done = waitpid(t->child, &status, options | WNOHANG);
        CHECK(done >= 0);
        if (done) {
            if (!WIFSTOPPED(status))
                active = 0;
            return status;
        }
        drain(t, 50);
    }
    CHECK(false);
    return 0;
}

static void finish(struct terminal *t, int code, int row)
{
    int status = wait_child(t, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == code);
    restored(t);
    CHECK(write(t->slave, "shell> ", 7) == 7);
    expect(t, row, "shell> ");
}

static void close_terminal(struct terminal *t)
{
    close(t->master);
    close(t->slave);
}

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0);
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    CHECK(close(fd) == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char path[] = "/tmp/tam-view-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0);

    write_file(path, "01 one\n02 two\n03 three\n04 four\n05 five\n"
                     "06 six\n07 needle.[x]\n08 eight\n09 nine\n"
                     "10 needle.[x]\n11 eleven\n12 twelve\n13 thirteen\n14 end");
    struct terminal t = start(argv[1], path, -1, -1);
    expect(&t, 0, "01 one");
    expect(&t, 3, path);
    send_key(&t, "\033[B");
    expect(&t, 0, "02 two");
    expect(&t, 2, "04 four");
    send_key(&t, "k");
    expect(&t, 0, "01 one");
    send_key(&t, " ");
    expect(&t, 0, "04 four");
    send_key(&t, "\033[5~");
    expect(&t, 0, "01 one");
    send_key(&t, "d");
    expect(&t, 0, "03 three");
    send_key(&t, "u");
    expect(&t, 0, "01 one");
    send_key(&t, "/needle\\.\\[x\\]\r");
    expect(&t, 0, "07 needle.[x]");
    send_key(&t, "n");
    expect(&t, 0, "10 needle.[x]");
    send_key(&t, "N");
    expect(&t, 0, "07 needle.[x]");
    send_key(&t, "ma");
    send_key(&t, "11g");
    expect(&t, 0, "11 eleven");
    send_key(&t, "'a");
    expect(&t, 0, "07 needle.[x]");
    send_key(&t, "''");
    expect(&t, 0, "11 eleven");
    send_key(&t, "?^10 needle\\.\r");
    expect(&t, 0, "10 needle.[x]");
    send_key(&t, "/[unclosed\r");
    expect(&t, 3, "Invalid pattern");
    send_key(&t, "/missing\r");
    expect(&t, 3, "Pattern not found");
    send_key(&t, "h");
    expect(&t, 0, "tam-view keys");
    send_key(&t, "q");
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "10 needle.[x]") && contains(&t, 2, "12 twelve"));
    close_terminal(&t);

    write_file(path, "first\nsecond\nthird");
    t = start(argv[1], path, -1, -1);
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "first") && contains(&t, 2, "third"));
    close_terminal(&t);

    write_file(path, "");
    t = start(argv[1], path, -1, -1);
    finish(&t, 0, 0);
    close_terminal(&t);

    write_file(path, "first\nsecond\nthird\nfourth\nfifth\n");
    t = start(argv[1], path, -1, -1);
    expect(&t, 3, path);
    send_key(&t, " ");
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "third") && contains(&t, 2, "fifth"));
    close_terminal(&t);

    fd = open(path, O_WRONLY);
    CHECK(fd >= 0);
    CHECK(lseek(fd, (off_t)8 * 1024 * 1024 * 1024, SEEK_SET) >= 0);
    const char tail[] = "\nlast-2\nlast-1\nlast";
    CHECK(write(fd, tail, sizeof(tail) - 1) == (ssize_t)sizeof(tail) - 1);
    CHECK(close(fd) == 0);
    t = start(argv[1], path, -1, -1);
    expect(&t, 3, path);
    send_key(&t, "G");
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "last-2") && contains(&t, 2, "last"));
    close_terminal(&t);

    int stream[2];
    CHECK(pipe(stream) == 0);
    t = start(argv[1], NULL, stream[0], stream[1]);
    close(stream[0]);
    expect(&t, 0, "Waiting for input");
    const char chunk[] = "alpha\nbeta\ngamma\ndelta\n";
    CHECK(write(stream[1], chunk, sizeof(chunk) - 1) == (ssize_t)sizeof(chunk) - 1);
    expect(&t, 2, "gamma");
    send_key(&t, "j");
    expect(&t, 2, "delta");
    send_key(&t, "k");
    expect(&t, 0, "alpha");
    send_key(&t, "/later\r");
    expect(&t, 3, "Searching");
    send_key(&t, "\003");
    expect(&t, 3, "Canceled");
    send_key(&t, "q");
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "alpha") && contains(&t, 2, "gamma"));
    close(stream[1]);
    close_terminal(&t);

    CHECK(pipe(stream) == 0);
    t = start(argv[1], "-", stream[0], stream[1]);
    close(stream[0]);
    CHECK(write(stream[1], "a\n", 2) == 2);
    expect(&t, 0, "a");
    send_key(&t, "j");
    expect(&t, 1, "Waiting for input");
    CHECK(contains(&t, 0, "a"));
    CHECK(write(stream[1], "b", 1) == 1);
    CHECK(close(stream[1]) == 0);
    finish(&t, 0, 2);
    CHECK(contains(&t, 1, "b"));
    close_terminal(&t);

    write_file(path, "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n");
    t = start(argv[1], path, -1, -1);
    expect(&t, 3, path);
    send_key(&t, "\032");
    CHECK(WIFSTOPPED(wait_child(&t, WUNTRACED)));
    restored(&t);
    CHECK(kill(t.child, SIGCONT) == 0);
    size(&t, 5, 40);
    expect(&t, 4, path);
    CHECK(contains(&t, 0, "one") && contains(&t, 3, "four"));
    CHECK(kill(t.child, SIGTERM) == 0);
    finish(&t, 128 + SIGTERM, 4);
    CHECK(contains(&t, 0, "one") && contains(&t, 3, "four"));
    close_terminal(&t);

    // Horizontal scrolling & chop toggle.
    write_file(path, "01234567890123456789012345678901234567890123456789\n"
                     "......................line2\n"
                     "......................line3\n"
                     "......................line4\n"
                     "......................line5\n");
    t = start(argv[1], path, -1, -1);
    expect(&t, 0, "01234567890123456789012345678901234567890123");
    send_key(&t, "\033[C");
    expect(&t, 0, "2345678901234567890123456789");
    expect(&t, 1, "line2");
    send_key(&t, "\033[D");
    expect(&t, 0, "01234567890123456789012345678901234567890123");
    send_key(&t, "S");
    expect(&t, 1, "......................line2");
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);

    // Multiple-file navigation.
    char path2[] = "/tmp/tam-view-test2-XXXXXX";
    int fd2 = mkstemp(path2);
    CHECK(fd2 >= 0 && close(fd2) == 0);
    write_file(path, "file1-a\nfile1-b\nfile1-c\nfile1-d\n");
    write_file(path2, "file2-a\nfile2-b\nfile2-c\nfile2-d\n");
    t = start_args(argv[1], path, path2, -1, -1);
    expect(&t, 0, "file1-a");
    expect(&t, 3, "(file 1 of 2)");
    send_key(&t, " ");
    expect(&t, 3, "End of file");
    send_key(&t, ":n");
    expect(&t, 0, "file2-a");
    expect(&t, 3, "(file 2 of 2)");
    send_key(&t, ":p");
    expect(&t, 3, "End of file");
    send_key(&t, "g");
    expect(&t, 0, "file1-a");
    send_key(&t, "2:x");
    expect(&t, 0, "file2-a");
    send_key(&t, ":f");
    expect(&t, 3, "file 2 of 2:");
    send_key(&t, " ");
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "file2-b") && contains(&t, 2, "file2-d"));
    close_terminal(&t);
    CHECK(unlink(path2) == 0);

    // Follow mode on a growing file.
    write_file(path, "log-1\nlog-2\nlog-3\nlog-4\n");
    t = start(argv[1], path, -1, -1);
    expect(&t, 0, "log-1");
    send_key(&t, "F");
    expect(&t, 2, "log-4");
    expect(&t, 3, "Waiting for data");
    fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(write(fd, "log-5\n", 6) == 6);
    CHECK(close(fd) == 0);
    expect(&t, 2, "log-5");
    send_key(&t, "\003");
    expect(&t, 3, "Follow stopped");
    send_key(&t, "q");
    finish(&t, 0, 3);
    CHECK(contains(&t, 2, "log-5"));
    close_terminal(&t);

    // Render mode (-R) and R toggle to raw and back.
    write_file(path, "# Heading\n\n- bullet\n\nline3\nline4\n");
    t = start_args(argv[1], "-R", path, -1, -1);
    expect(&t, 0, "Heading");
    expect(&t, 1, "=======");
    send_key(&t, "R");
    expect(&t, 0, "# Heading");
    expect(&t, 3, "Raw mode");
    send_key(&t, "R");
    expect(&t, 0, "Heading");
    expect(&t, 3, "Render mode");
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);

    CHECK(unlink(path) == 0);
    puts("PTY: ok");
    return 0;
}
