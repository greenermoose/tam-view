#include "view.h"

#include <tam_key.h>
#include <tam_tty.h>
#include <tam_uni.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ROWS 512
#define MAX_COLUMNS 4096
#define SEARCH_SIZE 512

enum job { IDLE, ADVANCE, GO_LINE, GO_END, SEARCH };
enum prompt { COMMAND, FIND_FORWARD, FIND_BACKWARD, HELP };

struct frame {
    struct position rows[MAX_ROWS];
    char *text;
    size_t stride;
    int count;
    bool valid;
    bool document;
};

struct search {
    unsigned char term[SEARCH_SIZE];
    size_t length;
    size_t prefix[SEARCH_SIZE];
    size_t matched;
    int direction;
    int original_direction;
    off_t hit;
};

struct pager {
    struct source *source;
    tam_tty_t tty;
    tam_key_parser_t keys;
    struct frame current;
    struct frame next;
    struct position top;
    struct position cursor;
    int height;
    int width;
    enum job job;
    enum prompt prompt;
    uint64_t count;
    uint64_t remaining;
    struct search search;
    char query[SEARCH_SIZE + 1];
    size_t query_length;
    const char *label;
    const char *message;
    bool waiting;
    bool dirty;
    bool scroll;
    bool quit;
    int result;
};

// Only the signal handler writes these flags and the wakeup byte.
static volatile sig_atomic_t wake_fd = -1;
static volatile sig_atomic_t interrupted;
static volatile sig_atomic_t resized;
static volatile sig_atomic_t suspended;

static void on_signal(int sig)
{
    int error = errno;
    if (sig == SIGWINCH)
        resized = 1;
    else if (sig == SIGTSTP)
        suspended = 1;
    else
        interrupted = sig;
    if (wake_fd >= 0) {
        unsigned char byte = 0;
        (void)write((int)wake_fd, &byte, 1);
    }
    errno = error;
}

static int signal_pipe(int fd[2])
{
    if (pipe(fd) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        if (fcntl(fd[i], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(fd[i], F_SETFD, FD_CLOEXEC) < 0)
            return -1;
    }
    wake_fd = fd[1];
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGWINCH, SIGTSTP, SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (sigaction(signals[i], &action, NULL) < 0)
            return -1;
    }
    return 0;
}

static int output(struct pager *p, const char *text)
{
    return tam_tty_write(&p->tty, text, strlen(text));
}

static int cursor_move(struct pager *p, int n, char direction)
{
    if (!n)
        return 0;
    char sequence[32];
    snprintf(sequence, sizeof(sequence), "\033[%d%c", n, direction);
    return output(p, sequence);
}

static off_t line_start(struct source *s, off_t p)
{
    while (p > 0) {
        int b = source_byte(s, p - 1);
        if (b == '\n')
            break;
        if (b < 0)
            return b == VIEW_ERROR ? -1 : p;
        p--;
    }
    return p;
}

// Walk only the physical lines needed for this movement. For a wrapped
// line, a ring keeps the last requested row starts in bounded memory.
static int back(struct pager *p, struct position end, int count,
                struct position *out)
{
    struct position starts[MAX_ROWS];
    while ((end.byte || end.tab_left) && count > 0) {
        off_t offset = line_start(p->source, end.byte);
        if (offset < 0)
            return -1;
        if (offset == end.byte && end.tab_left == 0 && offset > 0) {
            offset = line_start(p->source, offset - 1);
            if (offset < 0)
                return -1;
        }

        struct position row = {.byte = offset};
        int next_slot = 0;
        int saved = 0;
        while (position_compare(row, end) < 0) {
            struct position next;
            int result = view_row(p->source, row, p->width, NULL, &next);
            if (result == VIEW_ERROR)
                return -1;
            if (result != VIEW_ROW)
                break;
            starts[next_slot] = row;
            next_slot = (next_slot + 1) % count;
            if (saved < count)
                saved++;
            row = next;
        }
        if (saved == count) {
            *out = starts[next_slot];
            return 0;
        }
        count -= saved;
        end = (struct position){.byte = offset};
    }
    *out = end;
    return 0;
}

static int align(struct pager *p, struct position target, struct position *out)
{
    off_t offset = line_start(p->source, target.byte);
    if (offset < 0)
        return -1;
    struct position row = {.byte = offset};
    while (position_compare(row, target) < 0) {
        struct position next;
        int result = view_row(p->source, row, p->width, NULL, &next);
        if (result == VIEW_ERROR)
            return -1;
        if (result != VIEW_ROW || position_compare(next, target) > 0)
            break;
        row = next;
    }
    *out = row;
    return 0;
}

static int size_view(struct pager *p)
{
    int rows, cols;
    tam_tty_size(&p->tty, &rows, &cols);
    int height = rows > 1 ? rows - 1 : 1;
    p->height = height < MAX_ROWS ? height : MAX_ROWS;
    p->width = cols < MAX_COLUMNS ? cols : MAX_COLUMNS;
    size_t stride = view_row_capacity(p->width);
    char *text = realloc(p->next.text, (size_t)p->height * stride);
    if (!text)
        return -1;
    p->next.text = text;
    p->next.stride = stride;
    return 0;
}

static int build_page(struct pager *p, struct position *end)
{
    struct frame *f = &p->next;
    f->count = 0;
    f->document = p->prompt != HELP;
    f->valid = true;
    *end = p->top;
    if (!f->document) {
        static const char *const help[] = {
            "tam-view keys",
            "j / Down / Enter       Next line",
            "k / Up                 Previous line",
            "Space / Page Down      Next page",
            "b / Page Up            Previous page",
            "d / u                  Half page down / up",
            "g / G / Ng             Start / end / line N",
            "/ / ?                  Find literal text",
            "n / N                  Repeat / reverse search",
            "q                      Quit and keep the page"
        };
        for (size_t i = 0; i < sizeof(help) / sizeof(help[0]) &&
                           f->count < p->height; i++, f->count++) {
            view_label(f->text + f->count * f->stride, f->stride,
                       p->width, help[i]);
        }
        return VIEW_ROW;
    }

    for (; f->count < p->height; f->count++) {
        struct position row = *end;
        f->rows[f->count] = row;
        int result = view_row(p->source, row, p->width,
                              f->text + f->count * f->stride, end);
        if (result != VIEW_ROW)
            return result;
    }
    if (end->tab_left)
        return VIEW_ROW;
    int b = source_byte(p->source, end->byte);
    return b < 0 ? b : VIEW_ROW;
}

static void status(struct pager *p, int page_state, char *out, size_t cap)
{
    const char *message = p->message;
    char text[SEARCH_SIZE + 80];
    if (p->prompt == HELP) {
        message = "Press any key to return";
    } else if (p->prompt == FIND_FORWARD || p->prompt == FIND_BACKWARD) {
        snprintf(text, sizeof(text), "%c%s",
                 p->prompt == FIND_FORWARD ? '/' : '?', p->query);
        message = text;
    } else if (p->job == SEARCH) {
        message = "Searching... (Ctrl-C cancels)";
    } else if (!message && (page_state == VIEW_WAIT || p->job != IDLE)) {
        message = "Waiting for input... (q quits)";
    } else if (!message && p->count) {
        snprintf(text, sizeof(text), "%ju", (uintmax_t)p->count);
        message = text;
    } else if (!message) {
        char name[SEARCH_SIZE + 1];
        view_label(name, sizeof(name), p->width, p->label);
        snprintf(out, cap, "%s", name);
        return;
    }
    if (p->prompt == FIND_FORWARD || p->prompt == FIND_BACKWARD) {
        // Keep the editing cursor visible on narrow terminals.
        int width = p->width > 1 ? p->width - 1 : 1;
        size_t len = strlen(message);
        const char *tail = message + (len > (size_t)width ? len - (size_t)width : 0);
        while ((*tail & 0xc0) == 0x80)
            tail++;
        view_label(out, cap, width, tail);
    } else {
        view_label(out, cap, p->width, message);
    }
}

static int paint(struct pager *p)
{
    struct position end;
    int state = build_page(p, &end);
    if (state == VIEW_ERROR)
        return -1;
    bool final = state == VIEW_EOF && p->next.document;
    if (final && p->next.count < p->height && (p->top.byte || p->top.tab_left)) {
        if (p->next.count == 0 && p->top.byte > 0) {
            off_t size = source_size(p->source);
            if (size < 0)
                return -1;
            if (end.byte > size)
                end = (struct position){.byte = size};
        }
        if (back(p, end, p->height, &p->top) < 0 ||
            build_page(p, &end) == VIEW_ERROR)
            return -1;
    }

    struct frame *old = &p->current;
    struct frame *f = &p->next;
    int first = 0;
    bool scrolling = old->valid && old->document && f->document &&
                     old->count && f->count &&
                     (position_equal(f->rows[0], old->rows[0]) ||
                      (p->scroll && position_compare(f->rows[0], old->rows[0]) > 0));
    if (scrolling) {
        for (int i = 0; i < old->count; i++) {
            if (position_equal(old->rows[i], f->rows[0])) {
                first = old->count - i;
                break;
            }
        }
        if (first > f->count)
            scrolling = false;
    }
    if (!scrolling) {
        first = 0;
        if (old->valid && cursor_move(p, old->count, 'A') < 0)
            return -1;
    }

    for (int i = first; i < f->count; i++) {
        if (output(p, "\r\033[K") < 0 ||
            output(p, f->text + i * f->stride) < 0 ||
            output(p, "\r\n") < 0)
            return -1;
    }

    if (old->valid || f->count || !final) {
        if (output(p, "\r\033[K") < 0)
            return -1;
    }
    if (!scrolling && old->valid) {
        int extra = old->count - f->count;
        for (int i = 0; i < extra; i++) {
            if (output(p, "\033[B\r\033[K") < 0)
                return -1;
        }
        if (extra > 0 && cursor_move(p, extra, 'A') < 0)
            return -1;
    }

    if (!final && !p->quit) {
        char line[SEARCH_SIZE + 80];
        status(p, state, line, sizeof(line));
        if (output(p, line) < 0)
            return -1;
    }
    memcpy(old->rows, f->rows, (size_t)f->count * sizeof(f->rows[0]));
    old->count = f->count;
    old->valid = true;
    old->document = f->document;

    p->dirty = false;
    p->scroll = false;
    p->waiting = state == VIEW_WAIT;
    if (final) {
        old->valid = false; // The prompt line is already clear.
        p->quit = true;
    }
    return 0;
}

static unsigned char pattern_byte(const struct search *s, size_t index)
{
    return s->term[s->direction > 0 ? index : s->length - 1 - index];
}

static void start_search(struct pager *p, int direction, bool repeat)
{
    struct search *s = &p->search;
    p->message = NULL;
    if (!s->length) {
        p->message = "No previous search";
        return;
    }
    s->direction = direction;
    s->matched = 0;
    s->prefix[0] = 0;
    for (size_t i = 1, j = 0; i < s->length; i++) {
        while (j && pattern_byte(s, i) != pattern_byte(s, j))
            j = s->prefix[j - 1];
        if (pattern_byte(s, i) == pattern_byte(s, j))
            j++;
        s->prefix[i] = j;
    }

    off_t anchor = repeat && s->hit >= 0 ? s->hit : p->top.byte;
    // In a reverse repeat, allow an overlapping hit that starts earlier.
    if (repeat && s->hit >= 0)
        anchor += direction > 0 ? 1 : (off_t)s->length - 2;
    else if (direction < 0)
        anchor--;
    p->cursor = (struct position){.byte = anchor};
    p->job = SEARCH;
}

static int last_page(struct pager *p, struct position end)
{
    p->job = IDLE;
    p->dirty = true;
    return back(p, end, p->height, &p->top);
}

static int step_job(struct pager *p)
{
    struct source *s = p->source;
    if (p->job == GO_END) {
        if (!s->eof)
            return VIEW_WAIT;
        off_t size = source_size(s);
        if (size < 0)
            return VIEW_ERROR;
        if (size || s->spool)
            return last_page(p, (struct position){.byte = size}) < 0
                       ? VIEW_ERROR : VIEW_ROW;
        // Some seekable pseudo-files report size zero despite having content.
    }

    if (p->job == ADVANCE) {
        for (; p->remaining; p->remaining--) {
            struct position next;
            int result = view_row(s, p->cursor, p->width, NULL, &next);
            if (result == VIEW_EOF)
                return last_page(p, p->cursor) < 0 ? VIEW_ERROR : VIEW_ROW;
            if (result != VIEW_ROW)
                return result;
            p->cursor = next;
        }
        if (p->cursor.tab_left == 0) {
            int next = source_byte(s, p->cursor.byte);
            if (next == VIEW_EOF)
                return last_page(p, p->cursor) < 0 ? VIEW_ERROR : VIEW_ROW;
            if (next < 0)
                return next;
        }
        p->top = p->cursor;
        p->scroll = true;
        p->dirty = true;
        p->job = IDLE;
        return VIEW_ROW;
    }

    for (int i = 0; i < VIEW_BLOCK; i++) {
        off_t offset = p->cursor.byte;
        int b = offset < 0 ? VIEW_EOF : source_byte(s, offset);
        if (b == VIEW_WAIT || b == VIEW_ERROR)
            return b;
        if (p->job == SEARCH) {
            struct search *find = &p->search;
            if (b == VIEW_EOF) {
                p->message = "Pattern not found";
                break;
            }
            while (find->matched && b != pattern_byte(find, find->matched))
                find->matched = find->prefix[find->matched - 1];
            if (b == pattern_byte(find, find->matched))
                find->matched++;
            if (find->matched == find->length) {
                find->hit = find->direction > 0
                            ? offset - (off_t)find->length + 1 : offset;
                struct position hit = {.byte = find->hit};
                if (align(p, hit, &p->top) < 0)
                    return VIEW_ERROR;
                break;
            }
            p->cursor.byte += find->direction;
        } else {
            if (b == VIEW_EOF)
                return last_page(p, p->cursor) < 0 ? VIEW_ERROR : VIEW_ROW;
            p->cursor.byte++;
            if (p->job == GO_LINE && b == '\n' && --p->remaining == 0) {
                p->top = p->cursor;
                break;
            }
        }
        if (i == VIEW_BLOCK - 1)
            return VIEW_ROW;
    }
    p->job = IDLE;
    p->dirty = true;
    return VIEW_ROW;
}

static void edit_query(struct pager *p, tam_key_event_t key)
{
    if (key.type == TAM_KEY_ESC ||
        (key.type == TAM_KEY_CHAR && key.mods == TAM_MOD_CTRL &&
         key.codepoint == 'c')) {
        p->prompt = COMMAND;
    } else if (key.type == TAM_KEY_ENTER) {
        int direction = p->prompt == FIND_FORWARD ? 1 : -1;
        if (p->query_length) {
            memcpy(p->search.term, p->query, p->query_length);
            p->search.length = p->query_length;
        }
        p->search.original_direction = direction;
        p->search.hit = -1;
        p->prompt = COMMAND;
        start_search(p, direction, false);
    } else if (key.type == TAM_KEY_BACKSPACE && p->query_length) {
        do {
            p->query_length--;
        } while (p->query_length &&
                 (p->query[p->query_length] & 0xc0) == 0x80);
    } else if (key.type == TAM_KEY_CHAR && !key.mods && key.codepoint >= 32) {
        char bytes[4];
        size_t n = tam_utf8_encode(key.codepoint, bytes);
        if (n <= SEARCH_SIZE - p->query_length) {
            memcpy(p->query + p->query_length, bytes, n);
            p->query_length += n;
        }
    }
    p->query[p->query_length] = '\0';
}

static int handle_key(struct pager *p, tam_key_event_t key)
{
    p->dirty = true;
    int c = key.type == TAM_KEY_CHAR && !key.mods && key.codepoint < 128
            ? (int)key.codepoint : 0;
    bool ctrl_c = key.type == TAM_KEY_CHAR && key.mods == TAM_MOD_CTRL &&
                  key.codepoint == 'c';
    if (key.type == TAM_KEY_CHAR && key.mods == TAM_MOD_CTRL &&
        key.codepoint == 'z') {
        suspended = 1;
        return 0;
    }
    if (p->prompt == HELP) {
        p->prompt = COMMAND;
        p->quit = c == 'q';
        return 0;
    }
    if (p->prompt != COMMAND) {
        edit_query(p, key);
        return 0;
    }
    if (c == 'q') {
        p->quit = true;
        return 0;
    }
    if (ctrl_c || key.type == TAM_KEY_ESC) {
        if (p->job != IDLE) {
            p->job = IDLE;
            p->message = "Canceled";
        }
        p->count = 0;
        return 0;
    }
    if (p->job != IDLE)
        return 0;

    p->message = NULL;
    if (c >= '0' && c <= '9') {
        if (p->count <= (UINT64_MAX - (unsigned)(c - '0')) / 10)
            p->count = p->count * 10 + (unsigned)(c - '0');
        return 0;
    }

    uint64_t count = p->count;
    p->count = 0;
    int movement = 0;
    if (key.type == TAM_KEY_DOWN || key.type == TAM_KEY_ENTER || c == 'j')
        movement = 1;
    else if (key.type == TAM_KEY_UP || c == 'k')
        movement = -1;
    else if (key.type == TAM_KEY_PAGE_DOWN || c == ' ')
        movement = p->height;
    else if (key.type == TAM_KEY_PAGE_UP || c == 'b')
        movement = -p->height;
    else if (c == 'd')
        movement = (p->height + 1) / 2;
    else if (c == 'u')
        movement = -(p->height + 1) / 2;
    else if (c == 'g' || key.type == TAM_KEY_HOME) {
        p->search.hit = -1;
        p->cursor = (struct position){0};
        if (count > 1 && c == 'g') {
            p->remaining = count - 1;
            p->job = GO_LINE;
        } else {
            p->top = p->cursor;
        }
    } else if (c == 'G' || key.type == TAM_KEY_END) {
        p->cursor = (struct position){0};
        p->job = GO_END;
    } else if (c == '/' || c == '?') {
        p->query_length = 0;
        p->query[0] = '\0';
        p->prompt = c == '/' ? FIND_FORWARD : FIND_BACKWARD;
    } else if (c == 'n' || c == 'N') {
        start_search(p, c == 'n' ? p->search.original_direction
                                 : -p->search.original_direction, true);
    } else if (c == 'h') {
        p->prompt = HELP;
    }

    if (movement) {
        if (movement < 0 && back(p, p->top, -movement, &p->top) < 0)
            return -1;
        if (movement > 0) {
            p->cursor = p->top;
            p->remaining = (unsigned)movement;
            p->job = ADVANCE;
        }
        p->search.hit = -1;
    }
    return 0;
}

static int refresh_size(struct pager *p)
{
    if (p->current.valid && output(p, "\r\033[K\r\n") < 0)
        return -1;
    p->current.valid = false;
    p->dirty = true;
    if (p->job == ADVANCE)
        p->job = IDLE;
    if (size_view(p) < 0)
        return -1;
    return align(p, p->top, &p->top);
}

static int suspend_view(struct pager *p)
{
    if (p->current.valid && output(p, "\r\033[K") < 0)
        return -1;
    if (tam_tty_raw(&p->tty, false) < 0)
        return -1;
    if (raise(SIGSTOP) != 0)
        return -1;
    if (interrupted)
        return 0;
    if (tam_tty_raw(&p->tty, true) < 0)
        return -1;
    tam_key_init(&p->keys);
    return refresh_size(p);
}

static int poll_keys(struct pager *p)
{
    unsigned char bytes[TAM_KEY_BUF_SIZE];
    size_t room = sizeof(bytes) - tam_key_pending(&p->keys);
    if (!room) {
        tam_key_init(&p->keys);
        room = sizeof(bytes);
    }
    ssize_t count = read(p->tty.in_fd, bytes, room);
    if (count < 0 && errno == EINTR)
        return 0;
    if (count == 0) {
        p->quit = true;
        return 0;
    }
    if (count < 0)
        return -1;
    if (tam_key_feed(&p->keys, bytes, (size_t)count) < 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int run(struct pager *p, int wake)
{
    while (!p->quit) {
        if (interrupted) {
            p->result = 128 + interrupted;
            break;
        }
        if (suspended) {
            suspended = 0;
            if (suspend_view(p) < 0)
                return -1;
            continue;
        }
        if (resized) {
            resized = 0;
            if (refresh_size(p) < 0)
                return -1;
        }

        int job_state = VIEW_ROW;
        if (p->job != IDLE) {
            job_state = step_job(p);
            if (job_state == VIEW_ERROR)
                return -1;
        }
        if (p->dirty && paint(p) < 0)
            return -1;
        if (p->quit)
            break;

        tam_key_event_t key;
        int ready = tam_key_next(&p->keys, &key);
        if (ready > 0) {
            if (handle_key(p, key) < 0)
                return -1;
            continue;
        }
        if (ready < 0)
            tam_key_init(&p->keys);

        bool pump = !p->source->eof &&
                    (p->waiting || job_state == VIEW_WAIT);
        struct pollfd fds[] = {
            {.fd = wake, .events = POLLIN},
            {.fd = p->tty.in_fd, .events = POLLIN},
            {.fd = pump ? p->source->input : -1, .events = POLLIN}
        };
        bool scanning = p->job != IDLE && job_state != VIEW_WAIT;
        int timeout = scanning ? 0 : tam_key_pending(&p->keys) ? 50 : -1;
        int n = poll(fds, sizeof(fds) / sizeof(fds[0]), timeout);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (n == 0 && !scanning) {
            int got_key = tam_key_timeout(&p->keys, &key);
            tam_key_init(&p->keys);
            if (got_key > 0 && handle_key(p, key) < 0)
                return -1;
        }
        if (fds[0].revents) {
            char bytes[64];
            while (read(wake, bytes, sizeof(bytes)) > 0)
                continue;
            continue;
        }
        if (fds[1].revents) {
            if (poll_keys(p) < 0)
                return -1;
            continue;
        }
        if (fds[2].revents) {
            if (source_pump(p->source) < 0)
                return -1;
            if (p->waiting) {
                p->dirty = true;
                p->scroll = true;
            }
        }
    }

    if (p->current.valid && !p->current.document) {
        p->prompt = COMMAND;
        p->quit = true;
        if (paint(p) < 0)
            return -1;
    } else if (p->current.valid && output(p, "\r\033[K") < 0) {
        return -1;
    }
    return p->result;
}

int view_interactive(struct source *s, int tty, const char *label)
{
    struct pager p = {.source = s, .label = label, .dirty = true};
    p.search.hit = -1;
    int wake[2] = {-1, -1};
    int result = -1;
    if (tam_tty_open(&p.tty, tty, STDOUT_FILENO) < 0 ||
        !p.tty.interactive)
        return view_dump(s, 80);

    // Job control belongs to the shell. A background pager must stop before
    // changing the settings on its controlling terminal.
    for (;;) {
        pid_t foreground = tcgetpgrp(tty);
        if (foreground == getpgrp())
            break;
        if (foreground < 0)
            return -1;
        if (raise(SIGTTIN) != 0)
            return -1;
    }

    if (size_view(&p) == 0 && signal_pipe(wake) == 0 &&
        tam_tty_raw(&p.tty, true) == 0) {
        result = run(&p, wake[0]);
        if (result < 0 && p.current.valid) {
            int error = errno;
            (void)output(&p, "\r\033[K");
            errno = error;
        }
    }

    int error = errno;
    if (tam_tty_raw(&p.tty, false) < 0 && result == 0) {
        result = -1;
        error = errno;
    }
    tam_tty_close(&p.tty);
    wake_fd = -1;
    for (int i = 0; i < 2; i++)
        if (wake[i] >= 0)
            close(wake[i]);
    free(p.next.text);
    errno = error;
    return result;
}
