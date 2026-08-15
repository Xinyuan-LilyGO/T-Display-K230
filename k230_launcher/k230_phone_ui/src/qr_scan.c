#include <errno.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <quirc.h>
#include <v4l2-drm.h>

#define QR_SCAN_DEFAULT_DEVICE 1
#define QR_SCAN_DEFAULT_WIDTH 640
#define QR_SCAN_DEFAULT_HEIGHT 360
#define QR_SCAN_DEFAULT_TIMEOUT 15
#define QR_SCAN_DEFAULT_SKIP 4
#define QR_SCAN_PREVIEW_PATH_MAX 256
#define QR_SCAN_DEFAULT_PREVIEW_WIDTH 384
#define QR_SCAN_DEFAULT_PREVIEW_HEIGHT 216
#define QR_SCAN_DEFAULT_PREVIEW_INTERVAL_MS 120

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
    char preview_file[QR_SCAN_PREVIEW_PATH_MAX];
    unsigned preview_width;
    unsigned preview_height;
    unsigned preview_interval_ms;
} qr_scan_options_t;

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
            "[--timeout-sec N] [--skip N] [--preview] "
            "[--preview-file path --preview-width W --preview-height H]\n",
            argv0);
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

static int decode_frame(struct quirc *qr, const uint8_t *y_plane,
                        unsigned width, unsigned height, char *out,
                        size_t out_len)
{
    uint8_t *image;
    int qr_w = 0;
    int qr_h = 0;
    int count;

    if(!qr || !y_plane || !out || out_len == 0U) {
        return -1;
    }
    if(quirc_resize(qr, (int)width, (int)height) != 0) {
        return -1;
    }
    image = quirc_begin(qr, &qr_w, &qr_h);
    if(!image || qr_w != (int)width || qr_h != (int)height) {
        return -1;
    }
    memcpy(image, y_plane, (size_t)width * height);
    quirc_end(qr);

    count = quirc_count(qr);
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
            continue;
        }
        n = (size_t)data.payload_len;
        if(n >= out_len) {
            n = out_len - 1U;
        }
        memcpy(out, data.payload, n);
        out[n] = '\0';
        if(is_meshtastic_url(out)) {
            return 0;
        }
    }
    return -1;
}

typedef struct {
    const qr_scan_options_t *opts;
    struct quirc *qr;
    uint64_t deadline_ms;
    unsigned skipped;
    unsigned last_frame_count;
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
    if(monotonic_ms() >= rt->deadline_ms) {
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
                    result, sizeof(result)) == 0) {
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

    if(!opts) {
        return 2;
    }
    qr = quirc_new();
    if(!qr) {
        fprintf(stderr, "quirc allocation failed\n");
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
        quirc_destroy(qr);
        return 2;
    }

    memset(&runtime, 0, sizeof(runtime));
    runtime.opts = opts;
    runtime.qr = qr;
    runtime.deadline_ms = monotonic_ms() + (uint64_t)opts->timeout_s * 1000ULL;
    runtime.rc = 1;
    g_scan_runtime = &runtime;
    (void)v4l2_drm_run(&ctx, 1, scan_preview_handler);
    g_scan_runtime = NULL;
    if(display) {
        display_exit(display);
    }
    quirc_destroy(qr);
    if(runtime.rc != 0) {
        fprintf(stderr, "no Meshtastic QR code found\n");
    }
    return runtime.rc;
}

static int scan_camera(const qr_scan_options_t *opts)
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
    int rc = 1;

    if(!opts) {
        return 2;
    }
    if(opts->preview) {
        return scan_camera_preview(opts);
    }
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
        quirc_destroy(qr);
        free(preview_buf);
        return 2;
    }
    if(v4l2_drm_start(&ctx) != 0) {
        fprintf(stderr, "stream start failed: %s\n", strerror(errno));
        v4l2_drm_stop(&ctx);
        quirc_destroy(qr);
        free(preview_buf);
        return 2;
    }

    deadline_ms = monotonic_ms() + (uint64_t)opts->timeout_s * 1000ULL;
    while(monotonic_ms() < deadline_ms) {
        const uint8_t *frame;

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
                        sizeof(result)) == 0) {
            printf("%s\n", result);
            fflush(stdout);
            rc = 0;
            v4l2_drm_dump_release(&ctx);
            break;
        }
        v4l2_drm_dump_release(&ctx);
    }

    ioctl(ctx.video_fd, VIDIOC_STREAMOFF, &type);
    v4l2_drm_stop(&ctx);
    quirc_destroy(qr);
    free(preview_buf);
    if(rc != 0) {
        fprintf(stderr, "no Meshtastic QR code found\n");
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
