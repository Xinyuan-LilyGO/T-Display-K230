#include <errno.h>
#include <getopt.h>
#include <linux/videodev2.h>
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
            "[--timeout-sec N] [--skip N]\n",
            argv0);
}

static int parse_args(int argc, char **argv, qr_scan_options_t *opts)
{
    int ch;
    int option_index = 0;
    static const struct option long_options[] = {
        {"timeout-sec", required_argument, NULL, 1000},
        {"skip", required_argument, NULL, 1001},
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
        default:
            return -1;
        }
    }
    if(opts->width == 0 || opts->height == 0 || opts->timeout_s == 0) {
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

static int scan_camera(const qr_scan_options_t *opts)
{
    struct v4l2_drm_context ctx;
    struct quirc *qr;
    uint32_t v4l2_format;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint64_t deadline_ms;
    unsigned skipped = 0;
    char result[1024];
    int rc = 1;

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
    ctx.display = false;
    ctx.buffer_num = 5;

    if(v4l2_drm_setup(&ctx, 1, NULL) != 0) {
        fprintf(stderr, "v4l2 setup failed for /dev/video%u\n", opts->device);
        quirc_destroy(qr);
        return 2;
    }
    if(v4l2_drm_start(&ctx) != 0) {
        fprintf(stderr, "stream start failed: %s\n", strerror(errno));
        v4l2_drm_stop(&ctx);
        quirc_destroy(qr);
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
