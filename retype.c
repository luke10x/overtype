/*
 * gcc -Wall -o retype retype.c -lutf8proc -lunistring $(ncursesw5-config --cflags --libs)
 * indent -kr -ts4 -nut -l80 *.c
 * apt install libncursesw5-dev libunistring-dev libutf8proc-dev
 */

#include <stdio.h>
#include <stdint.h>
#include <locale.h>
#include <curses.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>
#include <inttypes.h>
#include <wchar.h>

#include <unitypes.h>
#include <uniconv.h>
#include <unistdio.h>
#include <unistr.h>
#include <uniwidth.h>
#include <utf8proc.h>

#define GREY_PAIR    5
#define GOOD_PAIR    6
#define ERROR_PAIR    7

#define MAX_LEN 81
#define MAX_LINES 99999

int get_padding(int longest_line, int term_cols);

void print_grey(const int row, const int col, const char *line);

void re_type(const int row, const int col, const char *line,
             const int offset, const struct winsize w);
static void sig_handler(int sig);

WINDOW *pad;

FILE *logger;

size_t smartlen(const char *line)
{
    size_t len = 0;
    ucs4_t _;
    for (const uint8_t * it = (const uint8_t *)line; it; it = u8_next(&_, it))
        len++;
    len--;                      // Because it is NUL terminated string
    return len;
}

int main(void)
{
    signal(SIGWINCH, sig_handler);

    int longest_line = 0;

    char ch_arr[MAX_LINES][MAX_LEN];

    FILE *fp;
    // fp = fopen("teve-musu.txt", "r");
    fp = stdin;

    logger = fopen("retype.log", "a+");

    if (fp == NULL) {
        perror("Failed: ");
        return 1;
    }

    char buffer[MAX_LEN];
    int i = 0;
    while (fgets(buffer, MAX_LEN - 1, fp)) {
        // Remove trailing newline
        buffer[strcspn(buffer, "\n")] = 0;
        strcpy(ch_arr[i], buffer);
        i++;
        if (i > 32767) {
            perror("Failed file too long");
            exit(1);
        }

        if (strlen(buffer) > longest_line) {
            longest_line = strlen(buffer);
        }
    }

    fclose(fp);

    int lines_total = i;

    freopen("/dev/tty", "rw", stdin);
    setlocale(LC_ALL, "");

    initscr();
    struct winsize w;
    ioctl(0, TIOCGWINSZ, &w);

    int padding = get_padding(longest_line, w.ws_col);
    keypad(stdscr, TRUE);
    cbreak();
    noecho();

    /*
     * initialize colors 
     */
    if (has_colors() == FALSE) {
        endwin();
        printf("Your terminal does not support color\n");
        exit(1);
    }

    start_color();
    init_pair(GREY_PAIR, COLOR_WHITE, COLOR_BLACK);
    init_pair(ERROR_PAIR, COLOR_RED + 8, COLOR_YELLOW + 8);
    init_pair(GOOD_PAIR, COLOR_GREEN + 8, COLOR_BLACK);
    clear();

    pad = newpad(lines_total, w.ws_col);

    const int lines_done = 0;

    for (i = 0; i < lines_done; i++) {
        print_grey(i, padding, ch_arr[i]);
    }

    time_t start_time;
    time_t end_time;

    start_time = time(NULL);
    for (i = lines_done; i < lines_total; i++) {
        print_grey(i, padding, ch_arr[i]);
        const int offset = (i < w.ws_row) ? 0 : i - w.ws_row + 1;

        refresh();
        prefresh(pad, offset, 0, 0, 0, w.ws_row - 1, w.ws_col - 1);

        wmove(pad, offset, padding);

        refresh();
        prefresh(pad, offset, 0, 0, 0, w.ws_row - 1, w.ws_col - 1);

        re_type(i, padding, ch_arr[i], offset, w);
    }
    end_time = time(NULL);

    const double diff_t = difftime(end_time, start_time);
    printf("Execution time = %f\n", diff_t);

    getch();
    endwin();
    exit(0);
}

int get_padding(int longest_line, int term_cols)
{
    return (term_cols - longest_line) / 2;
}

void print_grey(const int row, const int col, const char *line)
{
    wmove(pad, row, col);
    attron(COLOR_PAIR(GREY_PAIR));
    waddstr(pad, line);

    attroff(COLOR_PAIR(GREY_PAIR));
}

struct charstack {
    wint_t ch;
    struct charstack *next;
};

typedef uint8_t utf8_t;

uint8_t normalize(const uint32_t c)
{
    uint8_t *output;

    char *input_ = malloc(sizeof(const uint32_t) + 1);
    sprintf(input_, "%lc", c);

    const int first_ch_size = strlen(input_);   //u8_mblen(&c, sizeof(uint32_t));

    utf8proc_map((unsigned char *) input_, first_ch_size, &output,
                 UTF8PROC_DECOMPOSE | UTF8PROC_NULLTERM | UTF8PROC_STABLE |
                 UTF8PROC_STRIPMARK | UTF8PROC_CASEFOLD);

    return *output;
}

int isssame(const uint32_t expected_ch, int typed_ch)
{
    uint8_t expected_nch = normalize(expected_ch);
    uint8_t typed_nch = normalize(typed_ch);

    fwprintf(logger, L"%lc:%d(%d) == %lc:%d(%d) \n",
             expected_ch, expected_ch, expected_nch,
             typed_ch, typed_ch, typed_nch);

    return (expected_nch == typed_nch);
}

const uint32_t simplify(const char *input, const int index)
{
    const uint32_t *mbcs = u32_strconv_from_locale(input);

    const uint32_t unicode = (int) *(&mbcs[index]);
    return unicode;
}

int this_is_lnumber_start(const char *line, int typed)
{
    const uint32_t expected_ch = simplify(line, typed);
    if (expected_ch >= 0x0030 && expected_ch <= 0x0039) {
        return true;
    }
    return false;
}

int this_is_subsequent_space(const char *line, int typed)
{
    if (typed == 0)
        return false;
    const uint32_t previous_ch = simplify(line, typed - 1);
    const uint32_t expected_ch = simplify(line, typed);
    if (previous_ch == 0x0020 && expected_ch == 0x0020) {
        return true;
    }
    return false;
}

bool should_autotext(int now_started, const char *line, int typed,
                     struct charstack *undostack)
{
    if (undostack != 0) {
        return false;
    }
    if (now_started) {
        const uint32_t expected_ch = simplify(line, typed);
        if (expected_ch == 0x0020 ||
            expected_ch == 0x003A ||
            (expected_ch >= 0x0030 && expected_ch <= 0x0039)
            ) {
            return true;
        }
    } else {
        if (this_is_lnumber_start(line, typed)) {
            return true;
        }
        if (this_is_subsequent_space(line, typed)) {
            return true;
        }
    }
    return false;
}

void re_type(const int row, const int col, const char *line,
             const int offset, const struct winsize w)
{
    struct charstack *undostack = NULL;
    wmove(pad, row, col);
    refresh();
    prefresh(pad, offset, 0, 0, 0, w.ws_row - 1, w.ws_col - 1);

    int typed = 0;

    bool autotext_started = false;
    do {
        const uint32_t expected_ch = simplify(line, typed);
        // fwprintf(logger, L"%d. %lsc(%d) TEST: %ls(%d)\n", typed,
        // expected_ch, (int)expected_ch);
        fwprintf(logger, L"----- \n");
        fwprintf(logger, L"%d. TEST %lc(%d) \n", typed, expected_ch,
                 expected_ch);

        autotext_started =
            should_autotext(autotext_started, line, typed, undostack);
        wint_t typed_ch = autotext_started ? expected_ch : getwchar();

        if (typed_ch == '\t') {
            typed_ch = 0x20;
        }
        // ANY control char
        if (typed_ch == 27) {
            continue;
        }
        // ENTER key
        if (typed_ch == 13) {

            fwprintf(logger, L"typed: %d\n", typed);
            fwprintf(logger, L"normal length: %zu\n", strlen(line));

            const size_t i = smartlen(line);

            fwprintf(logger, L"smarte length: %zu\n", i);
            if (typed < i || undostack != NULL) {
                continue;
            }
            break;
        }

        if (typed_ch == 127) {
            if (undostack == NULL) {
                continue;
            }
            int x, y;
            getyx(pad, y, x);

            // Pop from undo stack
            struct charstack *temp;
            temp = undostack;
            undostack = undostack->next;

            wattron(pad, COLOR_PAIR(GREY_PAIR));
            wmove(pad, y, x - 1);
            waddch(pad, temp->ch);
            wattroff(pad, COLOR_PAIR(GREY_PAIR));

            typed--;

            // Again we need to go back 
            // becuase we just printed the popped char
            wmove(pad, y, x - 1);
            refresh();
            prefresh(pad, offset, 0, 0, 0, w.ws_row - 1, w.ws_col - 1);
            continue;
        }

        const int same = isssame(expected_ch, typed_ch);
        // const char testch = winch(pad) & A_CHARTEXT;
        if (same && undostack == NULL) {

            wattron(pad, COLOR_PAIR(GOOD_PAIR));
            char str[80];
            sprintf(str, "%lc", expected_ch);
            waddstr(pad, str);
            wattroff(pad, COLOR_PAIR(GOOD_PAIR));

            refresh();
            prefresh(pad, offset, 0, 0, 0, w.ws_row - 1, w.ws_col - 1);
        } else {

            const wint_t good_ch = (typed < smartlen(line)) ? expected_ch : ' ';
            struct charstack *nptr = malloc(sizeof(struct charstack));
            nptr->ch = good_ch;
            nptr->next = undostack;
            undostack = nptr;

            wattron(pad, COLOR_PAIR(ERROR_PAIR));
            char str[80];
            sprintf(str, "%lc", typed_ch);
            waddstr(pad, str);
            wattroff(pad, COLOR_PAIR(ERROR_PAIR));

            refresh();
            prefresh(pad, offset, 0, 0, 0, w.ws_row - 1, w.ws_col - 1);
        }
        typed++;
    }
    while (true);
}

static void sig_handler(int sig)
{
    if (SIGWINCH == sig) {
        struct winsize winsz;

        ioctl(0, TIOCGWINSZ, &winsz);
        printf("SIGWINCH raised, window size: %d rows / %d columns\n",
               winsz.ws_row, winsz.ws_col);
    }
}
