#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <quirc.h>
#include <v4l2-drm.h>

#define QR_SCAN_DEFAULT_DEVICE 1
#define QR_SCAN_DEFAULT_WIDTH 1280
#define QR_SCAN_DEFAULT_HEIGHT 720
#define QR_SCAN_DEFAULT_TIMEOUT 25
#define QR_SCAN_DEFAULT_SKIP 2
#define QR_SCAN_PREVIEW_PATH_MAX 256
#define QR_SCAN_DEFAULT_PREVIEW_WIDTH 384
#define QR_SCAN_DEFAULT_PREVIEW_HEIGHT 216
#define QR_SCAN_DEFAULT_PREVIEW_INTERVAL_MS 120
#define QR_SCAN_STAGE_NAME_MAX 32
#define QR_SCAN_FOCUS_SYSFS "/sys/bus/i2c/devices/0-000c/focus_position"
#define QR_SCAN_FOCUS_POWER_CONTROL "/sys/bus/i2c/devices/0-000c/power/control"
#define QR_SCAN_FOCUS_INTERVAL_MS 1400
#define QR_SCAN_ISP_SERVER_CMD \
    "if ! pidof isp_media_server >/dev/null 2>&1; then " \
    "ISP_MEDIA_SENSOR_DRIVER=/usr/lib/libvvcam.so " \
    "/usr/bin/isp_media_server >/tmp/isp.out.log 2>/tmp/isp.err.log & " \
    "fi"

typedef enum {
    QR_SCAN_FORMAT_NV12 = 0,
    QR_SCAN_FORMAT_NV16,
} qr_scan_format_t;

typedef struct {
    unsigned device;
    unsigned width;
    unsigned height;
    unsigned timeout_s;
    unsigned skip_frames;
    qr_scan_format_t format;
    bool preview;
    bool verbose;
    bool require_meshtastic;
    bool focus_sweep;
    char preview_file[QR_SCAN_PREVIEW_PATH_MAX];
    unsigned preview_width;
    unsigned preview_height;
    unsigned preview_interval_ms;
} qr_scan_options_t;

typedef struct {
    bool require_meshtastic;
    unsigned frames;
    unsigned attempts;
    unsigned candidates;
    unsigned decoded_non_meshtastic;
    unsigned decode_errors;
    int last_count;
    quirc_decode_error_t last_error;
    char last_stage[QR_SCAN_STAGE_NAME_MAX];
} qr_scan_stats_t;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int parse_uint(const char *value, unsigned *out, unsigned max_value)
{
    char *end = NULL;
    unsigned long parsed;

    if(!value || !out) {
        return -1;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if(errno || !end || *end != '\0' || parsed > max_value) {
        return -1;
    }
    *out = (unsigned)parsed;
    return 0;
}

static int parse_format(const char *value, qr_scan_format_t *format)
{
    if(!value || !format) {
        return -1;
    }
    if(strcmp(value, "NV12") == 0 || strcmp(value, "nv12") == 0) {
        *format = QR_SCAN_FORMAT_NV12;
        return 0;
    }
    if(strcmp(value, "NV16") == 0 || strcmp(value, "nv16") == 0) {
        *format = QR_SCAN_FORMAT_NV16;
        return 0;
    }
    return -1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [-d video] [-w width] [-h height] [-f NV12|NV16] "
            "[--timeout-sec N] [--skip N] [--preview] [--verbose] "
            "[--require-meshtastic] [--focus-sweep] "
            "[--preview-file path --preview-width W --preview-height H]\n",
            argv0);
}

static void ensure_camera_pipeline(void)
{
    int rc = system(QR_SCAN_ISP_SERVER_CMD);

    if(rc == -1 || (WIFEXITED(rc) && WEXITSTATUS(rc) != 0)) {
        fprintf(stderr, "isp_media_server start check failed\n");
    }
    usleep(300000);
}

static int parse_args(int argc, char **argv, qr_scan_options_t *opts)
{
    int ch;
    int option_index = 0;
    static const struct option long_options[] = {
        {"timeout-sec", required_argument, NULL, 1000},
        {"skip", required_argument, NULL, 1001},
        {"preview", no_argument, NULL, 1002},
        {"preview-file", required_argument, NULL, 1003},
        {"preview-width", required_argument, NULL, 1004},
        {"preview-height", required_argument, NULL, 1005},
        {"preview-interval-ms", required_argument, NULL, 1006},
        {"verbose", no_argument, NULL, 1007},
        {"require-meshtastic", no_argument, NULL, 1008},
        {"focus-sweep", no_argument, NULL, 1009},
        {0, 0, 0, 0},
    };

    if(!opts) {
        return -1;
    }
    opts->device = QR_SCAN_DEFAULT_DEVICE;
    opts->width = QR_SCAN_DEFAULT_WIDTH;
    opts->height = QR_SCAN_DEFAULT_HEIGHT;
    opts->timeout_s = QR_SCAN_DEFAULT_TIMEOUT;
    opts->skip_frames = QR_SCAN_DEFAULT_SKIP;
    opts->format = QR_SCAN_FORMAT_NV16;
    opts->preview = false;
    opts->verbose = false;
    opts->require_meshtastic = false;
    opts->focus_sweep = false;
    opts->preview_file[0] = '\0';
    opts->preview_width = QR_SCAN_DEFAULT_PREVIEW_WIDTH;
    opts->preview_height = QR_SCAN_DEFAULT_PREVIEW_HEIGHT;
    opts->preview_interval_ms = QR_SCAN_DEFAULT_PREVIEW_INTERVAL_MS;

    while((ch = getopt_long(argc, argv, "d:w:h:f:", long_options,
                            &option_index)) != -1) {
        switch(ch) {
        case 'd':
            if(parse_uint(optarg, &opts->device, 16) != 0) {
                return -1;
            }
            break;
        case 'w':
            if(parse_uint(optarg, &opts->width, 4096) != 0) {
                return -1;
            }
            break;
        case 'h':
            if(parse_uint(optarg, &opts->height, 4096) != 0) {
                return -1;
            }
            break;
        case 'f':
            if(parse_format(optarg, &opts->format) != 0) {
                return -1;
            }
            break;
        case 1000:
            if(parse_uint(optarg, &opts->timeout_s, 120) != 0) {
                return -1;
            }
            break;
        case 1001:
            if(parse_uint(optarg, &opts->skip_frames, 60) != 0) {
                return -1;
            }
            break;
        case 1002:
            opts->preview = true;
            break;
        case 1003:
            snprintf(opts->preview_file, sizeof(opts->preview_file), "%s",
                     optarg ? optarg : "");
            break;
        case 1004:
            if(parse_uint(optarg, &opts->preview_width, 1920) != 0) {
                return -1;
            }
            break;
        case 1005:
            if(parse_uint(optarg, &opts->preview_height, 1080) != 0) {
                return -1;
            }
            break;
        case 1006:
            if(parse_uint(optarg, &opts->preview_interval_ms, 5000) != 0) {
                return -1;
            }
            break;
        case 1007:
            opts->verbose = true;
            break;
        case 1008:
            opts->require_meshtastic = true;
            break;
        case 1009:
            opts->focus_sweep = true;
            break;
        default:
            return -1;
        }
    }
    if(opts->width == 0 || opts->height == 0 || opts->timeout_s == 0) {
        return -1;
    }
    if(opts->preview_file[0] &&
       (opts->preview_width == 0 || opts->preview_height == 0 ||
        opts->preview_interval_ms == 0)) {
        return -1;
    }
    return 0;
}

static int focus_write_text(const char *path, const char *text)
{
    int fd;
    size_t len;
    ssize_t written;

    if(!path || !text) {
        return -1;
    }
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if(fd < 0) {
        return -1;
    }
    len = strlen(text);
    written = write(fd, text, len);
    if(written == (ssize_t)len) {
        ssize_t newline_written = write(fd, "\n", 1);
        (void)newline_written;
    }
    close(fd);
    return written == (ssize_t)len ? 0 : -1;
}

static void focus_power_set(const char *mode)
{
    if(focus_write_text(QR_SCAN_FOCUS_POWER_CONTROL, mode) == 0) {
        fprintf(stderr, "qr_focus power=%s\n", mode);
    }
}

static int focus_set_position(unsigned pos)
{
    char text[24];

    snprintf(text, sizeof(text), "%u", pos > 1023U ? 1023U : pos);
    if(focus_write_text(QR_SCAN_FOCUS_SYSFS, text) == 0) {
        fprintf(stderr, "qr_focus pos=%s\n", text);
        return 0;
    }
    return -1;
}

static void focus_sweep_tick(const qr_scan_options_t *opts,
                             uint64_t now_ms, uint64_t *next_focus_ms,
                             unsigned *focus_index)
{
    static const unsigned positions[] = {
        800, 704, 896, 608, 992, 512, 384, 256,
    };

    if(!opts || !opts->focus_sweep || !next_focus_ms || !focus_index ||
       now_ms < *next_focus_ms) {
        return;
    }

    if(focus_set_position(positions[*focus_index]) == 0) {
        *focus_index = (*focus_index + 1U) %
            (unsigned)(sizeof(positions) / sizeof(positions[0]));
    }
    *next_focus_ms = now_ms + QR_SCAN_FOCUS_INTERVAL_MS;
}

static uint16_t gray_to_rgb565(uint8_t gray)
{
    return (uint16_t)(((uint16_t)(gray & 0xf8U) << 8U) |
                      ((uint16_t)(gray & 0xfcU) << 3U) |
                      ((uint16_t)gray >> 3U));
}

static int write_preview_frame(const qr_scan_options_t *opts,
                               const uint8_t *y_plane,
                               uint16_t *preview_buf)
{
    char tmp_path[QR_SCAN_PREVIEW_PATH_MAX + 8];
    FILE *fp;
    size_t count;

    if(!opts || !y_plane || !preview_buf || !opts->preview_file[0]) {
        return -1;
    }

    for(unsigned y = 0; y < opts->preview_height; y++) {
        unsigned src_y = (unsigned)(((uint64_t)y * opts->height) /
                                    opts->preview_height);
        const uint8_t *src_row = y_plane + (size_t)src_y * opts->width;

        for(unsigned x = 0; x < opts->preview_width; x++) {
            unsigned src_x = (unsigned)(((uint64_t)x * opts->width) /
                                        opts->preview_width);
            preview_buf[(size_t)y * opts->preview_width + x] =
                gray_to_rgb565(src_row[src_x]);
        }
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", opts->preview_file);
    fp = fopen(tmp_path, "wb");
    if(!fp) {
        return -1;
    }
    count = fwrite(preview_buf, sizeof(uint16_t),
                   (size_t)opts->preview_width * opts->preview_height, fp);
    if(fclose(fp) != 0 ||
       count != (size_t)opts->preview_width * opts->preview_height) {
        unlink(tmp_path);
        return -1;
    }
    if(rename(tmp_path, opts->preview_file) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static int is_meshtastic_url(const char *text)
{
    return text &&
           (strncmp(text, "https://meshtastic.org/e/#", 27) == 0 ||
            strncmp(text, "http://meshtastic.org/e/#", 26) == 0);
}

static void qr_stats_stage(qr_scan_stats_t *stats, const char *stage)
{
    if(!stats) {
        return;
    }
    snprintf(stats->last_stage, sizeof(stats->last_stage), "%s",
             stage ? stage : "-");
}

static int decode_gray_image(struct quirc *qr, const uint8_t *gray,
                             unsigned width, unsigned height,
                             unsigned stride, char *out, size_t out_len,
                             qr_scan_stats_t *stats, const char *stage)
{
    uint8_t *image;
    int qr_w = 0;
    int qr_h = 0;
    int count;

    if(!qr || !gray || !out || out_len == 0U ||
       width == 0U || height == 0U || stride < width) {
        return -1;
    }
    if(stats) {
        stats->attempts++;
        qr_stats_stage(stats, stage);
    }
    if(quirc_resize(qr, (int)width, (int)height) != 0) {
        return -1;
    }
    image = quirc_begin(qr, &qr_w, &qr_h);
    if(!image || qr_w != (int)width || qr_h != (int)height) {
        return -1;
    }
    if(stride == width) {
        memcpy(image, gray, (size_t)width * height);
    } else {
        for(unsigned y = 0; y < height; y++) {
            memcpy(image + (size_t)y * width, gray + (size_t)y * stride,
                   width);
        }
    }
    quirc_end(qr);

    count = quirc_count(qr);
    if(stats) {
        stats->last_count = count;
        stats->candidates += count > 0 ? (unsigned)count : 0U;
    }
    for(int i = 0; i < count; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_decode_error_t err;
        size_t n;

        quirc_extract(qr, i, &code);
        err = quirc_decode(&code, &data);
        if(err != QUIRC_SUCCESS) {
            quirc_flip(&code);
            err = quirc_decode(&code, &data);
        }
        if(err != QUIRC_SUCCESS || data.payload_len <= 0) {
            if(stats) {
                stats->decode_errors++;
                stats->last_error = err;
            }
            continue;
        }
        n = (size_t)data.payload_len;
        if(n >= out_len) {
            n = out_len - 1U;
        }
        memcpy(out, data.payload, n);
        out[n] = '\0';
        if(!stats || !stats->require_meshtastic || is_meshtastic_url(out)) {
            return 0;
        }
        if(stats) {
            stats->decoded_non_meshtastic++;
        }
    }
    return -1;
}

static void copy_center_roi(const uint8_t *src, unsigned src_w,
                            unsigned src_h, uint8_t *dst, unsigned *roi_w,
                            unsigned *roi_h)
{
    unsigned w = (src_w * 72U) / 100U;
    unsigned h = (src_h * 72U) / 100U;
    unsigned x0;
    unsigned y0;

    if(w < 240U || w > src_w) {
        w = src_w;
    }
    if(h < 240U || h > src_h) {
        h = src_h;
    }
    x0 = (src_w - w) / 2U;
    y0 = (src_h - h) / 2U;
    for(unsigned y = 0; y < h; y++) {
        memcpy(dst + (size_t)y * w, src + (size_t)(y0 + y) * src_w + x0,
               w);
    }
    *roi_w = w;
    *roi_h = h;
}

static void enhance_contrast(uint8_t *dst, const uint8_t *src,
                             unsigned width, unsigned height)
{
    uint8_t min_v = 255;
    uint8_t max_v = 0;
    size_t total = (size_t)width * height;

    for(size_t i = 0; i < total; i += 4U) {
        uint8_t v = src[i];
        if(v < min_v) {
            min_v = v;
        }
        if(v > max_v) {
            max_v = v;
        }
    }
    if(max_v <= min_v + 16U) {
        memcpy(dst, src, total);
        return;
    }
    for(size_t i = 0; i < total; i++) {
        int v = ((int)src[i] - (int)min_v) * 255 /
                ((int)max_v - (int)min_v);
        if(v < 0) {
            v = 0;
        } else if(v > 255) {
            v = 255;
        }
        dst[i] = (uint8_t)v;
    }
}

static void threshold_mean(uint8_t *dst, const uint8_t *src,
                           unsigned width, unsigned height)
{
    size_t total = (size_t)width * height;
    uint64_t sum = 0;
    uint8_t threshold;

    for(size_t i = 0; i < total; i += 4U) {
        sum += src[i];
    }
    threshold = (uint8_t)(sum / ((total + 3U) / 4U));
    for(size_t i = 0; i < total; i++) {
        dst[i] = src[i] > threshold ? 255U : 0U;
    }
}

static void flip_horizontal(uint8_t *dst, const uint8_t *src,
                            unsigned width, unsigned height)
{
    for(unsigned y = 0; y < height; y++) {
        const uint8_t *src_row = src + (size_t)y * width;
        uint8_t *dst_row = dst + (size_t)y * width;

        for(unsigned x = 0; x < width; x++) {
            dst_row[x] = src_row[width - 1U - x];
        }
    }
}

static void flip_vertical(uint8_t *dst, const uint8_t *src,
                          unsigned width, unsigned height)
{
    for(unsigned y = 0; y < height; y++) {
        memcpy(dst + (size_t)y * width,
               src + (size_t)(height - 1U - y) * width, width);
    }
}

static int decode_frame(struct quirc *qr, const uint8_t *y_plane,
                        unsigned width, unsigned height, char *out,
                        size_t out_len, uint8_t *work_a, uint8_t *work_b,
                        size_t work_len, qr_scan_stats_t *stats)
{
    unsigned roi_w = 0;
    unsigned roi_h = 0;
    size_t roi_len;

    if(stats) {
        stats->frames++;
    }
    if(decode_gray_image(qr, y_plane, width, height, width, out, out_len,
                         stats, "full") == 0) {
        return 0;
    }
    if(!work_a || !work_b || work_len < (size_t)width * height) {
        return -1;
    }

    copy_center_roi(y_plane, width, height, work_a, &roi_w, &roi_h);
    roi_len = (size_t)roi_w * roi_h;
    if(roi_w == 0U || roi_h == 0U || roi_len > work_len) {
        return -1;
    }
    if(decode_gray_image(qr, work_a, roi_w, roi_h, roi_w, out, out_len,
                         stats, "center") == 0) {
        return 0;
    }

    enhance_contrast(work_b, work_a, roi_w, roi_h);
    if(decode_gray_image(qr, work_b, roi_w, roi_h, roi_w, out, out_len,
                         stats, "center-contrast") == 0) {
        return 0;
    }

    threshold_mean(work_b, work_a, roi_w, roi_h);
    if(decode_gray_image(qr, work_b, roi_w, roi_h, roi_w, out, out_len,
                         stats, "center-threshold") == 0) {
        return 0;
    }

    flip_horizontal(work_b, work_a, roi_w, roi_h);
    if(decode_gray_image(qr, work_b, roi_w, roi_h, roi_w, out, out_len,
                         stats, "center-hflip") == 0) {
        return 0;
    }

    flip_vertical(work_b, work_a, roi_w, roi_h);
    if(decode_gray_image(qr, work_b, roi_w, roi_h, roi_w, out, out_len,
                         stats, "center-vflip") == 0) {
        return 0;
    }

    enhance_contrast(work_b, work_a, roi_w, roi_h);
    flip_horizontal(work_a, work_b, roi_w, roi_h);
    if(decode_gray_image(qr, work_a, roi_w, roi_h, roi_w, out, out_len,
                         stats, "center-contrast-hflip") == 0) {
        return 0;
    }
    return -1;
}

typedef struct {
    const qr_scan_options_t *opts;
    struct quirc *qr;
    uint64_t deadline_ms;
    uint64_t next_focus_ms;
    unsigned skipped;
    unsigned focus_index;
    unsigned last_frame_count;
    uint8_t *work_a;
    uint8_t *work_b;
    size_t work_len;
    qr_scan_stats_t stats;
    int rc;
} qr_scan_runtime_t;

static qr_scan_runtime_t *g_scan_runtime;

static int scan_preview_handler(struct v4l2_drm_context *ctx, bool displayed)
{
    qr_scan_runtime_t *rt = g_scan_runtime;
    const uint8_t *frame;
    char result[1024];

    (void)displayed;
    if(!rt || !ctx || !rt->opts || !rt->qr) {
        return 'q';
    }
    uint64_t now_ms = monotonic_ms();
    focus_sweep_tick(rt->opts, now_ms, &rt->next_focus_ms, &rt->focus_index);
    if(now_ms >= rt->deadline_ms) {
        rt->rc = 1;
        return 'q';
    }
    if(ctx->frame_count == rt->last_frame_count) {
        return 0;
    }
    rt->last_frame_count = ctx->frame_count;
    if(!ctx->buffers || ctx->vbuffer.index >= ctx->buffer_num ||
       !ctx->buffers[ctx->vbuffer.index].mmap) {
        return 0;
    }
    if(rt->skipped < rt->opts->skip_frames) {
        rt->skipped++;
        return 0;
    }

    frame = (const uint8_t *)ctx->buffers[ctx->vbuffer.index].mmap;
    result[0] = '\0';
    if(decode_frame(rt->qr, frame, rt->opts->width, rt->opts->height,
                    result, sizeof(result), rt->work_a, rt->work_b,
                    rt->work_len, &rt->stats) == 0) {
        printf("%s\n", result);
        fflush(stdout);
        rt->rc = 0;
        return 'q';
    }
    return 0;
}

static int scan_camera_preview(const qr_scan_options_t *opts)
{
    struct v4l2_drm_context ctx;
    struct display *display = NULL;
    struct quirc *qr;
    qr_scan_runtime_t runtime;
    uint32_t v4l2_format;
    uint8_t *work_a;
    uint8_t *work_b;
    size_t work_len;

    if(!opts) {
        return 2;
    }
    ensure_camera_pipeline();
    qr = quirc_new();
    if(!qr) {
        fprintf(stderr, "quirc allocation failed\n");
        return 2;
    }
    work_len = (size_t)opts->width * opts->height;
    work_a = malloc(work_len);
    work_b = malloc(work_len);
    if(!work_a || !work_b) {
        fprintf(stderr, "decode work allocation failed\n");
        free(work_a);
        free(work_b);
        quirc_destroy(qr);
        return 2;
    }

    memset(&ctx, 0, sizeof(ctx));
    v4l2_drm_default_context(&ctx);
    ctx.device = opts->device;
    ctx.width = opts->width;
    ctx.height = opts->height;
    v4l2_format = opts->format == QR_SCAN_FORMAT_NV12 ? V4L2_PIX_FMT_NV12 :
                                                        V4L2_PIX_FMT_NV16;
    ctx.video_format = v4l2_format;
    ctx.display_format = opts->format == QR_SCAN_FORMAT_NV12 ? DRM_FORMAT_NV12 :
                                                            DRM_FORMAT_NV16;
    ctx.display = true;
    ctx.buffer_num = 5;

    if(v4l2_drm_setup(&ctx, 1, &display) != 0) {
        fprintf(stderr, "v4l2 preview setup failed for /dev/video%u\n",
                opts->device);
        free(work_a);
        free(work_b);
        quirc_destroy(qr);
        return 2;
    }

    memset(&runtime, 0, sizeof(runtime));
    runtime.opts = opts;
    runtime.qr = qr;
    runtime.work_a = work_a;
    runtime.work_b = work_b;
    runtime.work_len = work_len;
    runtime.deadline_ms = monotonic_ms() + (uint64_t)opts->timeout_s * 1000ULL;
    runtime.next_focus_ms = 0;
    runtime.rc = 1;
    runtime.stats.require_meshtastic = opts->require_meshtastic;
    if(opts->focus_sweep) {
        focus_power_set("on");
    }
    g_scan_runtime = &runtime;
    (void)v4l2_drm_run(&ctx, 1, scan_preview_handler);
    g_scan_runtime = NULL;
    if(opts->focus_sweep) {
        focus_power_set("auto");
    }
    if(display) {
        display_exit(display);
    }
    if(opts->verbose) {
        fprintf(stderr,
                "qr_scan summary frames=%u attempts=%u candidates=%u non_mesh=%u errors=%u last_stage=%s last_count=%d last_error=%s\n",
                runtime.stats.frames, runtime.stats.attempts,
                runtime.stats.candidates,
                runtime.stats.decoded_non_meshtastic,
                runtime.stats.decode_errors,
                runtime.stats.last_stage[0] ? runtime.stats.last_stage : "-",
                runtime.stats.last_count,
                quirc_strerror(runtime.stats.last_error));
    }
    free(work_a);
    free(work_b);
    quirc_destroy(qr);
    if(runtime.rc != 0) {
        fprintf(stderr, opts->require_meshtastic ?
                "no Meshtastic QR code found\n" : "no QR code found\n");
    }
    return runtime.rc;
}

static int scan_camera_once(const qr_scan_options_t *opts)
{
    struct v4l2_drm_context ctx;
    struct quirc *qr;
    uint32_t v4l2_format;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint64_t deadline_ms;
    unsigned skipped = 0;
    char result[1024];
    uint16_t *preview_buf = NULL;
    uint64_t last_preview_ms = 0;
    uint64_t last_log_ms = 0;
    uint64_t next_focus_ms = 0;
    unsigned focus_index = 0;
    uint8_t *work_a = NULL;
    uint8_t *work_b = NULL;
    size_t work_len;
    qr_scan_stats_t stats;
    int rc = 1;

    if(!opts) {
        return 2;
    }
    if(opts->preview) {
        return scan_camera_preview(opts);
    }
    ensure_camera_pipeline();
    if(opts->preview_file[0]) {
        preview_buf = malloc((size_t)opts->preview_width *
                             opts->preview_height * sizeof(uint16_t));
        if(!preview_buf) {
            fprintf(stderr, "preview allocation failed\n");
            return 2;
        }
    }
    qr = quirc_new();
    if(!qr) {
        fprintf(stderr, "quirc allocation failed\n");
        free(preview_buf);
        return 2;
    }
    work_len = (size_t)opts->width * opts->height;
    work_a = malloc(work_len);
    work_b = malloc(work_len);
    if(!work_a || !work_b) {
        fprintf(stderr, "decode work allocation failed\n");
        free(work_a);
        free(work_b);
        quirc_destroy(qr);
        free(preview_buf);
        return 2;
    }
    memset(&stats, 0, sizeof(stats));
    stats.require_meshtastic = opts->require_meshtastic;

    memset(&ctx, 0, sizeof(ctx));
    v4l2_drm_default_context(&ctx);
    ctx.device = opts->device;
    ctx.width = opts->width;
    ctx.height = opts->height;
    v4l2_format = opts->format == QR_SCAN_FORMAT_NV12 ? V4L2_PIX_FMT_NV12 :
                                                        V4L2_PIX_FMT_NV16;
    ctx.video_format = v4l2_format;
    ctx.display = false;
    ctx.buffer_num = 5;

    if(v4l2_drm_setup(&ctx, 1, NULL) != 0) {
        fprintf(stderr, "v4l2 setup failed for /dev/video%u\n", opts->device);
        free(work_a);
        free(work_b);
        quirc_destroy(qr);
        free(preview_buf);
        return 2;
    }
    if(v4l2_drm_start(&ctx) != 0) {
        fprintf(stderr, "stream start failed: %s\n", strerror(errno));
        v4l2_drm_stop(&ctx);
        free(work_a);
        free(work_b);
        quirc_destroy(qr);
        free(preview_buf);
        return 2;
    }

    if(opts->focus_sweep) {
        focus_power_set("on");
    }
    deadline_ms = monotonic_ms() + (uint64_t)opts->timeout_s * 1000ULL;
    while(monotonic_ms() < deadline_ms) {
        const uint8_t *frame;
        uint64_t now_ms = monotonic_ms();

        focus_sweep_tick(opts, now_ms, &next_focus_ms, &focus_index);

        memset(&ctx.vbuffer, 0, sizeof(ctx.vbuffer));
        ctx.vbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ctx.vbuffer.memory = V4L2_MEMORY_MMAP;
        if(v4l2_drm_dump(&ctx, 1000) != 0) {
            continue;
        }
        if(ctx.vbuffer.index >= ctx.buffer_num ||
           !ctx.buffers[ctx.vbuffer.index].mmap) {
            v4l2_drm_dump_release(&ctx);
            continue;
        }
        frame = (const uint8_t *)ctx.buffers[ctx.vbuffer.index].mmap;
        if(preview_buf) {
            uint64_t now_ms = monotonic_ms();

            if(last_preview_ms == 0 ||
               now_ms - last_preview_ms >= opts->preview_interval_ms) {
                (void)write_preview_frame(opts, frame, preview_buf);
                last_preview_ms = now_ms;
            }
        }
        if(skipped < opts->skip_frames) {
            skipped++;
            v4l2_drm_dump_release(&ctx);
            continue;
        }
        result[0] = '\0';
        if(decode_frame(qr, frame, opts->width, opts->height, result,
                        sizeof(result), work_a, work_b, work_len,
                        &stats) == 0) {
            printf("%s\n", result);
            fflush(stdout);
            rc = 0;
            v4l2_drm_dump_release(&ctx);
            break;
        }
        if(opts->verbose) {
            uint64_t now_ms = monotonic_ms();

            if(last_log_ms == 0U || now_ms - last_log_ms >= 1000U) {
                fprintf(stderr,
                        "qr_scan frames=%u attempts=%u candidates=%u non_mesh=%u errors=%u last_stage=%s last_count=%d\n",
                        stats.frames, stats.attempts, stats.candidates,
                        stats.decoded_non_meshtastic, stats.decode_errors,
                        stats.last_stage[0] ? stats.last_stage : "-",
                        stats.last_count);
                last_log_ms = now_ms;
            }
        }
        v4l2_drm_dump_release(&ctx);
    }

    ioctl(ctx.video_fd, VIDIOC_STREAMOFF, &type);
    v4l2_drm_stop(&ctx);
    if(opts->focus_sweep) {
        focus_power_set("auto");
    }
    if(opts->verbose) {
        fprintf(stderr,
                "qr_scan summary width=%u height=%u frames=%u attempts=%u candidates=%u non_mesh=%u errors=%u last_stage=%s last_count=%d last_error=%s\n",
                opts->width, opts->height, stats.frames, stats.attempts,
                stats.candidates, stats.decoded_non_meshtastic,
                stats.decode_errors,
                stats.last_stage[0] ? stats.last_stage : "-",
                stats.last_count, quirc_strerror(stats.last_error));
    }
    free(work_a);
    free(work_b);
    quirc_destroy(qr);
    free(preview_buf);
    if(rc != 0) {
        fprintf(stderr, opts->require_meshtastic ?
                "no Meshtastic QR code found\n" : "no QR code found\n");
    }
    return rc;
}

static int same_resolution(const qr_scan_options_t *opts, unsigned width,
                           unsigned height)
{
    return opts && opts->width == width && opts->height == height;
}

static int scan_camera(const qr_scan_options_t *opts)
{
    static const unsigned fallback[][2] = {
        {1280, 720},
        {960, 540},
        {640, 360},
    };
    qr_scan_options_t trial;
    int rc;

    if(!opts) {
        return 2;
    }
    rc = scan_camera_once(opts);
    if(rc != 2 || opts->preview) {
        return rc;
    }
    for(size_t i = 0; i < sizeof(fallback) / sizeof(fallback[0]); i++) {
        if(same_resolution(opts, fallback[i][0], fallback[i][1])) {
            continue;
        }
        trial = *opts;
        trial.width = fallback[i][0];
        trial.height = fallback[i][1];
        fprintf(stderr, "retry QR scan at %ux%u\n", trial.width,
                trial.height);
        rc = scan_camera_once(&trial);
        if(rc != 2) {
            return rc;
        }
    }
    return rc;
}

int main(int argc, char **argv)
{
    qr_scan_options_t opts;

    if(parse_args(argc, argv, &opts) != 0) {
        usage(argv[0]);
        return 2;
    }
    return scan_camera(&opts);
}
