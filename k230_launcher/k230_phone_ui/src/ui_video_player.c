#include "ui_video_player.h"

#include "ui_audio.h"
#include "ui_common.h"
#include "ui_hardware.h"
#include "ui_i18n.h"

#include <dirent.h>
#include <errno.h>
#include <lvgl/src/misc/cache/instance/lv_image_cache.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define VIDEO_DIR "/root/videos"
#define VIDEO_MAX_ITEMS 24
#define VIDEO_LOG_PATH "/tmp/k230_video_player.log"
#define VIDEO_THUMB_W 160
#define VIDEO_THUMB_H 100
#define VIDEO_THUMB_BYTES (VIDEO_THUMB_W * VIDEO_THUMB_H * 2)
#define VIDEO_APP_TOP 54
#define VIDEO_PCM_RATE 48000
#define VIDEO_APLAY_BUFFER_US 50000
#define VIDEO_APLAY_PERIOD_US 10000
#define VIDEO_AUDIO_PREROLL_US 65000
#define VIDEO_LATE_DROP_US 180000
#define VIDEO_MAX_CONSECUTIVE_DROPS 2
#define VIDEO_SYNC_SLEEP_CHUNK_US 20000
#define VIDEO_PCM_VOLUME_BIN "/root/app/k230_phone_ui/k230_pcm_volume"
#define VIDEO_VOLUME_CONFIG_PATH "/root/.k230_phone_audio_volume"

typedef struct {
    char path[256];
    char name[128];
    off_t size;
    int thumb_ready;
    lv_image_dsc_t thumb_dsc;
} video_item_t;

typedef struct {
    char path[256];
    int view_w;
    int view_h;
} video_job_t;

static pthread_mutex_t video_lock = PTHREAD_MUTEX_INITIALIZER;
static video_item_t video_items[VIDEO_MAX_ITEMS];
static uint8_t video_thumb_pixels[VIDEO_MAX_ITEMS][VIDEO_THUMB_BYTES];
static int video_count;
static int video_selected;
static int video_fullscreen;
static char video_selected_path[256];
static volatile int video_stop_requested;
static int video_thread_valid;
static volatile int video_thread_done;
static pthread_t video_thread;
static pid_t video_audio_pid = -1;
static lv_timer_t *video_timer;
static lv_obj_t *video_status_label;
static lv_obj_t *video_meta_label;
static lv_obj_t *video_image;
static lv_obj_t *video_placeholder;
static char video_status[192] = "Ready";
static char video_meta[192] = "/root/videos";
static uint8_t *video_pixels[2];
static uint8_t *video_display_pixels;
static int video_front_idx;
static int video_back_idx = 1;
static uint32_t video_frame_seq;
static uint32_t video_render_seq;
static int video_frame_w;
static int video_frame_h;
static lv_image_dsc_t video_image_dsc;
static int32_t video_press_x;
static int32_t video_press_y;

static void video_log(const char *fmt, ...)
{
    FILE *log = fopen(VIDEO_LOG_PATH, "a");
    va_list ap;

    if(!log) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(log, fmt, ap);
    va_end(ap);
    fputc('\n', log);
    fclose(log);
}

static int video_is_supported(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;

    if(!dot) {
        return 0;
    }
    return strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".m4v") == 0 ||
           strcasecmp(dot, ".mov") == 0 || strcasecmp(dot, ".mkv") == 0 ||
           strcasecmp(dot, ".h265") == 0 || strcasecmp(dot, ".hevc") == 0 ||
           strcasecmp(dot, ".h264") == 0 || strcasecmp(dot, ".264") == 0;
}

static void video_format_size(off_t size, char *buf, size_t len)
{
    if(size >= 1024 * 1024) {
        snprintf(buf, len, "%.1f MB", (double)size / (1024.0 * 1024.0));
    } else {
        snprintf(buf, len, "%.0f KB", (double)size / 1024.0);
    }
}

static void video_set_status(const char *status, const char *meta)
{
    pthread_mutex_lock(&video_lock);
    snprintf(video_status, sizeof(video_status), "%s", status ? status : "");
    if(meta) {
        snprintf(video_meta, sizeof(video_meta), "%s", meta);
    }
    pthread_mutex_unlock(&video_lock);
    app_request_fast_refresh();
}

static int video_has_command(const char *path)
{
    return path && access(path, X_OK) == 0;
}

static int video_shell_quote(const char *src, char *dst, size_t dst_len)
{
    size_t used = 0;

    if(!src || !dst || dst_len < 3) {
        return -1;
    }
    dst[used++] = '\'';
    for(const char *p = src; *p; p++) {
        if(*p == '\'') {
            if(used + 4 >= dst_len) {
                return -1;
            }
            memcpy(dst + used, "'\\''", 4);
            used += 4;
        } else {
            if(used + 1 >= dst_len) {
                return -1;
            }
            dst[used++] = *p;
        }
    }
    if(used + 2 > dst_len) {
        return -1;
    }
    dst[used++] = '\'';
    dst[used] = '\0';
    return 0;
}

static void video_stop_audio(void)
{
    int status;

    if(video_audio_pid <= 0) {
        return;
    }

    kill(-video_audio_pid, SIGTERM);
    for(int i = 0; i < 10; i++) {
        if(waitpid(video_audio_pid, &status, WNOHANG) == video_audio_pid) {
            video_audio_pid = -1;
            return;
        }
        usleep(20000);
    }

    kill(-video_audio_pid, SIGKILL);
    waitpid(video_audio_pid, &status, 0);
    video_audio_pid = -1;
}

static void video_reap_audio(void)
{
    int status;

    if(video_audio_pid <= 0) {
        return;
    }
    if(waitpid(video_audio_pid, &status, WNOHANG) == video_audio_pid) {
        video_audio_pid = -1;
    }
}

static int video_start_audio_for_path(const char *path)
{
    char quoted[512];
    char cmd[1400];
    pid_t pid;
    int external;

    video_stop_audio();
    if(!path || !path[0]) {
        return 0;
    }
    if(!video_has_command("/usr/bin/ffmpeg") ||
       !video_has_command("/usr/bin/aplay") ||
       !video_has_command(VIDEO_PCM_VOLUME_BIN)) {
        video_set_status("Video audio helper missing", NULL);
        return 0;
    }
    if(video_shell_quote(path, quoted, sizeof(quoted)) != 0) {
        video_set_status("Video path error", NULL);
        return 0;
    }

    ui_audio_stop_for_exclusive_app("Video playback");
    external = ui_audio_output_is_external();
    ui_audio_output_set_external(external ? 1 : 0);
    ui_audio_set_volume_value(ui_audio_get_volume_value(), 1);

    snprintf(cmd, sizeof(cmd),
             "ffmpeg -nostdin -hide_banner -loglevel error -i %s "
             "-vn -f s16le -acodec pcm_s16le -ac 2 -ar %d - "
             "2>>" VIDEO_LOG_PATH " | "
             VIDEO_PCM_VOLUME_BIN " " VIDEO_VOLUME_CONFIG_PATH
             " 2>>" VIDEO_LOG_PATH " | "
             "aplay -q -D default -t raw -f S16_LE -c 2 -r %d "
             "-B %d -F %d - >>" VIDEO_LOG_PATH " 2>&1",
             quoted, VIDEO_PCM_RATE, VIDEO_PCM_RATE, VIDEO_APLAY_BUFFER_US,
             VIDEO_APLAY_PERIOD_US);

    pid = fork();
    if(pid < 0) {
        video_set_status("Video audio fork failed", NULL);
        return 0;
    }
    if(pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    video_audio_pid = pid;
    video_log("audio pid=%d path=%s", (int)pid, path);
    return 1;
}

static int64_t video_frame_duration_us(AVStream *stream, AVCodecContext *ctx)
{
    AVRational rate = {0, 1};
    int64_t duration;

    if(stream) {
        if(stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
            rate = stream->avg_frame_rate;
        } else if(stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
            rate = stream->r_frame_rate;
        }
    }
    if(rate.num <= 0 && ctx && ctx->framerate.num > 0 && ctx->framerate.den > 0) {
        rate = ctx->framerate;
    }
    if(rate.num <= 0 || rate.den <= 0) {
        return 33333;
    }

    duration = av_rescale_q(1, (AVRational){ rate.den, rate.num },
                            AV_TIME_BASE_Q);
    if(duration < 5000 || duration > 200000) {
        return 33333;
    }
    return duration;
}

static int64_t video_frame_pts_us(AVFrame *frame, AVStream *stream,
                                  int frame_index, int64_t fallback_us)
{
    int64_t pts;

    if(!frame || !stream) {
        return (int64_t)frame_index * fallback_us;
    }
    pts = frame->best_effort_timestamp;
    if(pts == AV_NOPTS_VALUE) {
        pts = frame->pts;
    }
    if(pts == AV_NOPTS_VALUE) {
        return (int64_t)frame_index * fallback_us;
    }
    return av_rescale_q(pts, stream->time_base, AV_TIME_BASE_Q);
}

static void video_wait_until_us(int64_t target_us)
{
    while(!video_stop_requested) {
        int64_t now = ui_monotonic_us();
        int64_t wait_us = target_us - now;

        if(wait_us <= 1000) {
            return;
        }
        if(wait_us > VIDEO_SYNC_SLEEP_CHUNK_US) {
            wait_us = VIDEO_SYNC_SLEEP_CHUNK_US;
        }
        usleep((useconds_t)wait_us);
    }
}

static void video_fit_size(int src_w, int src_h, int box_w, int box_h,
                           int *dst_w, int *dst_h)
{
    int w = box_w;
    int h;

    if(src_w <= 0 || src_h <= 0 || box_w <= 0 || box_h <= 0) {
        *dst_w = box_w > 0 ? box_w : 320;
        *dst_h = box_h > 0 ? box_h : 180;
        return;
    }

    h = (int)((int64_t)w * src_h / src_w);
    if(h > box_h) {
        h = box_h;
        w = (int)((int64_t)h * src_w / src_h);
    }
    if(w < 16) {
        w = 16;
    }
    if(h < 16) {
        h = 16;
    }
    *dst_w = w;
    *dst_h = h;
}

static int video_decode_thumbnail(const char *path, uint8_t *dst,
                                  int box_w, int box_h)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *ctx = NULL;
    AVCodec *codec = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws = NULL;
    int stream_index;
    int dst_w = 0;
    int dst_h = 0;
    int rc = -1;

    if(!path || !dst || box_w <= 0 || box_h <= 0) {
        return -1;
    }

    memset(dst, 0, (size_t)box_w * (size_t)box_h * 2U);
    if(avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        goto out;
    }
    if(avformat_find_stream_info(fmt, NULL) < 0) {
        goto out;
    }
    stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
                                       &codec, 0);
    if(stream_index < 0 || !codec) {
        goto out;
    }
    ctx = avcodec_alloc_context3(codec);
    if(!ctx ||
       avcodec_parameters_to_context(ctx, fmt->streams[stream_index]->codecpar) < 0 ||
       avcodec_open2(ctx, codec, NULL) < 0) {
        goto out;
    }

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if(!frame || !pkt) {
        goto out;
    }

    while(av_read_frame(fmt, pkt) >= 0) {
        if(pkt->stream_index == stream_index &&
           avcodec_send_packet(ctx, pkt) == 0) {
            while(avcodec_receive_frame(ctx, frame) == 0) {
                int xoff;
                int yoff;
                uint8_t *dst_data[4];
                int dst_linesize[4];

                video_fit_size(ctx->width, ctx->height, box_w, box_h,
                               &dst_w, &dst_h);
                xoff = (box_w - dst_w) / 2;
                yoff = (box_h - dst_h) / 2;
                sws = sws_getContext(ctx->width, ctx->height, ctx->pix_fmt,
                                     dst_w, dst_h, AV_PIX_FMT_RGB565LE,
                                     SWS_FAST_BILINEAR, NULL, NULL, NULL);
                if(!sws) {
                    goto out;
                }
                dst_data[0] = dst + (size_t)yoff * (size_t)box_w * 2U +
                              (size_t)xoff * 2U;
                dst_data[1] = dst_data[2] = dst_data[3] = NULL;
                dst_linesize[0] = box_w * 2;
                dst_linesize[1] = dst_linesize[2] = dst_linesize[3] = 0;
                sws_scale(sws, (const uint8_t *const *)frame->data,
                          frame->linesize, 0, ctx->height, dst_data,
                          dst_linesize);
                rc = 0;
                goto out;
            }
        }
        av_packet_unref(pkt);
    }

out:
    if(pkt) {
        av_packet_free(&pkt);
    }
    if(sws) {
        sws_freeContext(sws);
    }
    if(frame) {
        av_frame_free(&frame);
    }
    if(ctx) {
        avcodec_free_context(&ctx);
    }
    if(fmt) {
        avformat_close_input(&fmt);
    }
    return rc;
}

static void video_prepare_thumb(int index)
{
    if(index < 0 || index >= VIDEO_MAX_ITEMS) {
        return;
    }

    memset(&video_items[index].thumb_dsc, 0,
           sizeof(video_items[index].thumb_dsc));
    video_items[index].thumb_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    video_items[index].thumb_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    video_items[index].thumb_dsc.header.w = VIDEO_THUMB_W;
    video_items[index].thumb_dsc.header.h = VIDEO_THUMB_H;
    video_items[index].thumb_dsc.header.stride = VIDEO_THUMB_W * 2U;
    video_items[index].thumb_dsc.data_size = VIDEO_THUMB_BYTES;
    video_items[index].thumb_dsc.data = video_thumb_pixels[index];
    video_items[index].thumb_ready =
        video_decode_thumbnail(video_items[index].path, video_thumb_pixels[index],
                               VIDEO_THUMB_W, VIDEO_THUMB_H) == 0;
}

static void video_scan_files(void)
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;
    int selected = -1;

    mkdir(VIDEO_DIR, 0755);
    memset(video_items, 0, sizeof(video_items));
    memset(video_thumb_pixels, 0, sizeof(video_thumb_pixels));

    dir = opendir(VIDEO_DIR);
    if(!dir) {
        video_set_status("Video directory missing", VIDEO_DIR);
        video_count = 0;
        video_selected = -1;
        return;
    }

    while((entry = readdir(dir)) != NULL && count < VIDEO_MAX_ITEMS) {
        struct stat st;
        char path[sizeof(video_items[0].path)];

        if(entry->d_name[0] == '.' || !video_is_supported(entry->d_name)) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", VIDEO_DIR, entry->d_name);
        if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        snprintf(video_items[count].path, sizeof(video_items[count].path), "%s",
                 path);
        snprintf(video_items[count].name, sizeof(video_items[count].name), "%s",
                 entry->d_name);
        video_items[count].size = st.st_size;
        video_prepare_thumb(count);
        count++;
    }
    closedir(dir);

    for(int i = 0; i < count; i++) {
        if(video_selected_path[0] &&
           strcmp(video_selected_path, video_items[i].path) == 0) {
            selected = i;
            break;
        }
    }
    if(selected < 0) {
        if(video_selected >= 0 && video_selected < count) {
            selected = video_selected;
        } else {
            selected = count > 0 ? 0 : -1;
        }
    }
    if(selected >= 0 && selected < count) {
        snprintf(video_selected_path, sizeof(video_selected_path), "%s",
                 video_items[selected].path);
    } else {
        video_selected_path[0] = '\0';
    }

    pthread_mutex_lock(&video_lock);
    video_count = count;
    video_selected = selected;
    snprintf(video_status, sizeof(video_status), "%s",
             count ? "Ready" : "No videos");
    snprintf(video_meta, sizeof(video_meta), "%d video%s in /root/videos",
             count, count == 1 ? "" : "s");
    pthread_mutex_unlock(&video_lock);
}

static void video_stop_playback(void)
{
    video_stop_audio();
    if(!video_thread_valid) {
        return;
    }
    video_stop_requested = 1;
    pthread_join(video_thread, NULL);
    video_thread_valid = 0;
    video_thread_done = 0;
}

static void video_reap_finished_thread(void)
{
    if(video_thread_valid && video_thread_done) {
        pthread_join(video_thread, NULL);
        video_thread_valid = 0;
        video_thread_done = 0;
    }
}

static int video_decode_one_file(const char *path, int view_w, int view_h)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *ctx = NULL;
    AVCodec *codec = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws = NULL;
    int stream_index;
    int audio_stream_index;
    AVStream *video_stream;
    int dst_w = 0;
    int dst_h = 0;
    int rc = -1;
    int presented = 0;
    int frames_seen = 0;
    int dropped = 0;
    int consecutive_drops = 0;
    int audio_started = 0;
    int64_t fallback_frame_us;
    int64_t first_pts_us = AV_NOPTS_VALUE;
    int64_t playback_start_us = 0;
    char meta[192];

    if(avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        video_set_status("Open video failed", path);
        return -1;
    }
    if(avformat_find_stream_info(fmt, NULL) < 0) {
        video_set_status("Read stream info failed", path);
        goto out;
    }
    stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
                                       &codec, 0);
    if(stream_index < 0 || !codec) {
        video_set_status("No video stream", path);
        goto out;
    }
    video_stream = fmt->streams[stream_index];
    audio_stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1,
                                             stream_index, NULL, 0);
    ctx = avcodec_alloc_context3(codec);
    if(!ctx ||
       avcodec_parameters_to_context(ctx, fmt->streams[stream_index]->codecpar) < 0 ||
       avcodec_open2(ctx, codec, NULL) < 0) {
        video_set_status("Open decoder failed", codec ? codec->name : "decoder");
        goto out;
    }

    video_fit_size(ctx->width, ctx->height, view_w, view_h, &dst_w, &dst_h);
    if(!video_pixels[0] || !video_pixels[1]) {
        video_set_status("Video frame buffer missing", NULL);
        goto out;
    }
    snprintf(meta, sizeof(meta), "%s  %dx%d -> %dx%d", codec->name,
             ctx->width, ctx->height, dst_w, dst_h);
    video_set_status("Playing", meta);
    video_log("decode codec=%s src=%dx%d dst=%dx%d view=%dx%d audio=%s",
              codec->name, ctx->width, ctx->height, dst_w, dst_h, view_w,
              view_h, audio_stream_index >= 0 ? "yes" : "no");

    pthread_mutex_lock(&video_lock);
    video_frame_w = dst_w;
    video_frame_h = dst_h;
    video_front_idx = 0;
    video_back_idx = 1;
    pthread_mutex_unlock(&video_lock);

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if(!frame || !pkt) {
        video_set_status("Allocate decoder frame failed", NULL);
        goto out;
    }
    sws = sws_getContext(ctx->width, ctx->height, ctx->pix_fmt, dst_w, dst_h,
                         AV_PIX_FMT_RGB565LE, SWS_FAST_BILINEAR, NULL, NULL,
                         NULL);
    if(!sws) {
        video_set_status("Scale setup failed", NULL);
        goto out;
    }
    fallback_frame_us = video_frame_duration_us(video_stream, ctx);
    playback_start_us = ui_monotonic_us();

    while(!video_stop_requested && av_read_frame(fmt, pkt) >= 0) {
        if(pkt->stream_index == stream_index &&
           avcodec_send_packet(ctx, pkt) == 0) {
            while(!video_stop_requested &&
                  avcodec_receive_frame(ctx, frame) == 0) {
                int back;
                uint8_t *dst_data[4];
                int dst_linesize[4];
                int frame_index = frames_seen++;
                int64_t pts_us = video_frame_pts_us(frame, video_stream,
                                                    frame_index,
                                                    fallback_frame_us);
                int64_t relative_us;
                int64_t target_us;
                int64_t now_us;

                if(first_pts_us == AV_NOPTS_VALUE) {
                    first_pts_us = pts_us;
                    if(audio_stream_index >= 0) {
                        audio_started = video_start_audio_for_path(path);
                    }
                    playback_start_us = ui_monotonic_us() +
                                        (audio_started ? VIDEO_AUDIO_PREROLL_US : 0);
                    video_log("first frame pts=%lld fallback=%lld",
                              (long long)first_pts_us,
                              (long long)fallback_frame_us);
                }
                relative_us = pts_us - first_pts_us;
                if(relative_us < 0) {
                    relative_us = (int64_t)frame_index * fallback_frame_us;
                }
                target_us = playback_start_us + relative_us;
                now_us = ui_monotonic_us();
                if(audio_started && frame_index > 0 &&
                   now_us - target_us > VIDEO_LATE_DROP_US &&
                   consecutive_drops < VIDEO_MAX_CONSECUTIVE_DROPS) {
                    dropped++;
                    consecutive_drops++;
                    continue;
                }
                if(audio_started && now_us - target_us > VIDEO_LATE_DROP_US) {
                    target_us = now_us;
                }
                consecutive_drops = 0;

                pthread_mutex_lock(&video_lock);
                back = video_back_idx;
                pthread_mutex_unlock(&video_lock);

                dst_data[0] = video_pixels[back];
                dst_data[1] = dst_data[2] = dst_data[3] = NULL;
                dst_linesize[0] = dst_w * 2;
                dst_linesize[1] = dst_linesize[2] = dst_linesize[3] = 0;
                sws_scale(sws, (const uint8_t *const *)frame->data,
                          frame->linesize, 0, ctx->height, dst_data,
                          dst_linesize);

                video_wait_until_us(target_us);
                if(video_stop_requested) {
                    break;
                }

                pthread_mutex_lock(&video_lock);
                video_front_idx = back;
                video_back_idx = back ? 0 : 1;
                video_frame_seq++;
                pthread_mutex_unlock(&video_lock);
                presented++;
                if(presented == 1 || (presented % 30) == 0) {
                    video_log("frame shown=%d dropped=%d seq=%u rel=%lld",
                              presented, dropped, video_frame_seq,
                              (long long)relative_us);
                }
            }
        }
        av_packet_unref(pkt);
    }

    rc = 0;
    if(dropped > 0) {
        snprintf(meta, sizeof(meta), "%d frame%s shown, %d dropped",
                 presented, presented == 1 ? "" : "s", dropped);
    } else {
        snprintf(meta, sizeof(meta), "%d frame%s shown", presented,
                 presented == 1 ? "" : "s");
    }
    video_log("decode done status=%s presented=%d dropped=%d",
              video_stop_requested ? "stopped" : "completed", presented,
              dropped);
    video_set_status(video_stop_requested ? "Stopped" : "Completed", meta);

out:
    if(pkt) {
        av_packet_free(&pkt);
    }
    if(sws) {
        sws_freeContext(sws);
    }
    if(frame) {
        av_frame_free(&frame);
    }
    if(ctx) {
        avcodec_free_context(&ctx);
    }
    if(fmt) {
        avformat_close_input(&fmt);
    }
    return rc;
}

static void *video_thread_main(void *arg)
{
    video_job_t *job = (video_job_t *)arg;
    FILE *log = fopen(VIDEO_LOG_PATH, "a");

    if(log) {
        fprintf(log, "play %s view=%dx%d\n", job->path, job->view_w,
                job->view_h);
        fclose(log);
    }
    video_decode_one_file(job->path, job->view_w, job->view_h);
    free(job);
    video_stop_requested = 0;
    video_thread_done = 1;
    return NULL;
}

static void video_start_selected(int view_w, int view_h)
{
    video_job_t *job;
    int selected;

    video_reap_finished_thread();
    if(video_thread_valid) {
        return;
    }

    pthread_mutex_lock(&video_lock);
    selected = video_selected;
    pthread_mutex_unlock(&video_lock);
    if(selected < 0 || selected >= video_count) {
        video_set_status("Select a video first", NULL);
        return;
    }

    job = calloc(1, sizeof(*job));
    if(!job) {
        video_set_status("Start thread failed", NULL);
        return;
    }
    snprintf(job->path, sizeof(job->path), "%s", video_items[selected].path);
    job->view_w = view_w;
    job->view_h = view_h;
    video_stop_requested = 0;
    video_thread_done = 0;
    if(pthread_create(&video_thread, NULL, video_thread_main, job) == 0) {
        video_thread_valid = 1;
    } else {
        free(job);
        video_set_status("Start thread failed", NULL);
    }
}

static void video_timer_cb(lv_timer_t *timer)
{
    char status[192];
    char meta[192];
    uint32_t seq;
    int front;
    int frame_w;
    int frame_h;
    int copied = 0;

    (void)timer;
    video_reap_finished_thread();
    video_reap_audio();

    pthread_mutex_lock(&video_lock);
    snprintf(status, sizeof(status), "%s", video_status);
    snprintf(meta, sizeof(meta), "%s", video_meta);
    seq = video_frame_seq;
    front = video_front_idx;
    frame_w = video_frame_w;
    frame_h = video_frame_h;
    if(seq != video_render_seq && frame_w > 0 && frame_h > 0 &&
       video_pixels[front] && video_display_pixels) {
        memcpy(video_display_pixels, video_pixels[front],
               (size_t)frame_w * (size_t)frame_h * 2U);
        copied = 1;
    }
    pthread_mutex_unlock(&video_lock);

    if(video_status_label) {
        lv_label_set_text(video_status_label, ui_tr(status));
    }
    if(video_meta_label) {
        lv_label_set_text(video_meta_label, meta);
    }
    if(video_image && copied) {
        memset(&video_image_dsc, 0, sizeof(video_image_dsc));
        video_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        video_image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        video_image_dsc.header.w = (uint32_t)frame_w;
        video_image_dsc.header.h = (uint32_t)frame_h;
        video_image_dsc.header.stride = (uint32_t)frame_w * 2U;
        video_image_dsc.data_size = (uint32_t)frame_w * (uint32_t)frame_h * 2U;
        video_image_dsc.data = video_display_pixels;
        lv_image_cache_drop(&video_image_dsc);
        lv_image_set_src(video_image, &video_image_dsc);
        lv_obj_align(video_image, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(video_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(video_image);
        if(video_placeholder) {
            lv_obj_add_flag(video_placeholder, LV_OBJ_FLAG_HIDDEN);
        }
        video_render_seq = seq;
    }
}

static void video_switch_relative(int delta)
{
    int next;

    if(video_count <= 1 || delta == 0) {
        return;
    }

    next = video_selected + delta;
    if(next < 0) {
        next = video_count - 1;
    } else if(next >= video_count) {
        next = 0;
    }

    video_stop_playback();
    video_selected = next;
    snprintf(video_selected_path, sizeof(video_selected_path), "%s",
             video_items[next].path);
    video_fullscreen = 1;
    app_refresh_current_page();
}

static void video_swipe_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;

    if(!indev) {
        return;
    }
    lv_indev_get_point(indev, &point);

    if(code == LV_EVENT_PRESSED) {
        video_press_x = point.x;
        video_press_y = point.y;
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        int32_t dx = point.x - video_press_x;
        int32_t dy = point.y - video_press_y;

        if(llabs((long long)dx) > 90 && llabs((long long)dy) < 100) {
            video_switch_relative(dx < 0 ? 1 : -1);
        }
    }
}

static void video_open_fullscreen_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if(index < 0 || index >= video_count) {
        return;
    }
    video_selected = index;
    snprintf(video_selected_path, sizeof(video_selected_path), "%s",
             video_items[index].path);
    video_fullscreen = 1;
    app_refresh_current_page();
}

static void video_grid_back_event_cb(lv_event_t *event)
{
    (void)event;
    video_fullscreen = 0;
    video_stop_playback();
    app_refresh_current_page();
}

static void video_stop_event_cb(lv_event_t *event)
{
    (void)event;
    video_stop_playback();
    video_set_status("Stopped", NULL);
}

static void video_create_grid(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *count_label;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int body_h = screen_h - VIDEO_APP_TOP;
    int cols = (screen_w - 32) / 188;
    int tile_w;

    if(cols < 2) {
        cols = 2;
    }
    tile_w = (screen_w - 48 - (cols - 1) * 12) / cols;
    if(tile_w < 156) {
        tile_w = 156;
    }
    if(tile_w > 188) {
        tile_w = 188;
    }

    body = lv_obj_create(scr);
    lv_obj_set_pos(body, 0, VIDEO_APP_TOP);
    lv_obj_set_size(body, screen_w, body_h);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    count_label = ui_label(body, "", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(count_label, screen_w - 48);
    lv_obj_align(count_label, LV_ALIGN_TOP_LEFT, 24, 18);
    if(video_count > 0) {
        char text[64];

        snprintf(text, sizeof(text), "%d video%s", video_count,
                 video_count == 1 ? "" : "s");
        lv_label_set_text(count_label, text);
    } else {
        lv_label_set_text(count_label, ui_tr("No videos"));
    }

    if(video_count == 0) {
        lv_obj_t *empty = ui_label(body, "No MP4 files in /root/videos",
                                   &lv_font_montserrat_24, 0xF2F5F8);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -40);
    }

    for(int i = 0; i < video_count; i++) {
        int col = i % cols;
        int row = i / cols;
        int x = 24 + col * (tile_w + 12);
        int y = 58 + row * 166;
        lv_obj_t *tile = ui_panel(body, x, y, tile_w, 150);
        lv_obj_t *thumb_box;
        lv_obj_t *caption;
        lv_obj_t *meta;
        char size_text[48];

        lv_obj_set_style_pad_all(tile, 4, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x111820), 0);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, video_open_fullscreen_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);

        thumb_box = lv_obj_create(tile);
        lv_obj_set_pos(thumb_box, (tile_w - VIDEO_THUMB_W) / 2, 0);
        lv_obj_set_size(thumb_box, VIDEO_THUMB_W, VIDEO_THUMB_H);
        lv_obj_set_style_radius(thumb_box, 6, 0);
        lv_obj_set_style_bg_color(thumb_box, lv_color_hex(0x030507), 0);
        lv_obj_set_style_bg_opa(thumb_box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(thumb_box, 0, 0);
        lv_obj_clear_flag(thumb_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(thumb_box, LV_OBJ_FLAG_CLICKABLE |
                        LV_OBJ_FLAG_EVENT_BUBBLE);

        if(video_items[i].thumb_ready) {
            lv_obj_t *img = lv_image_create(thumb_box);
            lv_image_set_src(img, &video_items[i].thumb_dsc);
            lv_obj_center(img);
            lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE |
                            LV_OBJ_FLAG_EVENT_BUBBLE);
        } else {
            lv_obj_t *icon = ui_label(thumb_box, LV_SYMBOL_VIDEO,
                                      &lv_font_montserrat_32, 0x64748B);
            lv_obj_center(icon);
            ui_make_click_forwarder(icon);
        }

        caption = ui_label(tile, video_items[i].name, &lv_font_montserrat_14,
                           0xF2F5F8);
        lv_obj_set_width(caption, tile_w - 12);
        lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
        lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -22);
        ui_make_click_forwarder(caption);

        video_format_size(video_items[i].size, size_text, sizeof(size_text));
        meta = ui_label(tile, size_text, &lv_font_montserrat_12, 0x9AA4AF);
        lv_obj_set_width(meta, tile_w - 12);
        lv_obj_set_style_text_align(meta, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(meta, LV_ALIGN_BOTTOM_MID, 0, -4);
        ui_make_click_forwarder(meta);
    }

    ui_create_header(scr, "Video");
}

static void video_create_fullscreen(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *back;
    lv_obj_t *stop;
    lv_obj_t *name_label;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int body_h = screen_h - VIDEO_APP_TOP;
    int view_w = screen_w;
    int view_h = body_h - 42;

    if(view_h < 220) {
        view_h = body_h;
    }

    body = lv_obj_create(scr);
    lv_obj_set_pos(body, 0, VIDEO_APP_TOP);
    lv_obj_set_size(body, screen_w, body_h);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, video_swipe_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(body, video_swipe_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(body, video_swipe_event_cb, LV_EVENT_PRESS_LOST, NULL);

    video_frame_w = view_w;
    video_frame_h = view_h;
    video_frame_seq = 0;
    video_render_seq = 0;
    video_pixels[0] = calloc(1, (size_t)view_w * (size_t)view_h * 2U);
    video_pixels[1] = calloc(1, (size_t)view_w * (size_t)view_h * 2U);
    video_display_pixels = calloc(1, (size_t)view_w * (size_t)view_h * 2U);
    if(!video_pixels[0] || !video_pixels[1] || !video_display_pixels) {
        video_set_status("Video frame buffer missing", NULL);
        video_log("frame buffer allocation failed view=%dx%d", view_w, view_h);
    }

    video_image = lv_image_create(body);
    lv_obj_align(video_image, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(video_image, LV_OBJ_FLAG_HIDDEN);

    video_placeholder = ui_label(body, "Loading video",
                                 &lv_font_montserrat_22, 0x64748B);
    lv_obj_center(video_placeholder);

    back = lv_obj_create(body);
    lv_obj_set_pos(back, 18, 18);
    lv_obj_set_size(back, 46, 46);
    lv_obj_set_style_radius(back, 23, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x111820), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, video_grid_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_22,
                                   0xF2F5F8);
    lv_obj_center(back_icon);
    ui_make_click_forwarder(back_icon);

    stop = ui_command_button(body, screen_w - 116, body_h - 60, 92, "Stop",
                             0xEF4D5A);
    lv_obj_add_event_cb(stop, video_stop_event_cb, LV_EVENT_CLICKED, NULL);

    video_status_label = ui_label(body, "Ready", &lv_font_montserrat_16,
                                  0x25C281);
    lv_obj_align(video_status_label, LV_ALIGN_BOTTOM_LEFT, 24, -24);

    video_meta_label = ui_label(body, "", &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(video_meta_label, screen_w - 164);
    lv_label_set_long_mode(video_meta_label, LV_LABEL_LONG_DOT);
    lv_obj_align(video_meta_label, LV_ALIGN_BOTTOM_LEFT, 24, -6);

    if(video_selected >= 0 && video_selected < video_count) {
        name_label = ui_label(body, video_items[video_selected].name,
                              &lv_font_montserrat_16, 0xF2F5F8);
        lv_obj_set_width(name_label, screen_w - 152);
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
        lv_obj_align(name_label, LV_ALIGN_TOP_MID, 0, 28);
    }

    video_timer = lv_timer_create(video_timer_cb, 33, NULL);
    video_start_selected(view_w, view_h);
    video_timer_cb(video_timer);
}

int ui_video_player_handle_back(void)
{
    if(!video_fullscreen) {
        return 0;
    }
    video_fullscreen = 0;
    video_stop_playback();
    app_refresh_current_page();
    return 1;
}

void ui_video_player_create(lv_obj_t *scr)
{
    if(!video_fullscreen || video_count <= 0) {
        video_scan_files();
    }
    if(video_fullscreen && (video_selected < 0 || video_selected >= video_count)) {
        video_fullscreen = 0;
    }
    if(video_fullscreen) {
        video_create_fullscreen(scr);
    } else {
        video_create_grid(scr);
    }
}

void ui_video_player_cleanup(void)
{
    if(video_timer) {
        lv_timer_delete(video_timer);
        video_timer = NULL;
    }
    video_stop_playback();
    free(video_pixels[0]);
    free(video_pixels[1]);
    free(video_display_pixels);
    video_pixels[0] = NULL;
    video_pixels[1] = NULL;
    video_display_pixels = NULL;
    video_status_label = NULL;
    video_meta_label = NULL;
    video_image = NULL;
    video_placeholder = NULL;
}
