#include "ui_mic_spectrum.h"

#include "ui_hardware.h"
#include "ui_i18n.h"
#include "ui_prefs.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MIC_FFT_BINS 32
#define MIC_FFT_FRAMES 512
#define MIC_FFT_RATE 48000
#define MIC_FFT_CHANNELS 2
#define MIC_FFT_LOG_PATH "/tmp/k230_phone_mic_fft.log"
#define MIC_FFT_MIN_BIN 1
#define MIC_FFT_MAX_BIN (MIC_FFT_FRAMES / 2 - 1)
#define MIC_FFT_GAIN_PREF "mic_fft.gain_x10"
#define MIC_FFT_GAIN_MIN_X10 10
#define MIC_FFT_GAIN_MAX_X10 120
#define MIC_FFT_GAIN_DEFAULT_X10 30

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    int start;
    int end;
} mic_fft_band_t;

static pthread_mutex_t mic_fft_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mic_fft_thread;
static int mic_fft_thread_valid;
static volatile int mic_fft_running;
static pid_t mic_fft_pid = -1;
static float mic_fft_bins[MIC_FFT_BINS];
static float mic_fft_noise[MIC_FFT_BINS];
static float mic_fft_peak;
static float mic_fft_rms;
static uint32_t mic_fft_frame_count;
static char mic_fft_status[128] = "Idle";
static int mic_fft_noise_ready;

static int mic_fft_tables_ready;
static uint16_t mic_fft_bitrev[MIC_FFT_FRAMES];
static float mic_fft_window[MIC_FFT_FRAMES];
static float mic_fft_twiddle_cos[MIC_FFT_FRAMES / 2];
static float mic_fft_twiddle_sin[MIC_FFT_FRAMES / 2];
static mic_fft_band_t mic_fft_band[MIC_FFT_BINS];

static lv_timer_t *mic_fft_timer;
static lv_obj_t *mic_fft_bar[MIC_FFT_BINS];
static lv_obj_t *mic_fft_status_label;
static lv_obj_t *mic_fft_peak_label;
static lv_obj_t *mic_fft_frames_label;
static lv_obj_t *mic_fft_gain_label;
static lv_obj_t *mic_fft_gain_slider;
static int mic_fft_input_route_active;
static int mic_fft_bar_base_x = 14;
static int mic_fft_bar_slot_w = 15;
static int mic_fft_bar_w = 10;
static int mic_fft_bar_bottom = 18;
static int mic_fft_bar_max_h = 446;
static int mic_fft_gain_loaded;
static int mic_fft_gain_x10 = MIC_FFT_GAIN_DEFAULT_X10;

static void mic_fft_set_status(const char *text)
{
    pthread_mutex_lock(&mic_fft_lock);
    snprintf(mic_fft_status, sizeof(mic_fft_status), "%s", text ? text : "");
    pthread_mutex_unlock(&mic_fft_lock);
}

static void mic_fft_log(const char *text)
{
    FILE *fp = fopen(MIC_FFT_LOG_PATH, "a");

    if(!fp) {
        return;
    }
    fprintf(fp, "[%llu] %s\n", (unsigned long long)ui_monotonic_us(),
            text ? text : "");
    fclose(fp);
}

static int mic_fft_clamp_gain_x10(int gain_x10)
{
    if(gain_x10 < MIC_FFT_GAIN_MIN_X10) {
        return MIC_FFT_GAIN_MIN_X10;
    }
    if(gain_x10 > MIC_FFT_GAIN_MAX_X10) {
        return MIC_FFT_GAIN_MAX_X10;
    }
    return gain_x10;
}

static int mic_fft_load_gain_x10(void)
{
    char value[16];
    int gain_x10;

    if(mic_fft_gain_loaded) {
        return mic_fft_gain_x10;
    }

    ui_prefs_get(MIC_FFT_GAIN_PREF, value, sizeof(value), "");
    gain_x10 = value[0] ? atoi(value) : MIC_FFT_GAIN_DEFAULT_X10;
    mic_fft_gain_x10 = mic_fft_clamp_gain_x10(gain_x10);
    mic_fft_gain_loaded = 1;
    return mic_fft_gain_x10;
}

static void mic_fft_store_gain_x10(int gain_x10)
{
    char value[16];

    mic_fft_gain_x10 = mic_fft_clamp_gain_x10(gain_x10);
    snprintf(value, sizeof(value), "%d", mic_fft_gain_x10);
    ui_prefs_set(MIC_FFT_GAIN_PREF, value);
}

static void mic_fft_update_gain_label(void)
{
    char text[32];

    if(!mic_fft_gain_label) {
        return;
    }
    snprintf(text, sizeof(text), "x%.1f", (double)mic_fft_load_gain_x10() / 10.0);
    lv_label_set_text(mic_fft_gain_label, text);
}

static void mic_fft_stop_capture(void)
{
    int status;
    pid_t pid = mic_fft_pid;

    mic_fft_pid = -1;
    if(pid <= 0) {
        return;
    }

    kill(-pid, SIGTERM);
    for(int i = 0; i < 20; i++) {
        if(waitpid(pid, &status, WNOHANG) == pid) {
            return;
        }
        usleep(20000);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, &status, 0);
}

static int mic_fft_start_capture(int *fd_out)
{
    int pipefd[2];
    pid_t pid;

    if(!fd_out) {
        return -1;
    }
    *fd_out = -1;

    if(pipe(pipefd) != 0) {
        mic_fft_set_status("pipe failed");
        return -1;
    }

    pid = fork();
    if(pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        mic_fft_set_status("fork failed");
        return -1;
    }

    if(pid == 0) {
        setpgid(0, 0);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/usr/bin/arecord", "arecord", "-q", "-D", "default",
              "-f", "S16_LE", "-c", "2", "-r", "48000", "-t", "raw",
              "-", (char *)NULL);
        _exit(127);
    }

    setpgid(pid, pid);
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);
    mic_fft_pid = pid;
    *fd_out = pipefd[0];
    return 0;
}

static void mic_fft_prepare_tables(void)
{
    int bits = 0;

    if(mic_fft_tables_ready) {
        return;
    }

    while((1 << bits) < MIC_FFT_FRAMES) {
        bits++;
    }

    for(int i = 0; i < MIC_FFT_FRAMES; i++) {
        uint16_t rev = 0;
        for(int b = 0; b < bits; b++) {
            if(i & (1 << b)) {
                rev |= (uint16_t)(1U << (bits - 1 - b));
            }
        }
        mic_fft_bitrev[i] = rev;
        mic_fft_window[i] = (float)(0.5 - 0.5 *
                            cos((2.0 * M_PI * i) / (MIC_FFT_FRAMES - 1)));
    }

    for(int i = 0; i < MIC_FFT_FRAMES / 2; i++) {
        double angle = (2.0 * M_PI * i) / MIC_FFT_FRAMES;
        mic_fft_twiddle_cos[i] = (float)cos(angle);
        mic_fft_twiddle_sin[i] = (float)-sin(angle);
    }

    for(int i = 0; i < MIC_FFT_BINS; i++) {
        int range = MIC_FFT_MAX_BIN - MIC_FFT_MIN_BIN + 1;
        int start = MIC_FFT_MIN_BIN + (range * i) / MIC_FFT_BINS;
        int end = MIC_FFT_MIN_BIN + (range * (i + 1)) / MIC_FFT_BINS;

        if(start < MIC_FFT_MIN_BIN) {
            start = MIC_FFT_MIN_BIN;
        }
        if(end <= start) {
            end = start + 1;
        }
        if(end > MIC_FFT_MAX_BIN + 1) {
            end = MIC_FFT_MAX_BIN + 1;
        }
        mic_fft_band[i].start = start;
        mic_fft_band[i].end = end;
    }

    mic_fft_tables_ready = 1;
}

static void mic_fft_run(float *real, float *imag)
{
    for(int i = 0; i < MIC_FFT_FRAMES; i++) {
        int j = mic_fft_bitrev[i];
        if(i < j) {
            float tr = real[i];
            float ti = imag[i];
            real[i] = real[j];
            imag[i] = imag[j];
            real[j] = tr;
            imag[j] = ti;
        }
    }

    for(int len = 2; len <= MIC_FFT_FRAMES; len <<= 1) {
        int half = len >> 1;
        int step = MIC_FFT_FRAMES / len;

        for(int base = 0; base < MIC_FFT_FRAMES; base += len) {
            for(int j = 0; j < half; j++) {
                int even = base + j;
                int odd = even + half;
                int tw = j * step;
                float c = mic_fft_twiddle_cos[tw];
                float s = mic_fft_twiddle_sin[tw];
                float tr = c * real[odd] - s * imag[odd];
                float ti = s * real[odd] + c * imag[odd];
                float er = real[even];
                float ei = imag[even];

                real[odd] = er - tr;
                imag[odd] = ei - ti;
                real[even] = er + tr;
                imag[even] = ei + ti;
            }
        }
    }
}

static void mic_fft_compute(const int16_t *samples)
{
    float local[MIC_FFT_BINS];
    float real[MIC_FFT_FRAMES];
    float imag[MIC_FFT_FRAMES];
    float peak = 0.0f;
    double mean = 0.0;
    double rms_acc = 0.0;
    float rms;

    mic_fft_prepare_tables();

    for(int n = 0; n < MIC_FFT_FRAMES; n++) {
        mean += (double)samples[n];
    }
    mean /= (double)MIC_FFT_FRAMES;

    for(int n = 0; n < MIC_FFT_FRAMES; n++) {
        double centered = ((double)samples[n] - mean) / 32768.0;
        real[n] = (float)(centered * mic_fft_window[n]);
        imag[n] = 0.0f;
        rms_acc += centered * centered;
    }
    rms = (float)sqrt(rms_acc / (double)MIC_FFT_FRAMES);

    mic_fft_run(real, imag);

    for(int i = 0; i < MIC_FFT_BINS; i++) {
        double mag_acc = 0.0;
        double mag_peak = 0.0;
        int start = mic_fft_band[i].start;
        int end = mic_fft_band[i].end;
        int count = end - start;
        float level;
        float cleaned;

        for(int k = start; k < end; k++) {
            double mag = sqrt((double)real[k] * real[k] +
                              (double)imag[k] * imag[k]);
            mag_acc += mag;
            if(mag > mag_peak) {
                mag_peak = mag;
            }
        }

        if(count <= 0) {
            count = 1;
        }
        level = (float)(((mag_acc / (double)count) * 0.68 + mag_peak * 0.32) /
                        (double)MIC_FFT_FRAMES);
        level *= 1.0f + ((float)i / (float)(MIC_FFT_BINS - 1)) * 1.35f;
        level = log10f(1.0f + level * 220.0f) / log10f(221.0f);

        if(!mic_fft_noise_ready) {
            mic_fft_noise[i] = level;
        } else if(level < mic_fft_noise[i]) {
            mic_fft_noise[i] = mic_fft_noise[i] * 0.88f + level * 0.12f;
        } else {
            mic_fft_noise[i] = mic_fft_noise[i] * 0.995f + level * 0.005f;
        }

        cleaned = level - mic_fft_noise[i] * 1.08f - 0.006f;
        if(rms < 0.0035f) {
            cleaned *= 0.50f;
        }
        if(cleaned < 0.0f) {
            cleaned = 0.0f;
        }
        local[i] = cleaned * 2.9f;
        if(local[i] > 1.0f) {
            local[i] = 1.0f;
        }
        if(local[i] > peak) {
            peak = local[i];
        }
    }

    mic_fft_noise_ready = 1;

    pthread_mutex_lock(&mic_fft_lock);
    for(int i = 0; i < MIC_FFT_BINS; i++) {
        mic_fft_bins[i] = mic_fft_bins[i] * 0.74f + local[i] * 0.26f;
    }
    mic_fft_peak = mic_fft_peak * 0.78f + peak * 0.22f;
    mic_fft_rms = mic_fft_rms * 0.84f + rms * 0.16f;
    mic_fft_frame_count++;
    mic_fft_status[0] = '\0';
    pthread_mutex_unlock(&mic_fft_lock);
}

static void *mic_fft_thread_main(void *arg)
{
    int fd = -1;
    uint8_t raw[MIC_FFT_FRAMES * MIC_FFT_CHANNELS * 2];
    int16_t mono[MIC_FFT_FRAMES];
    size_t have = 0;

    (void)arg;
    unlink(MIC_FFT_LOG_PATH);
    mic_fft_log("start");
    mic_fft_log("fft=software-radix2-512 source=arecord k230_hw_fft=not_found");
    mic_fft_log("noise=dc-remove hann-window adaptive-floor");
    mic_fft_log("bands=linear avg-peak high-frequency-compensation");

    if(mic_fft_start_capture(&fd) != 0) {
        mic_fft_log("capture start failed");
        mic_fft_running = 0;
        return NULL;
    }

    while(mic_fft_running) {
        fd_set readfds;
        struct timeval tv;
        ssize_t n;

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        if(select(fd + 1, &readfds, NULL, NULL, &tv) <= 0) {
            continue;
        }

        n = read(fd, raw + have, sizeof(raw) - have);
        if(n > 0) {
            have += (size_t)n;
            if(have >= sizeof(raw)) {
                const int16_t *pcm = (const int16_t *)raw;

                for(int i = 0; i < MIC_FFT_FRAMES; i++) {
                    int left = pcm[i * 2];
                    int right = pcm[i * 2 + 1];
                    mono[i] = (int16_t)((left + right) / 2);
                }
                mic_fft_compute(mono);
                have = 0;
            }
        } else if(n == 0 || (errno != EAGAIN && errno != EINTR)) {
            mic_fft_set_status("Capture stopped");
            break;
        }
    }

    if(fd >= 0) {
        close(fd);
    }
    mic_fft_stop_capture();
    mic_fft_log("stop");
    return NULL;
}

static void mic_fft_start(void)
{
    if(mic_fft_thread_valid) {
        return;
    }
    if(access("/usr/bin/arecord", X_OK) != 0) {
        mic_fft_set_status("arecord missing");
        return;
    }

    memset(mic_fft_bins, 0, sizeof(mic_fft_bins));
    memset(mic_fft_noise, 0, sizeof(mic_fft_noise));
    mic_fft_peak = 0.0f;
    mic_fft_rms = 0.0f;
    mic_fft_noise_ready = 0;
    mic_fft_frame_count = 0;
    mic_fft_running = 1;
    if(pthread_create(&mic_fft_thread, NULL, mic_fft_thread_main, NULL) == 0) {
        mic_fft_thread_valid = 1;
        mic_fft_set_status("Starting");
    } else {
        mic_fft_running = 0;
        mic_fft_set_status("thread failed");
    }
}

static void mic_fft_layout_plot(lv_obj_t *plot, int w, int h)
{
    int usable_w = w - 28;
    int slot_w;
    int bar_w;

    if(usable_w < MIC_FFT_BINS * 5) {
        usable_w = MIC_FFT_BINS * 5;
    }
    slot_w = usable_w / MIC_FFT_BINS;
    if(slot_w < 5) {
        slot_w = 5;
    }
    bar_w = slot_w - 5;
    if(bar_w < 4) {
        bar_w = 4;
    }
    if(bar_w > 14) {
        bar_w = 14;
    }

    mic_fft_bar_base_x = 14;
    mic_fft_bar_slot_w = slot_w;
    mic_fft_bar_w = bar_w;
    mic_fft_bar_bottom = 18;
    mic_fft_bar_max_h = h - 36;
    if(mic_fft_bar_max_h < 120) {
        mic_fft_bar_max_h = 120;
    }

    for(int i = 0; i < MIC_FFT_BINS; i++) {
        if(!mic_fft_bar[i]) {
            continue;
        }
        lv_obj_set_width(mic_fft_bar[i], mic_fft_bar_w);
        lv_obj_align(mic_fft_bar[i], LV_ALIGN_BOTTOM_LEFT,
                     mic_fft_bar_base_x + i * mic_fft_bar_slot_w,
                     -mic_fft_bar_bottom);
    }
    (void)plot;
}

static void mic_fft_gain_event_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    lv_event_code_t code = lv_event_get_code(event);
    int gain_x10;

    if(!slider) {
        return;
    }

    gain_x10 = mic_fft_clamp_gain_x10((int)lv_slider_get_value(slider));
    mic_fft_gain_x10 = gain_x10;
    mic_fft_update_gain_label();
    if(code == LV_EVENT_RELEASED || code == LV_EVENT_READY ||
       code == LV_EVENT_DEFOCUSED) {
        mic_fft_store_gain_x10(gain_x10);
    }
    app_request_fast_refresh();
}

static void mic_fft_timer_cb(lv_timer_t *timer)
{
    float bins[MIC_FFT_BINS];
    float peak;
    float rms;
    uint32_t frames;
    char status[128];
    char text[96];
    float gain = (float)mic_fft_load_gain_x10() / 10.0f;
    float peak_display;
    float rms_display;

    (void)timer;
    pthread_mutex_lock(&mic_fft_lock);
    memcpy(bins, mic_fft_bins, sizeof(bins));
    peak = mic_fft_peak;
    rms = mic_fft_rms;
    frames = mic_fft_frame_count;
    snprintf(status, sizeof(status), "%s", mic_fft_status);
    pthread_mutex_unlock(&mic_fft_lock);

    for(int i = 0; i < MIC_FFT_BINS; i++) {
        float shown = bins[i] * gain;
        int h;

        if(shown > 1.0f) {
            shown = 1.0f;
        }
        h = 8 + (int)(shown * (float)mic_fft_bar_max_h);

        if(h > mic_fft_bar_max_h) {
            h = mic_fft_bar_max_h;
        }
        if(mic_fft_bar[i]) {
            lv_obj_set_height(mic_fft_bar[i], h);
            lv_obj_align(mic_fft_bar[i], LV_ALIGN_BOTTOM_LEFT,
                         mic_fft_bar_base_x + i * mic_fft_bar_slot_w,
                         -mic_fft_bar_bottom);
            lv_obj_set_style_bg_color(mic_fft_bar[i],
                                      lv_color_hex(shown > 0.65f ?
                                                   0xEF4D5A :
                                                   shown > 0.35f ?
                                                   0xF5A524 : 0x22D3EE), 0);
        }
    }

    if(mic_fft_status_label) {
        lv_label_set_text(mic_fft_status_label, ui_tr(status));
    }
    if(mic_fft_peak_label) {
        peak_display = peak * gain;
        rms_display = rms * gain;
        if(peak_display > 1.0f) {
            peak_display = 1.0f;
        }
        if(rms_display > 1.0f) {
            rms_display = 1.0f;
        }
        snprintf(text, sizeof(text), "Peak %d%%  RMS %.1f%%",
                 (int)(peak_display * 100.0f),
                 (double)(rms_display * 100.0f));
        lv_label_set_text(mic_fft_peak_label, text);
    }
    if(mic_fft_frames_label) {
        snprintf(text, sizeof(text), "%u frames", frames);
        lv_label_set_text(mic_fft_frames_label, text);
    }
    app_request_fast_refresh();
}

void ui_mic_spectrum_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *plot;
    lv_obj_t *info;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *gain_title;
    int landscape = ui_is_landscape();
    int body_h = ui_body_height(144);
    int content_w = ui_content_width();
    int plot_x = 24;
    int plot_y = landscape ? 24 : 30;
    int plot_w = landscape ? (content_w - 24) / 2 : 520;
    int plot_h = landscape ? body_h - 48 : 600;
    int info_x = landscape ? plot_x + plot_w + 24 : 24;
    int info_y = landscape ? plot_y : 668;
    int info_w = landscape ? content_w - plot_w - 24 : 520;
    int info_h = landscape ? plot_h : 310;
    int gain_y = landscape ? 318 : 224;

    if(plot_h < 260) {
        plot_h = 260;
    }
    if(info_h < 310) {
        info_h = 310;
    }
    mic_fft_load_gain_x10();

    if(!mic_fft_input_route_active) {
        ui_audio_input_route_enter("Mic FFT");
        mic_fft_input_route_active = 1;
    }

    ui_create_header(scr, "Mic FFT");

    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    plot = ui_panel(body, plot_x, plot_y, plot_w, plot_h);
    lv_obj_set_style_bg_color(plot, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_color(plot, lv_color_hex(0x263241), 0);
    lv_obj_set_style_pad_all(plot, 0, 0);

    for(int i = 0; i < MIC_FFT_BINS; i++) {
        mic_fft_bar[i] = lv_obj_create(plot);
        lv_obj_set_size(mic_fft_bar[i], 10, 8);
        lv_obj_set_style_radius(mic_fft_bar[i], 4, 0);
        lv_obj_set_style_bg_color(mic_fft_bar[i], lv_color_hex(0x22D3EE), 0);
        lv_obj_set_style_bg_opa(mic_fft_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(mic_fft_bar[i], 0, 0);
        lv_obj_clear_flag(mic_fft_bar[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    mic_fft_layout_plot(plot, plot_w, plot_h);

    info = ui_panel(body, info_x, info_y, info_w, info_h);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x151B22), 0);
    lv_obj_set_style_border_color(info, lv_color_hex(0x25303A), 0);

    title = ui_label(info, "Live microphone spectrum",
                     &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_width(title, info_w - 32);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);

    subtitle = ui_label(info, "48kHz PCM, 32 frequency bands",
                        &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(subtitle, 0, landscape ? 76 : 54);
    lv_obj_set_width(subtitle, info_w - 32);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);

    mic_fft_status_label = ui_label(info, "Starting", &lv_font_montserrat_18,
                                    0x25C281);
    lv_obj_set_pos(mic_fft_status_label, 0, landscape ? 142 : 100);
    lv_obj_set_width(mic_fft_status_label, info_w - 32);
    lv_label_set_long_mode(mic_fft_status_label, LV_LABEL_LONG_DOT);

    mic_fft_peak_label = ui_label(info, "Peak 0%", &lv_font_montserrat_20,
                                  0xF5A524);
    lv_obj_set_pos(mic_fft_peak_label, 0, landscape ? 202 : 138);
    lv_obj_set_width(mic_fft_peak_label, info_w - 32);
    lv_label_set_long_mode(mic_fft_peak_label, LV_LABEL_LONG_DOT);

    mic_fft_frames_label = ui_label(info, "0 frames", &lv_font_montserrat_20,
                                    0x9AA4AF);
    lv_obj_set_pos(mic_fft_frames_label, 0, landscape ? 252 : 178);
    lv_obj_set_width(mic_fft_frames_label, info_w - 32);
    lv_label_set_long_mode(mic_fft_frames_label, LV_LABEL_LONG_DOT);

    gain_title = ui_label(info, "Gain", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_pos(gain_title, 0, gain_y);
    lv_obj_set_width(gain_title, 180);
    lv_label_set_long_mode(gain_title, LV_LABEL_LONG_DOT);

    mic_fft_gain_label = ui_label(info, "x3.0", &lv_font_montserrat_20,
                                  0x22D3EE);
    lv_obj_set_pos(mic_fft_gain_label, info_w - 132, gain_y);
    lv_obj_set_width(mic_fft_gain_label, 100);
    lv_obj_set_style_text_align(mic_fft_gain_label, LV_TEXT_ALIGN_RIGHT, 0);

    mic_fft_gain_slider = lv_slider_create(info);
    lv_obj_set_pos(mic_fft_gain_slider, 0, gain_y + 50);
    lv_obj_set_size(mic_fft_gain_slider, info_w - 32, 22);
    lv_slider_set_range(mic_fft_gain_slider, MIC_FFT_GAIN_MIN_X10,
                        MIC_FFT_GAIN_MAX_X10);
    lv_slider_set_value(mic_fft_gain_slider, mic_fft_load_gain_x10(),
                        LV_ANIM_OFF);
    lv_obj_set_style_bg_color(mic_fft_gain_slider, lv_color_hex(0x2A3037),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(mic_fft_gain_slider, lv_color_hex(0x22D3EE),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(mic_fft_gain_slider, lv_color_hex(0xF2F5F8),
                              LV_PART_KNOB);
    lv_obj_set_style_height(mic_fft_gain_slider, 18, LV_PART_MAIN);
    lv_obj_set_style_width(mic_fft_gain_slider, 26, LV_PART_KNOB);
    lv_obj_set_style_height(mic_fft_gain_slider, 26, LV_PART_KNOB);
    lv_obj_add_event_cb(mic_fft_gain_slider, mic_fft_gain_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(mic_fft_gain_slider, mic_fft_gain_event_cb,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(mic_fft_gain_slider, mic_fft_gain_event_cb,
                        LV_EVENT_DEFOCUSED, NULL);
    mic_fft_update_gain_label();

    mic_fft_timer = lv_timer_create(mic_fft_timer_cb, 50, NULL);
    mic_fft_start();
    mic_fft_timer_cb(NULL);
}

void ui_mic_spectrum_cleanup(void)
{
    if(mic_fft_timer) {
        lv_timer_delete(mic_fft_timer);
        mic_fft_timer = NULL;
    }
    mic_fft_running = 0;
    mic_fft_stop_capture();
    if(mic_fft_thread_valid) {
        pthread_join(mic_fft_thread, NULL);
        mic_fft_thread_valid = 0;
    }
    if(mic_fft_input_route_active) {
        ui_audio_input_route_leave("Mic FFT");
        mic_fft_input_route_active = 0;
    }

    memset(mic_fft_bar, 0, sizeof(mic_fft_bar));
    mic_fft_status_label = NULL;
    mic_fft_peak_label = NULL;
    mic_fft_frames_label = NULL;
    mic_fft_gain_label = NULL;
    mic_fft_gain_slider = NULL;
}
