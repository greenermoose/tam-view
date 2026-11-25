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
    bool styled[ROWS][COLS];
    int rows, cols, row, col;
    int escape, number;
    int sgr_params[8];
    int sgr_nparams;
    bool sgr_on;
    int sgr_count;
    char osc_buf[256];
    size_t osc_len;
    bool osc8_on;
    int osc8_open_count;
    int osc8_close_count;
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
        if (c == ']') {
            s->escape = 3;
            s->osc_len = 0;
            return;
        }
        CHECK(c == '[');
        s->escape = 2;
        s->number = 0;
        s->sgr_nparams = 0;
    } else if (s->escape == 3) {
        if (c == 27) {
            s->escape = 4;
            return;
        }
        if (s->osc_len + 1 < sizeof(s->osc_buf))
            s->osc_buf[s->osc_len++] = (char)c;
    } else if (s->escape == 4) {
        CHECK(c == '\\');
        s->osc_buf[s->osc_len] = '\0';
        CHECK(strncmp(s->osc_buf, "8;;", 3) == 0);
        if (s->osc_buf[3] != '\0') {
            s->osc8_on = true;
            s->osc8_open_count++;
        } else {
            s->osc8_on = false;
            s->osc8_close_count++;
        }
        s->escape = 0;
    } else if (s->escape == 2) {
        if (c >= '0' && c <= '9') {
            s->number = 10 * s->number + (c - '0');
            return;
        }
        if (c == ';') {
            CHECK(s->sgr_nparams < 8);
            s->sgr_params[s->sgr_nparams++] = s->number;
            s->number = 0;
            return;
        }
        if (c == 'm') {
            CHECK(s->sgr_nparams < 8);
            s->sgr_params[s->sgr_nparams++] = s->number;
            s->sgr_count++;
            s->sgr_on = !(s->sgr_nparams == 1 && s->sgr_params[0] == 0);
            s->escape = 0;
            s->sgr_nparams = 0;
            return;
        }
        CHECK(s->sgr_nparams == 0);
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
            // These are the only non-SGR controls the viewer may write. In
            // particular, clearing or switching the screen fails the test.
            CHECK(c == 'K' && !s->number);
            memset(s->cell[s->row] + s->col, ' ', (size_t)(s->cols - s->col));
            memset(s->styled[s->row] + s->col, 0, (size_t)(s->cols - s->col));
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
            memmove(s->styled[0], s->styled[1], (size_t)(s->rows - 1) * sizeof(s->styled[0]));
            memset(s->styled[s->row], 0, (size_t)s->cols);
        }
    } else {
        CHECK(c >= ' ' && c <= '~');
        CHECK(s->col < s->cols);
        s->cell[s->row][s->col] = (char)c;
        s->styled[s->row][s->col] = s->sgr_on;
        s->col++;
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

static void expect_styled(struct terminal *t, int row, int col, bool want)
{
    for (int i = 0; i < 60; i++) {
        if (t->screen.styled[row][col] == want)
            return;
        drain(t, 50);
    }
    fprintf(stderr, "wanted styled[%d][%d] == %d\n", row, col, (int)want);
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

static struct terminal start_sized(const char *viewer, const char *path1,
                                   const char *path2, int input, int producer,
                                   const char *term_val, int rows, int cols,
                                   const char *prior_output)
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
    size(&t, rows, cols);
    if (prior_output)
        CHECK(write(t.slave, prior_output, strlen(prior_output)) ==
              (ssize_t)strlen(prior_output));

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
        CHECK(setenv("TERM", term_val ? term_val : "linux", 1) == 0);
        if (path2)
            execl(viewer, viewer, path1, path2, (char *)NULL);
        else
            execl(viewer, viewer, path1, (char *)NULL);
        _exit(127);
    }
    active = t.child;
    return t;
}

static struct terminal start_env(const char *viewer, const char *path1,
                                 const char *path2, int input, int producer,
                                 const char *term_val)
{
    return start_sized(viewer, path1, path2, input, producer, term_val,
                       4, 44, NULL);
}

static struct terminal start_args(const char *viewer, const char *path1,
                                  const char *path2, int input, int producer)
{
    return start_env(viewer, path1, path2, input, producer, "linux");
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
    CHECK(!t->screen.sgr_on);
    CHECK(!t->screen.osc8_on);
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

static void append_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    CHECK(close(fd) == 0);
}

static void test_watch(const char *viewer, const char *path)
{
    write_file(path, "one\ntwo\nthree\nfour\nfive");
    struct terminal t = start(viewer, path, -1, -1);
    expect(&t, 3, path);
    append_file(path, "-extended\nsix\n");
    // A simultaneous rewrite in the visible region makes the append
    // observable without relying on a fixed sleep.
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0 && pwrite(fd, "ONE", 3, 0) == 3 && close(fd) == 0);
    expect(&t, 0, "ONE");
    fd = open(path, O_WRONLY);
    CHECK(fd >= 0 && pwrite(fd, "TWO", 3, 4) == 3 && close(fd) == 0);
    expect(&t, 1, "TWO");
    append_file(path, "seven\n");
    for (int i = 0; i < 8; i++)
        drain(&t, 50);
    CHECK(contains(&t, 0, "ONE") && contains(&t, 2, "three"));
    send_key(&t, "G");
    expect(&t, 2, "seven");
    append_file(path, "eight");
    expect(&t, 2, "eight");
    append_file(path, "-extended");
    expect(&t, 2, "eight-extended");

    // Atomic replacement and temporary absence both use the viewed path.
    char replacement[] = "/tmp/tam-view-watch-XXXXXX";
    fd = mkstemp(replacement);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(rename(path, replacement) == 0);
    expect(&t, 3, "File unavailable");
    write_file(replacement, "after rotation\n");
    CHECK(rename(replacement, path) == 0);
    expect(&t, 0, "after rotation");
    write_file(path, "");
    expect(&t, 0, "   ");
    append_file(path, "new start\n");
    expect(&t, 0, "new start");
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);

    write_file(path, "old first\nold second\nold third\n");
    fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    char block[4096];
    memset(block, 'x', sizeof(block));
    for (size_t i = 63; i < sizeof(block); i += 64)
        block[i] = '\n';
    for (int i = 0; i < 20; i++)
        CHECK(write(fd, block, sizeof(block)) == (ssize_t)sizeof(block));
    CHECK(close(fd) == 0);
    append_file(path, "old fourth\nold fifth\n");
    t = start_args(viewer, "--no-watch", path, -1, -1);
    expect(&t, 0, "old first");
    write_file(path, "new first\nnew second\nnew third\nnew fourth\nnew fifth\n");
    send_key(&t, "G");
    expect(&t, 2, "old fifth");
    send_key(&t, "F");
    expect(&t, 3, "Watch disabled");
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);

    write_file(path, "# Title\n\nFirst paragraph.\n\nSecond paragraph.\n\n"
                     "Third paragraph.\n\nFourth paragraph.\n\nFifth paragraph.\n");
    t = start_args(viewer, "-R", path, -1, -1);
    expect(&t, 0, "Title");
    send_key(&t, "6g");
    expect(&t, 0, "Second paragraph.");
    char saved[] = "/tmp/tam-view-watch-save-XXXXXX";
    fd = mkstemp(saved);
    CHECK(fd >= 0 && close(fd) == 0);
    write_file(saved, "# Title\n\nInserted above.\n\nFirst paragraph.\n\n"
                      "Second paragraph.\n\nNEW third paragraph.\n\nFourth paragraph.\n");
    CHECK(rename(saved, path) == 0);
    expect(&t, 2, "NEW third paragraph.");
    CHECK(contains(&t, 0, "Second paragraph."));
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);
}

static void test_pipe_follow(const char *viewer)
{
    int stream[2];
    CHECK(pipe(stream) == 0);
    struct terminal t = start(viewer, "-", stream[0], stream[1]);
    close(stream[0]);
    expect(&t, 3, "Waiting for input");
    const char initial[] = "log1\nlog2\nlog3\nlog4\n";
    CHECK(write(stream[1], initial, sizeof(initial) - 1) == (ssize_t)sizeof(initial) - 1);
    expect(&t, 2, "log3");
    send_key(&t, "F");
    expect(&t, 2, "log4");
    CHECK(write(stream[1], "log5\n", 5) == 5);
    expect(&t, 2, "log5");
    send_key(&t, "\003");
    expect(&t, 3, "Follow stopped");
    send_key(&t, "q");
    finish(&t, 0, 3);
    close(stream[1]);
    close_terminal(&t);
}

static void expect_blank(struct terminal *t, int row)
{
    for (int col = 0; col < t->screen.cols; col++)
        CHECK(t->screen.cell[row][col] == ' ');
}

static void test_page_overlap(const char *viewer, const char *path)
{
    write_file(path, "row 1\nrow 2\nrow 3\nrow 4\nrow 5\nrow 6\nrow 7\n");
    struct terminal t = start_sized(viewer, "--overlap=1", path, -1, -1,
                                    "linux", 5, 44, NULL);
    expect(&t, 3, "row 4");
    send_key(&t, "\033[6~");
    expect(&t, 0, "row 4");
    expect(&t, 3, "row 7");
    send_key(&t, "b");
    expect(&t, 0, "row 1");
    size(&t, 6, 44);
    expect(&t, 4, "row 5");
    send_key(&t, "\033[6~");
    expect(&t, 0, "row 5");
    expect(&t, 5, "End of file");
    expect_blank(&t, 3);
    expect_blank(&t, 4);
    send_key(&t, "\033[5~");
    expect(&t, 0, "row 1");
    send_key(&t, "q");
    finish(&t, 0, 5);
    close_terminal(&t);

    write_file(path, "row 1\nrow 2\nrow 3\nrow 4\nrow 5\n");
    t = start_args(viewer, "--overlap=0", path, -1, -1);
    expect(&t, 2, "row 3");
    send_key(&t, " ");
    expect(&t, 3, "End of file");
    CHECK(contains(&t, 0, "row 4") && contains(&t, 1, "row 5"));
    expect_blank(&t, 2);
    send_key(&t, " b");
    expect(&t, 0, "row 1");
    send_key(&t, "  q");
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "row 4") && contains(&t, 1, "row 5"));
    expect_blank(&t, 2);
    close_terminal(&t);

    write_file(path, "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDD\nrow 5\n");
    t = start_sized(viewer, "--overlap=99", path, -1, -1,
                    "linux", 4, 10, NULL);
    expect(&t, 2, "CCCCCCCCCC");
    send_key(&t, " ");
    expect(&t, 0, "BBBBBBBBBB");
    expect(&t, 2, "DDDDDDDDDD");
    send_key(&t, "b");
    expect(&t, 0, "AAAAAAAAAA");
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);
}

static void test_short_screen(const char *viewer, const char *path)
{
    write_file(path, "only row\n");
    const char *prior = "old first\r\nold second\r\nold prompt";
    struct terminal t = start_sized(viewer, path, NULL, -1, -1,
                                    "linux", 9, 44, prior);
    expect(&t, 8, "End of file");
    CHECK(contains(&t, 0, "only row"));
    for (int row = 1; row < 8; row++)
        expect_blank(&t, row);
    send_key(&t, "q");
    finish(&t, 0, 8);
    CHECK(contains(&t, 0, "only row"));
    close_terminal(&t);

    t = start_sized(viewer, "-E", path, -1, -1, "linux", 9, 44, prior);
    finish(&t, 0, 8);
    CHECK(contains(&t, 0, "only row"));
    for (int row = 1; row < 8; row++)
        expect_blank(&t, row);
    close_terminal(&t);

    t = start_sized(viewer, path, NULL, -1, -1, "linux", 1, 44, NULL);
    expect(&t, 0, "only row");
    send_key(&t, "q");
    finish(&t, 0, 0);
    close_terminal(&t);
}

static void test_pipe_page(const char *viewer)
{
    int input[2];
    CHECK(pipe(input) == 0);
    struct terminal t = start_args(viewer, "--overlap=0", "-", input[0], input[1]);
    close(input[0]);
    CHECK(write(input[1], "a\nb\nc\n", 6) == 6);
    expect(&t, 2, "c");
    send_key(&t, " ");
    expect(&t, 3, "Waiting for input");
    CHECK(contains(&t, 0, "a"));
    CHECK(write(input[1], "d\n", 2) == 2);
    expect(&t, 0, "d");
    send_key(&t, " ");
    CHECK(close(input[1]) == 0);
    expect(&t, 3, "End of file");
    CHECK(contains(&t, 0, "d"));
    expect_blank(&t, 1);
    expect_blank(&t, 2);
    send_key(&t, "b");
    expect(&t, 0, "a");
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(unsetenv("NO_COLOR") == 0);
    char path[] = "/tmp/tam-view-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0);

    test_watch(argv[1], path);
    test_pipe_follow(argv[1]);
    test_page_overlap(argv[1], path);
    test_short_screen(argv[1], path);
    test_pipe_page(argv[1]);

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
    expect(&t, 0, "02 two");
    expect(&t, 2, "04 four");
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
    send_key(&t, "g");
    expect(&t, 0, "tam-view keybindings guide");
    send_key(&t, "g");
    expect(&t, 0, "10 needle.[x]");
    send_key(&t, "h");
    expect(&t, 0, "tam-view keybindings guide");
    send_key(&t, "q");
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "10 needle.[x]") && contains(&t, 2, "12 twelve"));
    close_terminal(&t);

    write_file(path, "first\nsecond\nthird");
    t = start(argv[1], path, -1, -1);
    expect(&t, 3, "End of file (q quit)");
    CHECK(contains(&t, 0, "first") && contains(&t, 2, "third"));
    send_key(&t, "q");
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "first") && contains(&t, 2, "third"));
    close_terminal(&t);

    // -E / --quit-at-eof automatically returns to the shell at EOF.
    t = start_args(argv[1], "-E", path, -1, -1);
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "first") && contains(&t, 2, "third"));
    close_terminal(&t);

    write_file(path, "");
    t = start(argv[1], path, -1, -1);
    expect(&t, 3, "End of file (q quit)");
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);

    write_file(path, "first\nsecond\nthird\nfourth\nfifth\n");
    t = start(argv[1], path, -1, -1);
    expect(&t, 3, path);
    send_key(&t, " ");
    expect(&t, 0, "second");
    send_key(&t, " ");
    expect(&t, 3, "End of file (q quit)");
    send_key(&t, "q");
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
    expect(&t, 3, "End of file (q quit)");
    send_key(&t, "q");
    finish(&t, 0, 3);
    CHECK(contains(&t, 0, "last-2") && contains(&t, 2, "last"));
    close_terminal(&t);

    int stream[2];
    CHECK(pipe(stream) == 0);
    t = start(argv[1], NULL, stream[0], stream[1]);
    close(stream[0]);
    expect(&t, 3, "Waiting for input");
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
    expect(&t, 3, "Waiting for input");
    CHECK(contains(&t, 0, "a"));
    CHECK(write(stream[1], "b", 1) == 1);
    CHECK(close(stream[1]) == 0);
    expect(&t, 3, "End of file (q quit)");
    send_key(&t, "q");
    finish(&t, 0, 3);
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
    expect(&t, 4, "/tmp/tam-view-test-");
    CHECK(strstr(t.screen.cell[4], "1-4/8") != NULL);
    CHECK(strstr(t.screen.cell[4], "v" VIEW_VERSION) != NULL);
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
    send_key(&t, "1g");
    expect(&t, 0, "file1-a");
    send_key(&t, "2:x");
    expect(&t, 0, "file2-a");
    send_key(&t, ":f");
    expect(&t, 3, "file 2 of 2:");
    send_key(&t, " ");
    expect(&t, 3, "End of file (q quit)");
    send_key(&t, "18446744073709551615:n");
    expect(&t, 3, "No next file");
    send_key(&t, "q");
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

    // Render mode (-R), styling, C toggle, search highlight + Esc clear, and --no-color.
    write_file(path, "# Heading\n\n- bullet\n\nline3\nline4\n");
    t = start_args(argv[1], "-R", path, -1, -1);
    expect(&t, 0, "Heading");
    expect(&t, 1, "=======");
    CHECK(t.screen.styled[0][0]);
    CHECK(t.screen.sgr_count > 0);
    send_key(&t, "C");
    expect(&t, 3, "Color off");
    CHECK(!t.screen.styled[0][0]);
    send_key(&t, "C");
    expect(&t, 3, "Color on");
    CHECK(t.screen.styled[0][0]);
    send_key(&t, "R");
    expect(&t, 0, "# Heading");
    expect(&t, 3, "Raw mode");
    send_key(&t, "/Heading\r");
    expect_styled(&t, 0, 2, true);
    send_key(&t, "\033");
    expect(&t, 3, "Highlight cleared");
    CHECK(!t.screen.styled[0][2]);
    send_key(&t, "R");
    expect(&t, 0, "Heading");
    expect(&t, 3, "Render mode");
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);

    // --no-color suppresses all SGR output in interactive mode.
    t = start_args(argv[1], "--no-color", path, -1, -1);
    expect(&t, 0, "# Heading");
    send_key(&t, "/Heading\r");
    expect(&t, 0, "# Heading");
    CHECK(t.screen.sgr_count == 0);
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);

    // NO_COLOR=1 suppresses all SGR output by default.
    CHECK(setenv("NO_COLOR", "1", 1) == 0);
    t = start_args(argv[1], "-R", path, -1, -1);
    expect(&t, 0, "Heading");
    CHECK(t.screen.sgr_count == 0);
    CHECK(t.screen.osc8_open_count == 0);
    send_key(&t, "q");
    finish(&t, 0, 3);
    close_terminal(&t);
    CHECK(unsetenv("NO_COLOR") == 0);

    // 0.4.0: Attention line on Markdown/HTML, footer L<n> <top>-<bot>/<total> v0.4.0,
    // Right/Left link following & history, mock xdg-open, and OSC 8 on xterm-256color.
    {
        char dir[] = "/tmp/tam-view-links-XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        char md_path[256], html_path[256], mock_bin[256], mock_log[256];
        snprintf(md_path, sizeof(md_path), "%s/index.md", dir);
        snprintf(html_path, sizeof(html_path), "%s/other.html", dir);
        snprintf(mock_bin, sizeof(mock_bin), "%s/xdg-open", dir);
        snprintf(mock_log, sizeof(mock_log), "%s/opened.txt", dir);

        int mfd = open(md_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(mfd >= 0);
        const char *md_doc =
            "# Top\n\n"
            "Go [A](other.html#s%25202) [B](http://x.org) [C](no.md)\n\n"
            "Jump [Target](#target)\n\n"
            "## Target\n\n"
            "Reached target.\n";
        CHECK(write(mfd, md_doc, strlen(md_doc)) == (ssize_t)strlen(md_doc));
        CHECK(close(mfd) == 0);

        int hfd = open(html_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(hfd >= 0);
        const char *html_doc =
            "<html><body><h1>Other Page</h1><p>First</p>"
            "<h2 id=\"s%202\">Section Two <a href=\"no.html\">Bad</a></h2><p>Body</p></body></html>";
        CHECK(write(hfd, html_doc, strlen(html_doc)) == (ssize_t)strlen(html_doc));
        CHECK(close(hfd) == 0);

        int bfd = open(mock_bin, O_WRONLY | O_CREAT | O_TRUNC, 0755);
        CHECK(bfd >= 0);
        char script[512];
        snprintf(script, sizeof(script), "#!/bin/sh\nprintf '%%s\\n' \"$1\" > '%s'\n", mock_log);
        CHECK(write(bfd, script, strlen(script)) == (ssize_t)strlen(script));
        CHECK(close(bfd) == 0);

        const char *old_path = getenv("PATH");
        char new_path[4096];
        snprintf(new_path, sizeof(new_path), "%s:%s", dir, old_path ? old_path : "/usr/bin");
        CHECK(setenv("PATH", new_path, 1) == 0);

        t = start_env(argv[1], "--ascii", md_path, -1, -1, "xterm-256color");
        size(&t, 6, 54);
        expect(&t, 0, "Top");
        // Footer shows L1 and version v0.4.0.
        for (int i = 0; i < 60; i++) {
            if (strstr(t.screen.cell[5], "L1 ") && strstr(t.screen.cell[5], "v" VIEW_VERSION))
                break;
            drain(&t, 50);
        }
        CHECK(strstr(t.screen.cell[5], "L1 ") != NULL);
        CHECK(strstr(t.screen.cell[5], "v" VIEW_VERSION) != NULL);
        CHECK(t.screen.osc8_open_count > 0);
        CHECK(t.screen.osc8_open_count == t.screen.osc8_close_count);

        // Move attention line down to row 3 ("Go Local (other.html#sec%25202) or Ext (...)").
        send_key(&t, "\033[B\033[B\033[B");
        for (int i = 0; i < 60; i++) {
            if (strstr(t.screen.cell[5], "L4 "))
                break;
            drain(&t, 50);
        }
        CHECK(strstr(t.screen.cell[5], "L4 ") != NULL);

        // Right arrow follows first link on row ("other.html#s%25202") and lands on Section Two (L6).
        send_key(&t, "\033[C");
        expect(&t, 1, "Section Two");
        for (int i = 0; i < 60 && !strstr(t.screen.cell[5], "L6 "); i++)
            drain(&t, 50);
        CHECK(strstr(t.screen.cell[5], "L6 ") != NULL);

        // Following a broken link ("no.html") from other.html fails and rolls back its history push,
        // so Left still returns to index.md (Finding 7).
        send_key(&t, "\033[C");
        expect(&t, 5, "/tmp/tam-view-links-");
        send_key(&t, "\033[D");
        expect(&t, 0, "Top");

        // Down steps to second link on row ("http://x.org").
        // Resize preserves focus on the second link on the row (Finding 8).
        for (int i = 0; i < 60 && !strstr(t.screen.cell[5], "L4 "); i++)
            drain(&t, 50);
        send_key(&t, "\033[B");
        drain(&t, 50);
        size(&t, 6, 60);
        for (int i = 0; i < 60 && t.screen.cell[5][54] != 'v'; i++)
            drain(&t, 50);
        CHECK(t.screen.cell[5][54] == 'v');

        // First press arms external link; any other key cancels it; two presses invoke xdg-open (B3).
        send_key(&t, "\033[C");
        expect(&t, 5, "Press again to open: http://x.org");
        send_key(&t, "\033");
        for (int i = 0; i < 60 && !strstr(t.screen.cell[5], "L4 "); i++)
            drain(&t, 50);
        send_key(&t, "\033[C");
        expect(&t, 5, "Press again to open: http://x.org");
        send_key(&t, "\033[C");
        expect(&t, 5, "Started opener: http://x.org");

        // Verify mock xdg-open received exact URL.
        bool logged = false;
        for (int i = 0; i < 40; i++) {
            FILE *lf = fopen(mock_log, "r");
            if (lf) {
                char line[128] = {0};
                if (fgets(line, sizeof(line), lf) &&
                    strncmp(line, "http://x.org", 12) == 0) {
                    logged = true;
                }
                fclose(lf);
                if (logged)
                    break;
            }
            drain(&t, 25);
        }
        CHECK(logged);

        // Step to third link on row ("no.md"); failed open reports error (Finding 7).
        send_key(&t, "\033[B");
        send_key(&t, "\033[C");
        expect(&t, 5, "/tmp/tam-view-links-");

        send_key(&t, "q");
        finish(&t, 0, 5);
        close_terminal(&t);

        if (old_path)
            setenv("PATH", old_path, 1);
        unlink(mock_log);
        unlink(mock_bin);
        unlink(html_path);
        unlink(md_path);
        rmdir(dir);
    }

    {
        int input[2];
        CHECK(pipe(input) == 0);
        t = start_args(argv[1], "-R", NULL, input[0], input[1]);
        close(input[0]);
        CHECK(write(input[1], "# First\n", 8) == 8);
        expect(&t, 0, "First");
        expect(&t, 1, "=====");
        CHECK(!contains(&t, 0, "# First"));
        send_key(&t, "q");
        finish(&t, 0, 3);
        close(input[1]);
        close_terminal(&t);

        // B2: >64 KiB Markdown document piped in two halves separated by a pause.
        CHECK(pipe(input) == 0);
        t = start_args(argv[1], "-R", NULL, input[0], input[1]);
        close(input[0]);
        for (int p = 1; p <= 100; p++) {
            char para[512];
            int n = snprintf(para, sizeof(para),
                "Paragraph %d: lorem ipsum dolor sit amet, consectetur adipiscing elit. "
                "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
                "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris "
                "nisi ut aliquip ex ea commodo consequat duis aute irure dolor in "
                "reprehenderit in voluptate velit esse cillum dolore eu fugiat.\n\n",
                p);
            CHECK(write(input[1], para, (size_t)n) == n);
        }
        expect(&t, 0, "Paragraph 1:");
        for (int p = 101; p <= 199; p++) {
            char para[512];
            int n = snprintf(para, sizeof(para),
                "Paragraph %d: lorem ipsum dolor sit amet, consectetur adipiscing elit. "
                "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
                "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris "
                "nisi ut aliquip ex ea commodo consequat duis aute irure dolor in "
                "reprehenderit in voluptate velit esse cillum dolore eu fugiat.\n\n",
                p);
            CHECK(write(input[1], para, (size_t)n) == n);
        }
        const char *last_para = "Paragraph 200 final line.\n";
        CHECK(write(input[1], last_para, strlen(last_para)) == (ssize_t)strlen(last_para));
        close(input[1]);
        send_key(&t, "G");
        expect(&t, 2, "Paragraph 200 final line.");
        send_key(&t, "q");
        finish(&t, 0, 3);
        close_terminal(&t);

        // Auto-detect piped HTML in interactive mode without -R.
        CHECK(pipe(input) == 0);
        t = start_args(argv[1], "--ascii", NULL, input[0], input[1]);
        close(input[0]);
        const char *piped_html = "<!DOCTYPE html><html><body><h1>Piped HTML</h1></body></html>\n";
        CHECK(write(input[1], piped_html, strlen(piped_html)) == (ssize_t)strlen(piped_html));
        close(input[1]);
        expect(&t, 0, "Piped HTML");
        expect(&t, 1, "==========");
        send_key(&t, "q");
        finish(&t, 0, 3);
        close_terminal(&t);
    }
    CHECK(unlink(path) == 0);
    puts("PTY: ok");
    return 0;
}
