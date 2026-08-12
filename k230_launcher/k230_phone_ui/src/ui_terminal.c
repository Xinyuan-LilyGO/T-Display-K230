#define _GNU_SOURCE

#include "ui_terminal.h"

#include "ui_common.h"
#include "ui_hardware.h"
#include "ui_i18n.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "tmt.h"

#define TERMINAL_LOG_PATH "/tmp/k230_terminal.log"
#define TERMINAL_ROWS 32U
#define TERMINAL_COLS 72U
#define TERMINAL_ROW_TEXT_MAX (TERMINAL_COLS * 4U + 8U)
#define TERMINAL_PENDING_MAX 8192U
#define TERMINAL_TIMER_MS 35
#define TERMINAL_SHELL "/bin/sh"

#ifndef LV_KEYBOARD_CTRL_BUTTON_MODE_TEXT_LOWER
#define LV_KEYBOARD_CTRL_BUTTON_MODE_TEXT_LOWER "abc"
#endif
#ifndef LV_KEYBOARD_CTRL_BUTTON_MODE_TEXT_UPPER
#define LV_KEYBOARD_CTRL_BUTTON_MODE_TEXT_UPPER "ABC"
#endif
#ifndef LV_KEYBOARD_CTRL_BUTTON_MODE_SPECIAL
#define LV_KEYBOARD_CTRL_BUTTON_MODE_SPECIAL "1#"
#endif

typedef enum {
    TERMINAL_KEY_CTRL = 0,
    TERMINAL_KEY_ESC,
    TERMINAL_KEY_TAB,
    TERMINAL_KEY_ENTER,
    TERMINAL_KEY_C,
    TERMINAL_KEY_D,
    TERMINAL_KEY_L,
    TERMINAL_KEY_UP,
    TERMINAL_KEY_DOWN,
    TERMINAL_KEY_LEFT,
    TERMINAL_KEY_RIGHT,
    TERMINAL_KEY_BS,
    TERMINAL_KEY_CLEAR,
    TERMINAL_KEY_RESTART,
    TERMINAL_KEY_FONT_DOWN,
    TERMINAL_KEY_FONT_UP,
} terminal_key_t;

typedef enum {
    TERMINAL_KBD_LOWER = 0,
    TERMINAL_KBD_UPPER,
    TERMINAL_KBD_NUM,
    TERMINAL_KBD_SYMBOL,
} terminal_keyboard_mode_t;

typedef struct {
    const lv_font_t *font;
    int line_height;
    const char *name;
} terminal_font_zoom_t;

static const terminal_font_zoom_t terminal_font_zoom[] = {
    { &lv_font_montserrat_12, 16, "Small" },
    { &lv_font_montserrat_14, 18, "Medium" },
    { &lv_font_montserrat_16, 20, "Large" },
};

static const char *const terminal_kbd_lower_map[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "123", "ABC", "z", "x", "c", "v", "b", "n", "m", "Space", "BS", "Enter", ""
};

static const char *const terminal_kbd_upper_map[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "123", "abc", "Z", "X", "C", "V", "B", "N", "M", "Space", "BS", "Enter", ""
};

static const char *const terminal_kbd_num_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
    "abc", "#+=", ".", ",", "?", "!", "'", "Space", "BS", "Enter", ""
};

static const char *const terminal_kbd_symbol_map[] = {
    "[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
    "_", "\\", "|", "~", "<", ">", "`", ".", ",", "?", "\n",
    "abc", "123", "-", "/", ":", ";", "!", "Space", "BS", "Enter", ""
};

static pthread_mutex_t terminal_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t terminal_reader_thread;
static int terminal_reader_started;
static volatile int terminal_reader_stop;
static int terminal_master_fd = -1;
static pid_t terminal_child_pid = -1;
static TMT *terminal_tmt;
static unsigned char terminal_pending[TERMINAL_PENDING_MAX];
static size_t terminal_pending_len;
static int terminal_dirty;
static int terminal_ctrl_armed;
static int terminal_cursor_visible = 1;
static int terminal_font_level = 0;
static terminal_keyboard_mode_t terminal_keyboard_mode = TERMINAL_KBD_LOWER;
static char terminal_last_rows[TERMINAL_ROWS][TERMINAL_ROW_TEXT_MAX];

static lv_obj_t *terminal_output_box;
static lv_obj_t *terminal_row_labels[TERMINAL_ROWS];
static lv_obj_t *terminal_status_label;
static lv_obj_t *terminal_keyboard;
static lv_obj_t *terminal_ctrl_btn;
static lv_timer_t *terminal_timer;
static int terminal_use_hardware_keyboard;

static int terminal_default_row_width(void)
{
    int w = ui_screen_width() - 44;

    return w > 120 ? w : 120;
}

static void terminal_stop_shell(void);
static int terminal_spawn_shell(void);

static int terminal_row_width(void)
{
    int fallback = terminal_default_row_width();
    int w = fallback;

    if(terminal_output_box) {
        lv_obj_update_layout(terminal_output_box);
        w = lv_obj_get_width(terminal_output_box) - 12;
    }
    if(w < fallback / 2) {
        w = fallback;
    }

    return w > 120 ? w : 120;
}

static void terminal_log(const char *fmt, ...)
{
    FILE *fp = fopen(TERMINAL_LOG_PATH, "a");
    va_list ap;

    if(!fp) {
        return;
    }
    fprintf(fp, "%llu ", (unsigned long long)ui_monotonic_us());
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\n");
    fclose(fp);
}

static void terminal_set_status(const char *text)
{
    if(terminal_status_label) {
        lv_label_set_text(terminal_status_label, ui_tr(text ? text : "Ready"));
    }
}

static int terminal_write_fd_all(int fd, const char *buf, size_t len)
{
    size_t written = 0;

    if(fd < 0 || !buf || len == 0) {
        return -1;
    }

    while(written < len) {
        ssize_t rc = write(fd, buf + written, len - written);
        if(rc > 0) {
            written += (size_t)rc;
            continue;
        }
        if(rc < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
            continue;
        }
        return -1;
    }
    return 0;
}

static void terminal_tmt_cb(tmt_msg_t msg, TMT *vt, const void *arg, void *user)
{
    (void)vt;
    (void)user;

    switch(msg) {
    case TMT_MSG_UPDATE:
    case TMT_MSG_MOVED:
    case TMT_MSG_BELL:
        terminal_dirty = 1;
        break;
    case TMT_MSG_ANSWER:
        if(arg) {
            int fd = terminal_master_fd;
            terminal_write_fd_all(fd, (const char *)arg, strlen((const char *)arg));
        }
        break;
    case TMT_MSG_CURSOR:
        if(arg) {
            terminal_cursor_visible = (((const char *)arg)[0] != 'f');
            terminal_dirty = 1;
        }
        break;
    }
}

static int terminal_open_tmt_locked(void)
{
    if(terminal_tmt) {
        return 0;
    }

    terminal_tmt = tmt_open(TERMINAL_ROWS, TERMINAL_COLS, terminal_tmt_cb,
                            NULL, NULL);
    if(!terminal_tmt) {
        terminal_log("tmt_open failed");
        return -1;
    }
    terminal_cursor_visible = 1;
    terminal_dirty = 1;
    return 0;
}

static void terminal_feed_tmt_locked(const char *buf, size_t len)
{
    if(!buf || len == 0) {
        return;
    }
    if(terminal_open_tmt_locked() != 0) {
        return;
    }
    tmt_write(terminal_tmt, buf, len);
    terminal_dirty = 1;
}

static void terminal_append_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    size_t len;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    len = strlen(line);

    pthread_mutex_lock(&terminal_lock);
    terminal_feed_tmt_locked(line, len);
    terminal_feed_tmt_locked("\r\n", 2);
    pthread_mutex_unlock(&terminal_lock);
}

static int terminal_write_bytes(const char *buf, size_t len)
{
    size_t written = 0;
    int fd;

    if(!buf || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&terminal_lock);
    fd = terminal_master_fd;
    pthread_mutex_unlock(&terminal_lock);

    if(fd < 0) {
        terminal_set_status("Shell not running");
        return -1;
    }

    while(written < len) {
        ssize_t rc = write(fd, buf + written, len - written);
        if(rc > 0) {
            written += (size_t)rc;
            continue;
        }
        if(rc < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
            continue;
        }
        terminal_log("write failed errno=%d (%s)", errno, strerror(errno));
        terminal_set_status("Write failed");
        return -1;
    }
    return 0;
}

static void terminal_send_enter(void)
{
    terminal_write_bytes("\r", 1);
}

static void terminal_send_text_direct(const char *text)
{
    if(text && text[0]) {
        terminal_write_bytes(text, strlen(text));
    }
}

static void terminal_clear_output(void)
{
    pthread_mutex_lock(&terminal_lock);
    terminal_pending_len = 0;
    if(terminal_tmt) {
        tmt_reset(terminal_tmt);
    } else {
        terminal_open_tmt_locked();
    }
    terminal_dirty = 1;
    pthread_mutex_unlock(&terminal_lock);
}

static void terminal_apply_font_zoom(void)
{
    const terminal_font_zoom_t *zoom =
        &terminal_font_zoom[terminal_font_level];
    size_t row;

    for(row = 0; row < TERMINAL_ROWS; row++) {
        if(!terminal_row_labels[row]) {
            continue;
        }
        lv_obj_set_style_text_font(terminal_row_labels[row], zoom->font, 0);
        lv_obj_set_pos(terminal_row_labels[row], 0,
                       (int)row * zoom->line_height);
        lv_obj_set_size(terminal_row_labels[row], terminal_row_width(),
                        zoom->line_height);
    }
}

static void terminal_adjust_font_zoom(int delta)
{
    int max_level =
        (int)(sizeof(terminal_font_zoom) / sizeof(terminal_font_zoom[0])) - 1;
    int next = terminal_font_level + delta;
    char status[64];

    if(next < 0) {
        next = 0;
    } else if(next > max_level) {
        next = max_level;
    }
    if(next == terminal_font_level) {
        snprintf(status, sizeof(status), "Font %s",
                 terminal_font_zoom[terminal_font_level].name);
        terminal_set_status(status);
        return;
    }

    terminal_font_level = next;
    terminal_apply_font_zoom();
    snprintf(status, sizeof(status), "Font %s",
             terminal_font_zoom[terminal_font_level].name);
    terminal_set_status(status);
}

static void terminal_set_ctrl_armed(int armed)
{
    terminal_ctrl_armed = armed ? 1 : 0;
    if(!terminal_ctrl_btn) {
        return;
    }
    if(terminal_ctrl_armed) {
        lv_obj_set_style_bg_color(terminal_ctrl_btn, lv_color_hex(0x2563EB), 0);
        terminal_set_status("Ctrl armed");
    } else {
        lv_obj_set_style_bg_color(terminal_ctrl_btn, lv_color_hex(0x1D2630), 0);
        terminal_set_status("Shell ready");
    }
}

static void terminal_send_ctrl(char letter)
{
    char code = (char)(letter & 0x1F);
    terminal_write_bytes(&code, 1);
    terminal_set_ctrl_armed(0);
}

static void terminal_special_key(terminal_key_t key)
{
    switch(key) {
    case TERMINAL_KEY_CTRL:
        terminal_set_ctrl_armed(!terminal_ctrl_armed);
        break;
    case TERMINAL_KEY_ESC:
        terminal_write_bytes("\x1B", 1);
        terminal_set_ctrl_armed(0);
        break;
    case TERMINAL_KEY_TAB:
        terminal_write_bytes("\t", 1);
        terminal_set_ctrl_armed(0);
        break;
    case TERMINAL_KEY_ENTER:
        terminal_send_enter();
        terminal_set_ctrl_armed(0);
        break;
    case TERMINAL_KEY_C:
        if(terminal_ctrl_armed) {
            terminal_send_ctrl('C');
        } else {
            terminal_send_text_direct("c");
        }
        break;
    case TERMINAL_KEY_D:
        if(terminal_ctrl_armed) {
            terminal_send_ctrl('D');
        } else {
            terminal_send_text_direct("d");
        }
        break;
    case TERMINAL_KEY_L:
        if(terminal_ctrl_armed) {
            terminal_send_ctrl('L');
            terminal_clear_output();
        } else {
            terminal_send_text_direct("l");
        }
        break;
    case TERMINAL_KEY_UP:
        terminal_write_bytes("\x1B[A", 3);
        terminal_set_ctrl_armed(0);
        break;
    case TERMINAL_KEY_DOWN:
        terminal_write_bytes("\x1B[B", 3);
        terminal_set_ctrl_armed(0);
        break;
    case TERMINAL_KEY_RIGHT:
        terminal_write_bytes("\x1B[C", 3);
        terminal_set_ctrl_armed(0);
        break;
    case TERMINAL_KEY_LEFT:
        terminal_write_bytes("\x1B[D", 3);
        terminal_set_ctrl_armed(0);
        break;
    case TERMINAL_KEY_BS:
        terminal_write_bytes("\x7F", 1);
        break;
    case TERMINAL_KEY_CLEAR:
        terminal_clear_output();
        terminal_set_ctrl_armed(0);
        break;
    case TERMINAL_KEY_RESTART:
        terminal_stop_shell();
        terminal_clear_output();
        terminal_append_line("[terminal] restarting shell");
        terminal_set_ctrl_armed(0);
        if(terminal_spawn_shell() != 0) {
            terminal_set_status("Shell failed");
        }
        break;
    case TERMINAL_KEY_FONT_DOWN:
        terminal_adjust_font_zoom(-1);
        terminal_set_ctrl_armed(0);
        break;
    case TERMINAL_KEY_FONT_UP:
        terminal_adjust_font_zoom(1);
        terminal_set_ctrl_armed(0);
        break;
    }
}

static void terminal_special_event_cb(lv_event_t *event)
{
    terminal_key_t key = (terminal_key_t)(intptr_t)lv_event_get_user_data(event);
    terminal_special_key(key);
}

static int terminal_keyboard_is_mode_key(const char *text)
{
    return text &&
           (strcmp(text, "123") == 0 || strcmp(text, "abc") == 0 ||
            strcmp(text, "ABC") == 0 || strcmp(text, "#+=") == 0);
}

static void terminal_keyboard_set_mode(terminal_keyboard_mode_t mode)
{
    terminal_keyboard_mode = mode;
    if(!terminal_keyboard) {
        return;
    }

    switch(mode) {
    case TERMINAL_KBD_UPPER:
        lv_buttonmatrix_set_map(terminal_keyboard, terminal_kbd_upper_map);
        break;
    case TERMINAL_KBD_NUM:
        lv_buttonmatrix_set_map(terminal_keyboard, terminal_kbd_num_map);
        break;
    case TERMINAL_KBD_SYMBOL:
        lv_buttonmatrix_set_map(terminal_keyboard, terminal_kbd_symbol_map);
        break;
    case TERMINAL_KBD_LOWER:
    default:
        lv_buttonmatrix_set_map(terminal_keyboard, terminal_kbd_lower_map);
        break;
    }
}

static void terminal_keyboard_send_button(const char *text)
{
    if(!text || !text[0]) {
        return;
    }

    if(strcmp(text, "123") == 0) {
        terminal_keyboard_set_mode(TERMINAL_KBD_NUM);
    } else if(strcmp(text, "abc") == 0) {
        terminal_keyboard_set_mode(TERMINAL_KBD_LOWER);
    } else if(strcmp(text, "ABC") == 0) {
        terminal_keyboard_set_mode(TERMINAL_KBD_UPPER);
    } else if(strcmp(text, "#+=") == 0) {
        terminal_keyboard_set_mode(TERMINAL_KBD_SYMBOL);
    } else if(strcmp(text, "Enter") == 0) {
        terminal_send_enter();
    } else if(strcmp(text, "BS") == 0) {
        terminal_write_bytes("\x7F", 1);
    } else if(strcmp(text, "Space") == 0) {
        terminal_write_bytes(" ", 1);
    } else if(!terminal_keyboard_is_mode_key(text)) {
        terminal_send_text_direct(text);
    }
    terminal_set_ctrl_armed(0);
}

static void terminal_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if(code == LV_EVENT_VALUE_CHANGED && terminal_keyboard) {
        uint32_t btn_id = lv_buttonmatrix_get_selected_button(terminal_keyboard);
        const char *text;

        if(btn_id == LV_BUTTONMATRIX_BUTTON_NONE) {
            return;
        }
        text = lv_buttonmatrix_get_button_text(terminal_keyboard, btn_id);
        terminal_keyboard_send_button(text);
    }
}

static void terminal_hardware_key_cb(int code, uint32_t key, int pressed,
                                     void *user_data)
{
    char ch[2];

    (void)user_data;
    if(!pressed) {
        return;
    }

    terminal_log("hardware key code=%d key=0x%08X", code, key);

    if(code == 23) {
        terminal_special_key(TERMINAL_KEY_CTRL);
        return;
    }

    switch(key) {
    case LV_KEY_ENTER:
        terminal_special_key(TERMINAL_KEY_ENTER);
        return;
    case LV_KEY_ESC:
        terminal_special_key(TERMINAL_KEY_ESC);
        return;
    case LV_KEY_NEXT:
        terminal_special_key(TERMINAL_KEY_TAB);
        return;
    case LV_KEY_BACKSPACE:
    case LV_KEY_DEL:
        terminal_special_key(TERMINAL_KEY_BS);
        return;
    case LV_KEY_UP:
        terminal_special_key(TERMINAL_KEY_UP);
        return;
    case LV_KEY_DOWN:
        terminal_special_key(TERMINAL_KEY_DOWN);
        return;
    case LV_KEY_LEFT:
        terminal_special_key(TERMINAL_KEY_LEFT);
        return;
    case LV_KEY_RIGHT:
        terminal_special_key(TERMINAL_KEY_RIGHT);
        return;
    default:
        break;
    }

    if(key < 32U || key > 126U) {
        return;
    }

    if(terminal_ctrl_armed && isalpha((unsigned char)key)) {
        terminal_send_ctrl((char)toupper((unsigned char)key));
        return;
    }

    ch[0] = (char)key;
    ch[1] = '\0';
    terminal_send_text_direct(ch);
    terminal_set_ctrl_armed(0);
}

static void *terminal_reader_cb(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[512];

    while(!terminal_reader_stop) {
        fd_set rfds;
        struct timeval tv;
        int rc;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            break;
        }
        if(rc == 0 || !FD_ISSET(fd, &rfds)) {
            continue;
        }

        rc = (int)read(fd, buf, sizeof(buf));
        if(rc > 0) {
            pthread_mutex_lock(&terminal_lock);
            if((size_t)rc > TERMINAL_PENDING_MAX - terminal_pending_len) {
                terminal_feed_tmt_locked((const char *)terminal_pending,
                                         terminal_pending_len);
                terminal_pending_len = 0;
            }
            if((size_t)rc <= TERMINAL_PENDING_MAX - terminal_pending_len) {
                memcpy(terminal_pending + terminal_pending_len, buf, (size_t)rc);
                terminal_pending_len += (size_t)rc;
            } else {
                terminal_feed_tmt_locked(buf, (size_t)rc);
            }
            terminal_dirty = 1;
            pthread_mutex_unlock(&terminal_lock);
        } else if(rc == 0 || !(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
    }

    terminal_log("reader exit fd=%d", fd);
    return NULL;
}

static int terminal_spawn_shell(void)
{
    int master;
    int slave;
    char *slave_name;
    pid_t pid;
    struct winsize ws;

    master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(master < 0) {
        terminal_log("posix_openpt failed errno=%d (%s)", errno, strerror(errno));
        terminal_append_line("[terminal] posix_openpt failed: %s", strerror(errno));
        return -1;
    }
    if(grantpt(master) < 0 || unlockpt(master) < 0) {
        terminal_log("grantpt/unlockpt failed errno=%d (%s)", errno, strerror(errno));
        close(master);
        terminal_append_line("[terminal] grantpt failed: %s", strerror(errno));
        return -1;
    }
    slave_name = ptsname(master);
    if(!slave_name) {
        terminal_log("ptsname failed errno=%d (%s)", errno, strerror(errno));
        close(master);
        terminal_append_line("[terminal] ptsname failed: %s", strerror(errno));
        return -1;
    }

    pid = fork();
    if(pid < 0) {
        terminal_log("fork failed errno=%d (%s)", errno, strerror(errno));
        close(master);
        terminal_append_line("[terminal] fork failed: %s", strerror(errno));
        return -1;
    }

    if(pid == 0) {
        setsid();
        slave = open(slave_name, O_RDWR | O_NOCTTY);
        if(slave < 0) {
            _exit(127);
        }

        memset(&ws, 0, sizeof(ws));
        ws.ws_row = TERMINAL_ROWS;
        ws.ws_col = TERMINAL_COLS;
        ioctl(slave, TIOCSWINSZ, &ws);
        ioctl(slave, TIOCSCTTY, 0);

        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if(slave > STDERR_FILENO) {
            close(slave);
        }
        close(master);

        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);

        setenv("TERM", "ansi", 1);
        setenv("LANG", "C.UTF-8", 1);
        setenv("LC_CTYPE", "C.UTF-8", 1);
        setenv("HOME", "/root", 1);
        setenv("SHELL", TERMINAL_SHELL, 1);
        if(chdir("/root") != 0) {
            int chdir_rc = chdir("/");
            (void)chdir_rc;
        }
        execl(TERMINAL_SHELL, "sh", "-l", (char *)NULL);
        execl("/bin/ash", "ash", "-l", (char *)NULL);
        _exit(127);
    }

    pthread_mutex_lock(&terminal_lock);
    terminal_master_fd = master;
    terminal_child_pid = pid;
    terminal_reader_stop = 0;
    pthread_mutex_unlock(&terminal_lock);

    if(pthread_create(&terminal_reader_thread, NULL, terminal_reader_cb,
                      (void *)(intptr_t)master) != 0) {
        terminal_log("reader pthread_create failed");
        kill(pid, SIGHUP);
        close(master);
        pthread_mutex_lock(&terminal_lock);
        terminal_master_fd = -1;
        terminal_child_pid = -1;
        pthread_mutex_unlock(&terminal_lock);
        terminal_append_line("[terminal] reader thread failed");
        return -1;
    }
    terminal_reader_started = 1;
    terminal_log("spawned shell pid=%ld master=%d", (long)pid, master);
    terminal_set_status("Shell ready");
    return 0;
}

static void terminal_reap_child(void)
{
    pid_t pid;
    int status;

    pthread_mutex_lock(&terminal_lock);
    pid = terminal_child_pid;
    pthread_mutex_unlock(&terminal_lock);

    if(pid <= 0) {
        return;
    }
    if(waitpid(pid, &status, WNOHANG) == pid) {
        pthread_mutex_lock(&terminal_lock);
        terminal_child_pid = -1;
        pthread_mutex_unlock(&terminal_lock);
        terminal_set_status("Shell exited");
        terminal_append_line("[terminal] shell exited");
    }
}

static size_t terminal_append_utf8(char *out, size_t pos, size_t max, wchar_t wc)
{
    char tmp[MB_LEN_MAX];
    mbstate_t st;
    size_t len;

    if(pos + 1U >= max) {
        return pos;
    }
    if(wc == 0 || wc == L'\n' || wc == L'\r' || wc == L'\t') {
        wc = L' ';
    }
    if(wc < 32) {
        wc = L' ';
    }
    if(wc >= 32 && wc < 127) {
        out[pos++] = (char)wc;
        out[pos] = '\0';
        return pos;
    }

    memset(&st, 0, sizeof(st));
    len = wcrtomb(tmp, wc, &st);
    if(len == (size_t)-1 || len == 0 || pos + len >= max) {
        out[pos++] = '?';
        out[pos] = '\0';
        return pos;
    }

    memcpy(out + pos, tmp, len);
    pos += len;
    out[pos] = '\0';
    return pos;
}

static void terminal_format_row_locked(size_t row, char *out, size_t out_len)
{
    const TMTSCREEN *screen;
    const TMTPOINT *cursor;
    size_t col;
    size_t pos = 0;

    if(!out || out_len == 0) {
        return;
    }
    out[0] = '\0';

    if(!terminal_tmt) {
        return;
    }

    screen = tmt_screen(terminal_tmt);
    cursor = tmt_cursor(terminal_tmt);
    if(!screen || row >= screen->nline) {
        return;
    }

    for(col = 0; col < screen->ncol && col < TERMINAL_COLS; col++) {
        wchar_t wc = screen->lines[row]->chars[col].c;
        if(terminal_cursor_visible && cursor && cursor->r == row &&
           cursor->c == col) {
            wc = (wc == L' ' || wc == 0) ? L'_' : wc;
        }
        pos = terminal_append_utf8(out, pos, out_len, wc);
        if(pos + 1U >= out_len) {
            break;
        }
    }

    if(pos == 0) {
        out[pos++] = ' ';
        out[pos] = '\0';
    }
}

static void terminal_timer_cb(lv_timer_t *timer)
{
    char rows[TERMINAL_ROWS][TERMINAL_ROW_TEXT_MAX];
    int dirty;
    size_t row;

    (void)timer;
    memset(rows, 0, sizeof(rows));

    pthread_mutex_lock(&terminal_lock);
    if(terminal_pending_len > 0) {
        terminal_feed_tmt_locked((const char *)terminal_pending,
                                 terminal_pending_len);
        terminal_pending_len = 0;
    }
    dirty = terminal_dirty;
    if(dirty) {
        for(row = 0; row < TERMINAL_ROWS; row++) {
            terminal_format_row_locked(row, rows[row], sizeof(rows[row]));
        }
        if(terminal_tmt) {
            tmt_clean(terminal_tmt);
        }
        terminal_dirty = 0;
    }
    pthread_mutex_unlock(&terminal_lock);

    if(dirty) {
        for(row = 0; row < TERMINAL_ROWS; row++) {
            if(!terminal_row_labels[row]) {
                continue;
            }
            if(strcmp(terminal_last_rows[row], rows[row]) != 0) {
                lv_label_set_text(terminal_row_labels[row], rows[row][0] ?
                                  rows[row] : " ");
                snprintf(terminal_last_rows[row], sizeof(terminal_last_rows[row]),
                         "%s", rows[row][0] ? rows[row] : " ");
            }
        }
    }

    terminal_reap_child();
}

static lv_obj_t *terminal_special_button_sized(lv_obj_t *parent, int x, int y,
                                               int w, int h, const char *text,
                                               terminal_key_t key)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1D2630), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2D3744), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2F3C48), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, terminal_special_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)key);

    lv_obj_t *label = ui_label(btn, text,
                               h < 38 ? &lv_font_montserrat_14 :
                               &lv_font_montserrat_16,
                               0xF2F5F8);
    lv_obj_center(label);
    ui_make_click_forwarder(label);

    if(key == TERMINAL_KEY_CTRL) {
        terminal_ctrl_btn = btn;
    }
    return btn;
}

static lv_obj_t *terminal_special_button(lv_obj_t *parent, int x, int y, int w,
                                         const char *text, terminal_key_t key)
{
    return terminal_special_button_sized(parent, x, y, w, 52, text, key);
}

void ui_terminal_create(lv_obj_t *scr)
{
    size_t row;
    int landscape = ui_is_landscape();
    int keyboard_h;
    int output_y = landscape ? 132 : 146;
    int special_y1;
    int special_y2;
    int output_h;
    int side_controls_w = landscape ? 184 : 0;
    int output_w = ui_screen_width() - 32;

    terminal_use_hardware_keyboard = ui_extension_keyboard_active();
    keyboard_h = terminal_use_hardware_keyboard ? 0 : (landscape ? 168 : 278);
    if(landscape) {
        output_w = ui_screen_width() - side_controls_w - 40;
        special_y1 = output_y;
        special_y2 = special_y1;
        output_h = ui_screen_height() - output_y - keyboard_h - 16;
    } else if(terminal_use_hardware_keyboard) {
        special_y1 = ui_screen_height() - 132;
        special_y2 = ui_screen_height() - 66;
        output_h = special_y1 - output_y - 12;
    } else {
        special_y1 = 808;
        special_y2 = 878;
        output_h = 654;
    }

    if(output_h < 120) {
        output_h = 120;
    }
    if(output_w < 320) {
        output_w = ui_screen_width() - 32;
    }

    setenv("LC_CTYPE", "C.UTF-8", 0);
    if(!setlocale(LC_CTYPE, "")) {
        setlocale(LC_CTYPE, "C");
    }
    memset(terminal_last_rows, 0, sizeof(terminal_last_rows));
    memset(terminal_row_labels, 0, sizeof(terminal_row_labels));

    ui_create_header(scr, "Terminal");

    terminal_status_label = ui_label(scr, "Shell starting", &lv_font_montserrat_16,
                                     0x9AA4AF);
    lv_obj_align(terminal_status_label, LV_ALIGN_TOP_RIGHT, -28, 92);

    terminal_output_box = lv_obj_create(scr);
    lv_obj_set_pos(terminal_output_box, 16, output_y);
    lv_obj_set_size(terminal_output_box, output_w, output_h);
    lv_obj_set_style_bg_color(terminal_output_box, lv_color_hex(0x05080C), 0);
    lv_obj_set_style_bg_opa(terminal_output_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(terminal_output_box, 0, 0);
    lv_obj_set_style_border_color(terminal_output_box, lv_color_hex(0x22303C), 0);
    lv_obj_set_style_radius(terminal_output_box, 0, 0);
    lv_obj_set_style_pad_all(terminal_output_box, 6, 0);
    lv_obj_clear_flag(terminal_output_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(terminal_output_box);

    for(row = 0; row < TERMINAL_ROWS; row++) {
        terminal_row_labels[row] = ui_label(terminal_output_box, " ",
                                            &lv_font_montserrat_12, 0xCDE7D8);
        lv_obj_set_pos(terminal_row_labels[row], 0, (int)row * 16);
        lv_obj_set_size(terminal_row_labels[row], terminal_row_width(), 16);
        lv_label_set_long_mode(terminal_row_labels[row], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_letter_space(terminal_row_labels[row], 0, 0);
        lv_obj_set_style_text_line_space(terminal_row_labels[row], 0, 0);
    }
    terminal_apply_font_zoom();

    if(landscape) {
        static const struct {
            const char *text;
            terminal_key_t key;
        } keys[] = {
            { "Ctrl", TERMINAL_KEY_CTRL }, { "Esc", TERMINAL_KEY_ESC },
            { "Tab", TERMINAL_KEY_TAB }, { "Enter", TERMINAL_KEY_ENTER },
            { "A-", TERMINAL_KEY_FONT_DOWN }, { "A+", TERMINAL_KEY_FONT_UP },
            { "Clear", TERMINAL_KEY_CLEAR }, { "Restart", TERMINAL_KEY_RESTART },
            { "C", TERMINAL_KEY_C }, { "D", TERMINAL_KEY_D },
            { "L", TERMINAL_KEY_L }, { "BS", TERMINAL_KEY_BS },
            { "Left", TERMINAL_KEY_LEFT }, { "Right", TERMINAL_KEY_RIGHT },
            { "Up", TERMINAL_KEY_UP }, { "Down", TERMINAL_KEY_DOWN },
        };
        int side_x = ui_screen_width() - side_controls_w + 8;
        int available_h = ui_screen_height() - output_y - keyboard_h - 16;
        int btn_w = 76;
        int btn_h = terminal_use_hardware_keyboard ? 42 : 28;
        int gap_x = 8;
        int gap_y = terminal_use_hardware_keyboard ? 8 : 4;

        if(available_h < 260) {
            btn_h = 28;
            gap_y = 4;
        }
        for(size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            int col = (int)(i % 2U);
            int row_i = (int)(i / 2U);
            terminal_special_button_sized(scr,
                                          side_x + col * (btn_w + gap_x),
                                          output_y + row_i * (btn_h + gap_y),
                                          btn_w, btn_h,
                                          keys[i].text, keys[i].key);
        }
    } else {
        terminal_special_button(scr, 24, special_y1, 58, "Ctrl", TERMINAL_KEY_CTRL);
        terminal_special_button(scr, 88, special_y1, 52, "Esc", TERMINAL_KEY_ESC);
        terminal_special_button(scr, 146, special_y1, 52, "Tab", TERMINAL_KEY_TAB);
        terminal_special_button(scr, 204, special_y1, 70, "Enter", TERMINAL_KEY_ENTER);
        terminal_special_button(scr, 280, special_y1, 46, "A-", TERMINAL_KEY_FONT_DOWN);
        terminal_special_button(scr, 332, special_y1, 46, "A+", TERMINAL_KEY_FONT_UP);
        terminal_special_button(scr, 384, special_y1, 64, "Clear", TERMINAL_KEY_CLEAR);
        terminal_special_button(scr, 454, special_y1, 90, "Restart", TERMINAL_KEY_RESTART);

        terminal_special_button(scr, 24, special_y2, 52, "C", TERMINAL_KEY_C);
        terminal_special_button(scr, 84, special_y2, 52, "D", TERMINAL_KEY_D);
        terminal_special_button(scr, 144, special_y2, 52, "L", TERMINAL_KEY_L);
        terminal_special_button(scr, 204, special_y2, 62, "Left", TERMINAL_KEY_LEFT);
        terminal_special_button(scr, 274, special_y2, 58, "Up", TERMINAL_KEY_UP);
        terminal_special_button(scr, 340, special_y2, 66, "Down", TERMINAL_KEY_DOWN);
        terminal_special_button(scr, 414, special_y2, 66, "Right", TERMINAL_KEY_RIGHT);
        terminal_special_button(scr, 488, special_y2, 56, "BS", TERMINAL_KEY_BS);
    }

    if(terminal_use_hardware_keyboard) {
        ui_extension_keyboard_set_key_cb(terminal_hardware_key_cb, NULL);
        terminal_keyboard = NULL;
    } else {
        terminal_keyboard = lv_buttonmatrix_create(scr);
        lv_obj_set_size(terminal_keyboard, ui_screen_width(), keyboard_h);
        lv_obj_align(terminal_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_text_font(terminal_keyboard, &lv_font_montserrat_14, 0);
        lv_obj_set_style_bg_color(terminal_keyboard, lv_color_hex(0x101418),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(terminal_keyboard, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(terminal_keyboard, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(terminal_keyboard, landscape ? 4 : 8,
                                 LV_PART_MAIN);
        lv_obj_set_style_pad_row(terminal_keyboard, landscape ? 4 : 8,
                                 LV_PART_MAIN);
        lv_obj_set_style_pad_column(terminal_keyboard, landscape ? 4 : 8,
                                    LV_PART_MAIN);
        lv_obj_set_style_bg_color(terminal_keyboard, lv_color_hex(0x1C2530),
                                  LV_PART_ITEMS);
        lv_obj_set_style_bg_color(terminal_keyboard, lv_color_hex(0x2F3C4A),
                                  LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(terminal_keyboard, 0, LV_PART_ITEMS);
        lv_obj_set_style_radius(terminal_keyboard, 8, LV_PART_ITEMS);
        lv_obj_add_event_cb(terminal_keyboard, terminal_keyboard_event_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
        terminal_keyboard_set_mode(TERMINAL_KBD_LOWER);
    }

    pthread_mutex_lock(&terminal_lock);
    if(terminal_open_tmt_locked() != 0) {
        terminal_set_status("Terminal core failed");
    }
    pthread_mutex_unlock(&terminal_lock);

    terminal_clear_output();
    terminal_append_line("[terminal] libtmt ansi 72x32");
    terminal_append_line("[terminal] /bin/sh -l");
    if(terminal_use_hardware_keyboard) {
        terminal_append_line("[terminal] hardware keyboard active");
    }
    if(terminal_spawn_shell() != 0) {
        terminal_set_status("Shell failed");
    }
    terminal_timer = lv_timer_create(terminal_timer_cb, TERMINAL_TIMER_MS, NULL);
    lv_timer_ready(terminal_timer);
}

static void terminal_stop_shell(void)
{
    int fd;
    pid_t pid;

    pthread_mutex_lock(&terminal_lock);
    fd = terminal_master_fd;
    pid = terminal_child_pid;
    terminal_reader_stop = 1;
    terminal_master_fd = -1;
    terminal_child_pid = -1;
    pthread_mutex_unlock(&terminal_lock);

    if(pid > 0) {
        kill(pid, SIGHUP);
    }
    if(fd >= 0) {
        close(fd);
    }
    if(terminal_reader_started) {
        pthread_join(terminal_reader_thread, NULL);
        terminal_reader_started = 0;
    }
    if(pid > 0) {
        int status;
        int i;
        for(i = 0; i < 20; i++) {
            pid_t rc = waitpid(pid, &status, WNOHANG);
            if(rc == pid || (rc < 0 && errno == ECHILD)) {
                return;
            }
            usleep(10000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

void ui_terminal_cleanup(void)
{
    size_t row;

    if(terminal_use_hardware_keyboard) {
        ui_extension_keyboard_set_key_cb(NULL, NULL);
        terminal_use_hardware_keyboard = 0;
    }

    if(terminal_timer) {
        lv_timer_delete(terminal_timer);
        terminal_timer = NULL;
    }

    terminal_stop_shell();

    pthread_mutex_lock(&terminal_lock);
    if(terminal_tmt) {
        tmt_close(terminal_tmt);
        terminal_tmt = NULL;
    }
    terminal_pending_len = 0;
    terminal_dirty = 0;
    pthread_mutex_unlock(&terminal_lock);

    terminal_output_box = NULL;
    for(row = 0; row < TERMINAL_ROWS; row++) {
        terminal_row_labels[row] = NULL;
        terminal_last_rows[row][0] = '\0';
    }
    terminal_status_label = NULL;
    terminal_keyboard = NULL;
    terminal_ctrl_btn = NULL;
    terminal_ctrl_armed = 0;
}
