// SPDX-License-Identifier: GPL-3.0-only
/*
 * Minimal Xiaozhi keyword spotting helper for T-Display K230.
 *
 * This helper intentionally owns the microphone in a separate process so the
 * launcher can start it only while the Xiaozhi app is visible and stop it on
 * app exit or before PTT recording.
 */

#include "kws.h"

#include <alsa/asoundlib.h>
#include <cstdarg>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <unistd.h>
#include <vector>

#include "mmz.h"

#define DEFAULT_MODEL "/root/app/k230_phone_ui/models/xiaozhi_kws.kmodel"
#define DEFAULT_CAPTURE_DEV "default"
#define DEFAULT_SAMPLE_RATE 16000U
#define DEFAULT_CHANNELS 1U
#define DEFAULT_FRAME_SAMPLES 4800U

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void log_line(const char *tag, const char *fmt, ...)
{
    struct timespec ts;
    struct tm tmv;
    char time_buf[64];
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tmv);
    std::fprintf(stdout, "[%s.%03ld] %s ", time_buf, ts.tv_nsec / 1000000L,
                 tag ? tag : "INFO");
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
}

typedef struct {
    snd_pcm_t *pcm;
    const char *device;
} capture_t;

static int capture_open(capture_t *cap, const char *device)
{
    snd_pcm_hw_params_t *params = NULL;
    unsigned int rate = DEFAULT_SAMPLE_RATE;
    snd_pcm_uframes_t period = DEFAULT_FRAME_SAMPLES;
    int dir = 0;
    int rc;

    std::memset(cap, 0, sizeof(*cap));
    cap->device = device && device[0] ? device : DEFAULT_CAPTURE_DEV;

    rc = snd_pcm_open(&cap->pcm, cap->device, SND_PCM_STREAM_CAPTURE, 0);
    if(rc < 0) {
        log_line("KWS_ERROR", "open capture %s: %s", cap->device,
                 snd_strerror(rc));
        return -1;
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(cap->pcm, params);
    snd_pcm_hw_params_set_access(cap->pcm, params,
                                 SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(cap->pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(cap->pcm, params, DEFAULT_CHANNELS);
    snd_pcm_hw_params_set_rate_near(cap->pcm, params, &rate, &dir);
    snd_pcm_hw_params_set_period_size_near(cap->pcm, params, &period, &dir);

    rc = snd_pcm_hw_params(cap->pcm, params);
    if(rc < 0) {
        log_line("KWS_ERROR", "set capture hw params: %s", snd_strerror(rc));
        snd_pcm_close(cap->pcm);
        cap->pcm = NULL;
        return -1;
    }

    snd_pcm_prepare(cap->pcm);
    log_line("KWS_STARTED", "keyword=小智小智 model_rate=%u capture=%s",
             rate, cap->device);
    return 0;
}

static void capture_close(capture_t *cap)
{
    if(cap && cap->pcm) {
        snd_pcm_drop(cap->pcm);
        snd_pcm_close(cap->pcm);
        cap->pcm = NULL;
    }
}

static int capture_read_4800(capture_t *cap, std::vector<float> &wav)
{
    int16_t pcm[DEFAULT_FRAME_SAMPLES];
    snd_pcm_sframes_t got_total = 0;

    wav.clear();
    wav.reserve(DEFAULT_FRAME_SAMPLES);

    while(got_total < (snd_pcm_sframes_t)DEFAULT_FRAME_SAMPLES && !g_stop) {
        snd_pcm_sframes_t need =
            (snd_pcm_sframes_t)DEFAULT_FRAME_SAMPLES - got_total;
        snd_pcm_sframes_t rc =
            snd_pcm_readi(cap->pcm, pcm + got_total, (snd_pcm_uframes_t)need);

        if(rc == -EPIPE) {
            log_line("KWS_WARN", "capture overrun");
            snd_pcm_prepare(cap->pcm);
            continue;
        }
        if(rc == -ESTRPIPE) {
            snd_pcm_prepare(cap->pcm);
            continue;
        }
        if(rc < 0) {
            log_line("KWS_ERROR", "capture read: %s", snd_strerror((int)rc));
            snd_pcm_prepare(cap->pcm);
            usleep(20000);
            continue;
        }
        if(rc == 0) {
            usleep(1000);
            continue;
        }
        got_total += rc;
    }

    if(got_total != (snd_pcm_sframes_t)DEFAULT_FRAME_SAMPLES) {
        return -1;
    }

    for(size_t i = 0; i < DEFAULT_FRAME_SAMPLES; ++i) {
        wav.push_back((float)pcm[i]);
    }
    return 0;
}

static void print_usage(const char *name)
{
    std::fprintf(stderr,
                 "Usage: %s [--model PATH] [--capture DEV] "
                 "[--threshold N] [--debug N]\n",
                 name);
}

void __attribute__((destructor)) cleanup()
{
    shrink_memory_pool();
    kd_mpi_mmz_deinit();
}

int main(int argc, char **argv)
{
    const char *model = DEFAULT_MODEL;
    const char *capture = DEFAULT_CAPTURE_DEV;
    float threshold = 0.50f;
    int debug = 0;

    for(int i = 1; i < argc; ++i) {
        if(std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if(std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capture = argv[++i];
        } else if(std::strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold = (float)std::atof(argv[++i]);
        } else if(std::strcmp(argv[i], "--debug") == 0 && i + 1 < argc) {
            debug = std::atoi(argv[++i]);
        } else if(std::strcmp(argv[i], "-h") == 0 ||
                  std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if(access(model, R_OK) != 0) {
        log_line("KWS_ERROR", "model missing: %s", model);
        return 1;
    }

    capture_t cap;
    if(capture_open(&cap, capture) != 0) {
        return 1;
    }

    int rc = 0;
    try {
        KWS kws(model, "xiaozhi", 2, threshold, debug);
        std::string last = "Deactivated!";
        std::string cur = "Deactivated!";
        std::vector<float> wav;
        uint64_t frames = 0;
        uint64_t waiting_log_div = 0;

        while(!g_stop) {
            if(capture_read_4800(&cap, wav) != 0) {
                rc = 1;
                break;
            }
            frames++;

            if(!kws.pre_process(wav)) {
                continue;
            }
            kws.inference();
            cur = kws.post_process();

            if(cur != "Deactivated!") {
                log_line("KWS_SCORE", "frame=%llu result=%s",
                         (unsigned long long)frames, cur.c_str());
            } else if((frames / 4U) != waiting_log_div) {
                waiting_log_div = frames / 4U;
                log_line("KWS_WAITING", "keyword=小智小智 frames=%llu",
                         (unsigned long long)frames);
            }

            if(last != "Deactivated!" && cur == "Deactivated!") {
                log_line("KWS_DETECTED", "keyword=小智小智 frames=%llu",
                         (unsigned long long)frames);
                last = cur;
                usleep(800000);
                continue;
            }
            last = cur;
        }
    } catch(const std::exception &e) {
        log_line("KWS_ERROR", "exception: %s", e.what());
        rc = 1;
    } catch(...) {
        log_line("KWS_ERROR", "unknown exception");
        rc = 1;
    }

    capture_close(&cap);
    log_line("KWS_STOPPED", "rc=%d", rc);
    return rc;
}
