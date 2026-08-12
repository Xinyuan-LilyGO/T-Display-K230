#include "ui_i2s_test.h"

#include "ui_audio.h"
#include "ui_hardware.h"
#include "ui_i18n.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define I2S_TEST_RAW_PATH "/tmp/k230_i2s_test.raw"
#define I2S_TEST_LOG_PATH "/tmp/k230_i2s_test.log"
#define I2S_TEST_RATE 48000
#define I2S_TEST_DURATION_MS 4000
#define I2S_PI 3.14159265358979323846

typedef enum {
    I2S_ACTION_ENABLE_AMP = 0,
    I2S_ACTION_DISABLE_AMP,
    I2S_ACTION_TONE_1K,
    I2S_ACTION_TONE_440,
    I2S_ACTION_LEFT_ONLY,
    I2S_ACTION_RIGHT_ONLY,
    I2S_ACTION_STOP,
} i2s_action_t;

static lv_obj_t *i2s_status_label;
static lv_obj_t *i2s_gpio_label;
static lv_obj_t *i2s_card_label;
static lv_obj_t *i2s_log_label;
static lv_timer_t *i2s_page_timer;
static pid_t i2s_test_pid = -1;
static char i2s_status_text[192] = "Ready";

static void i2s_set_status(const char *fmt, ...)
{
    va_list ap;

    if(!fmt) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(i2s_status_text, sizeof(i2s_status_text), fmt, ap);
    va_end(ap);

    if(i2s_status_label) {
        lv_label_set_text(i2s_status_label, i2s_status_text);
    }
}

static int i2s_set_amp(int enabled)
{
    return ui_audio_output_set_external(enabled);
}

static void i2s_read_asound_card(char *buf, size_t len)
{
    FILE *fp;
    char line[160];
    char first[160] = "";

    if(!buf || len == 0) {
        return;
    }
    snprintf(buf, len, "%s", "K230_I2S_INNO");

    fp = fopen("/proc/asound/cards", "r");
    if(!fp) {
        return;
    }

    while(fgets(line, sizeof(line), fp)) {
        ui_trim_text(line);
        if(!first[0] && line[0]) {
            snprintf(first, sizeof(first), "%s", line);
        }
        if(strstr(line, "K230")) {
            snprintf(buf, len, "%s", line);
            fclose(fp);
            return;
        }
    }

    fclose(fp);
    if(first[0]) {
        snprintf(buf, len, "%s", first);
    }
}

static void i2s_read_last_log(char *buf, size_t len)
{
    FILE *fp;
    char line[160];

    if(!buf || len == 0) {
        return;
    }
    buf[0] = '\0';

    fp = fopen(I2S_TEST_LOG_PATH, "r");
    if(!fp) {
        snprintf(buf, len, "%s", ui_tr("No log yet"));
        return;
    }

    while(fgets(line, sizeof(line), fp)) {
        ui_trim_text(line);
        if(line[0]) {
            snprintf(buf, len, "%s", line);
        }
    }
    fclose(fp);

    if(!buf[0]) {
        snprintf(buf, len, "%s", ui_tr("No log yet"));
    }
}

static int i2s_write_tone(const char *path, int frequency_hz, int left_pct,
                          int right_pct)
{
    FILE *fp;
    int samples = (I2S_TEST_RATE * I2S_TEST_DURATION_MS) / 1000;
    double phase = 0.0;
    double inc = 2.0 * I2S_PI * (double)frequency_hz / (double)I2S_TEST_RATE;
    double left_gain = (double)left_pct / 100.0;
    double right_gain = (double)right_pct / 100.0;

    fp = fopen(path, "wb");
    if(!fp) {
        return -1;
    }

    for(int i = 0; i < samples; i++) {
        double env = 1.0;
        int16_t tone;
        int16_t left;
        int16_t right;

        if(i < 480) {
            env = (double)i / 480.0;
        } else if(samples - i < 480) {
            env = (double)(samples - i) / 480.0;
        }

        tone = (int16_t)(sin(phase) * 26000.0 * env);
        left = (int16_t)((double)tone * left_gain);
        right = (int16_t)((double)tone * right_gain);
        if(fwrite(&left, sizeof(left), 1, fp) != 1 ||
           fwrite(&right, sizeof(right), 1, fp) != 1) {
            fclose(fp);
            return -1;
        }

        phase += inc;
        if(phase >= 2.0 * I2S_PI) {
            phase -= 2.0 * I2S_PI;
        }
    }

    return fclose(fp) == 0 ? 0 : -1;
}

static void i2s_reap_playback(void)
{
    int status;
    pid_t rc;

    if(i2s_test_pid <= 0) {
        return;
    }

    rc = waitpid(i2s_test_pid, &status, WNOHANG);
    if(rc == i2s_test_pid) {
        if(WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            i2s_set_status("%s", ui_tr("Playback finished"));
        } else {
            i2s_set_status("%s", ui_tr("Playback failed"));
        }
        i2s_test_pid = -1;
    } else if(rc < 0 && errno == ECHILD) {
        i2s_test_pid = -1;
    }
}

static void i2s_stop_playback(void)
{
    int status;

    if(i2s_test_pid <= 0) {
        return;
    }

    kill(-i2s_test_pid, SIGTERM);
    for(int i = 0; i < 10; i++) {
        pid_t rc = waitpid(i2s_test_pid, &status, WNOHANG);
        if(rc == i2s_test_pid || (rc < 0 && errno == ECHILD)) {
            i2s_test_pid = -1;
            return;
        }
        usleep(30000);
    }

    kill(-i2s_test_pid, SIGKILL);
    waitpid(i2s_test_pid, &status, 0);
    i2s_test_pid = -1;
}

static void i2s_start_tone(int frequency_hz, int left_pct, int right_pct,
                           const char *name)
{
    char cmd[384];
    FILE *log_fp;
    pid_t pid;

    i2s_stop_playback();
    ui_audio_stop_for_exclusive_app("I2S test");

    if(i2s_set_amp(1) != 0) {
        i2s_set_status("%s", ui_tr("GPIO34 enable failed"));
        return;
    }

    if(i2s_write_tone(I2S_TEST_RAW_PATH, frequency_hz, left_pct,
                      right_pct) != 0) {
        i2s_set_status("%s", ui_tr("Tone file failed"));
        return;
    }

    log_fp = fopen(I2S_TEST_LOG_PATH, "a");
    if(log_fp) {
        fprintf(log_fp, "play %s freq=%d left=%d right=%d\n",
                name ? name : "tone", frequency_hz, left_pct, right_pct);
        fclose(log_fp);
    }

    snprintf(cmd, sizeof(cmd),
             "aplay -q -D default -t raw -f S16_LE -c 2 -r %d "
             "-B 120000 -F 20000 %s >>%s 2>&1",
             I2S_TEST_RATE, I2S_TEST_RAW_PATH, I2S_TEST_LOG_PATH);

    pid = fork();
    if(pid < 0) {
        i2s_set_status("%s", ui_tr("fork failed"));
        return;
    }

    if(pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    setpgid(pid, pid);
    i2s_test_pid = pid;
    i2s_set_status("%s: %s", ui_tr("Playing"), ui_tr(name ? name : "Tone"));
}

static void i2s_update_runtime_labels(void)
{
    char text[160];

    if(i2s_gpio_label) {
        snprintf(text, sizeof(text), "GPIO34 %s",
                 ui_amp_is_enabled() ? "high" : "low");
        lv_label_set_text(i2s_gpio_label, text);
    }

    if(i2s_card_label) {
        i2s_read_asound_card(text, sizeof(text));
        lv_label_set_text(i2s_card_label, text[0] ? text : "K230_I2S_INNO");
    }

    if(i2s_log_label) {
        i2s_read_last_log(text, sizeof(text));
        lv_label_set_text(i2s_log_label, text[0] ? text : ui_tr("No log yet"));
    }

    if(i2s_status_label) {
        lv_label_set_text(i2s_status_label, i2s_status_text);
    }
}

static void i2s_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    i2s_reap_playback();
    i2s_update_runtime_labels();
}

static void i2s_button_event_cb(lv_event_t *event)
{
    i2s_action_t action = (i2s_action_t)(intptr_t)lv_event_get_user_data(event);

    switch(action) {
    case I2S_ACTION_ENABLE_AMP:
        if(i2s_set_amp(1) == 0) {
            i2s_set_status("%s", ui_tr("Amp enabled"));
        } else {
            i2s_set_status("%s", ui_tr("GPIO34 enable failed"));
        }
        break;
    case I2S_ACTION_DISABLE_AMP:
        i2s_stop_playback();
        if(i2s_set_amp(0) == 0) {
            i2s_set_status("%s", ui_tr("Amp disabled"));
        } else {
            i2s_set_status("%s", ui_tr("GPIO34 disable failed"));
        }
        break;
    case I2S_ACTION_TONE_1K:
        i2s_start_tone(1000, 100, 100, "1 kHz tone");
        break;
    case I2S_ACTION_TONE_440:
        i2s_start_tone(440, 100, 100, "440 Hz tone");
        break;
    case I2S_ACTION_LEFT_ONLY:
        i2s_start_tone(1000, 100, 0, "Left only");
        break;
    case I2S_ACTION_RIGHT_ONLY:
        i2s_start_tone(1000, 0, 100, "Right only");
        break;
    case I2S_ACTION_STOP:
        i2s_stop_playback();
        i2s_set_status("%s", ui_tr("Stopped"));
        break;
    default:
        break;
    }

    i2s_update_runtime_labels();
    app_request_fast_refresh();
}

static lv_obj_t *i2s_button(lv_obj_t *parent, int x, int y, int w,
                            const char *text, uint32_t color,
                            i2s_action_t action)
{
    lv_obj_t *btn = ui_command_button(parent, x, y, w, text, color);

    lv_obj_add_event_cb(btn, i2s_button_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)action);
    return btn;
}

void ui_i2s_test_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *hero;
    lv_obj_t *info;
    lv_obj_t *controls;
    lv_obj_t *log_panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    int landscape = ui_is_landscape();
    int body_h = ui_body_height(144);
    int left_w = landscape ? 340 : 520;
    int right_x = landscape ? 388 : 24;
    int right_w = landscape ? ui_screen_width() - right_x - 24 : 520;
    int hero_h = landscape ? 158 : 210;
    int info_y = landscape ? 202 : 258;
    int info_h = landscape ? body_h - info_y - 24 : 244;
    int controls_y = landscape ? 24 : 526;
    int controls_h = landscape ? 244 : 354;
    int log_y = landscape ? 292 : 904;
    int log_h = landscape ? body_h - log_y - 24 : 152;
    int btn_w;

    if(right_w < 340) {
        right_w = 340;
    }
    if(info_h < 170) {
        info_h = 170;
    }
    if(log_h < 72) {
        log_h = 72;
    }
    btn_w = landscape ? (right_w - 64) / 3 : (right_w - 48) / 2;

    ui_create_header(scr, "I2S Test");

    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    hero = ui_panel(body, 24, 24, left_w, hero_h);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x141F2B), 0);
    lv_obj_set_style_border_color(hero, lv_color_hex(0x263544), 0);

    title = ui_label(hero, "MAX98357A", &lv_font_montserrat_30, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    subtitle = ui_label(hero, "Test sound from ALSA default device",
                        &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(subtitle, left_w - 32);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 0, 52);

    i2s_status_label = ui_label(hero, i2s_status_text, &lv_font_montserrat_22,
                                0x25C281);
    lv_obj_set_width(i2s_status_label, ui_inner_width());
    lv_label_set_long_mode(i2s_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(i2s_status_label, LV_ALIGN_TOP_LEFT, 0,
                 landscape ? 112 : 126);

    info = ui_panel(body, 24, info_y, left_w, info_h);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x151B22), 0);
    ui_info_row(info, 16, "I2S pins", "BCLK32 LRCK33 DATA35", 0xF2F5F8);
    ui_info_row(info, 70, "Audio format", "48 kHz S16_LE stereo", 0x3DA5FF);
    ui_info_row(info, 124, "Amp enable", "GPIO34 high enable", 0x25C281);
    ui_info_row(info, 178, "ALSA card", "K230_I2S_INNO", 0xF5A524);
    i2s_card_label = lv_obj_get_child(info, lv_obj_get_child_count(info) - 1);

    controls = ui_panel(body, right_x, controls_y, right_w, controls_h);
    lv_obj_set_style_bg_color(controls, lv_color_hex(0x101820), 0);
    i2s_gpio_label = ui_label(controls, "GPIO34 low", &lv_font_montserrat_18,
                              0x9AA4AF);
    lv_obj_align(i2s_gpio_label, LV_ALIGN_TOP_LEFT, 0, 0);

    if(landscape) {
        i2s_button(controls, 0, 44, btn_w, "Enable amp", 0x25C281,
                   I2S_ACTION_ENABLE_AMP);
        i2s_button(controls, btn_w + 16, 44, btn_w, "Disable amp", 0xF5A524,
                   I2S_ACTION_DISABLE_AMP);
        i2s_button(controls, (btn_w + 16) * 2, 44, btn_w, "Stop", 0xEF4D5A,
                   I2S_ACTION_STOP);
        i2s_button(controls, 0, 110, btn_w, "1 kHz tone", 0x3DA5FF,
                   I2S_ACTION_TONE_1K);
        i2s_button(controls, btn_w + 16, 110, btn_w, "440 Hz tone", 0x8B5CF6,
                   I2S_ACTION_TONE_440);
        i2s_button(controls, (btn_w + 16) * 2, 110, btn_w, "Left only", 0x22D3EE,
                   I2S_ACTION_LEFT_ONLY);
        i2s_button(controls, 0, 176, btn_w, "Right only", 0xEC4899,
                   I2S_ACTION_RIGHT_ONLY);
    } else {
        i2s_button(controls, 0, 52, btn_w, "Enable amp", 0x25C281,
                   I2S_ACTION_ENABLE_AMP);
        i2s_button(controls, btn_w + 16, 52, btn_w, "Disable amp", 0xF5A524,
                   I2S_ACTION_DISABLE_AMP);
        i2s_button(controls, 0, 126, btn_w, "1 kHz tone", 0x3DA5FF,
                   I2S_ACTION_TONE_1K);
        i2s_button(controls, btn_w + 16, 126, btn_w, "440 Hz tone", 0x8B5CF6,
                   I2S_ACTION_TONE_440);
        i2s_button(controls, 0, 200, btn_w, "Left only", 0x22D3EE,
                   I2S_ACTION_LEFT_ONLY);
        i2s_button(controls, btn_w + 16, 200, btn_w, "Right only", 0xEC4899,
                   I2S_ACTION_RIGHT_ONLY);
        i2s_button(controls, 0, 274, 488, "Stop", 0xEF4D5A,
                   I2S_ACTION_STOP);
    }

    log_panel = ui_panel(body, right_x, log_y, right_w, log_h);
    lv_obj_set_style_bg_color(log_panel, lv_color_hex(0x151B22), 0);
    ui_label(log_panel, "Last log", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(lv_obj_get_child(log_panel,
                                  lv_obj_get_child_count(log_panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    i2s_log_label = ui_label(log_panel, "--", &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(i2s_log_label, right_w - 32);
    lv_label_set_long_mode(i2s_log_label, LV_LABEL_LONG_DOT);
    lv_obj_align(i2s_log_label, LV_ALIGN_TOP_LEFT, 0, 50);

    i2s_page_timer = lv_timer_create(i2s_timer_cb, 300, NULL);
    if(i2s_set_amp(1) == 0) {
        i2s_set_status("%s", ui_tr("Ready"));
    } else {
        i2s_set_status("%s", ui_tr("GPIO34 enable failed"));
    }
    i2s_update_runtime_labels();
}

void ui_i2s_test_cleanup(void)
{
    if(i2s_page_timer) {
        lv_timer_delete(i2s_page_timer);
        i2s_page_timer = NULL;
    }

    i2s_stop_playback();
    i2s_status_label = NULL;
    i2s_gpio_label = NULL;
    i2s_card_label = NULL;
    i2s_log_label = NULL;
}
