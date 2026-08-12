#include <errno.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <v4l2-drm.h>

#define DEFAULT_DEVICE 1
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define DEFAULT_THUMB_WIDTH 360
#define DEFAULT_THUMB_HEIGHT 640
#define DEFAULT_SKIP_FRAMES 3

typedef enum {
    FORMAT_NV12 = 0,
    FORMAT_NV16,
} capture_format_t;

typedef struct {
    unsigned device;
    unsigned width;
    unsigned height;
    unsigned thumb_width;
    unsigned thumb_height;
    unsigned skip_frames;
    capture_format_t format;
    const char *output_path;
    const char *thumb_path;
} capture_options_t;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [-d video] [-w width] [-h height] [-f NV12|NV16] "
            "-o photo.ppm [-t thumb.rgb565] [--thumb-width N] [--thumb-height N]\n",
            argv0);
}

static int parse_uint(const char *value, unsigned *out)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if(errno || !end || *end != '\0' || parsed > 8192) {
        return -1;
    }

    *out = (unsigned)parsed;
    return 0;
}

static int parse_format(const char *value, capture_format_t *format)
{
    if(strcmp(value, "NV12") == 0 || strcmp(value, "nv12") == 0) {
        *format = FORMAT_NV12;
        return 0;
    }
    if(strcmp(value, "NV16") == 0 || strcmp(value, "nv16") == 0) {
        *format = FORMAT_NV16;
        return 0;
    }
    return -1;
}

static int parse_args(int argc, char **argv, capture_options_t *opts)
{
    int ch;
    int option_index = 0;
    static const struct option long_options[] = {
        {"thumb-width", required_argument, NULL, 1000},
        {"thumb-height", required_argument, NULL, 1001},
        {"skip", required_argument, NULL, 1002},
        {0, 0, 0, 0},
    };

    opts->device = DEFAULT_DEVICE;
    opts->width = DEFAULT_WIDTH;
    opts->height = DEFAULT_HEIGHT;
    opts->thumb_width = DEFAULT_THUMB_WIDTH;
    opts->thumb_height = DEFAULT_THUMB_HEIGHT;
    opts->skip_frames = DEFAULT_SKIP_FRAMES;
    opts->format = FORMAT_NV16;
    opts->output_path = NULL;
    opts->thumb_path = NULL;

    while((ch = getopt_long(argc, argv, "d:w:h:f:o:t:", long_options,
                            &option_index)) != -1) {
        switch(ch) {
        case 'd':
            if(parse_uint(optarg, &opts->device) != 0) {
                return -1;
            }
            break;
        case 'w':
            if(parse_uint(optarg, &opts->width) != 0) {
                return -1;
            }
            break;
        case 'h':
            if(parse_uint(optarg, &opts->height) != 0) {
                return -1;
            }
            break;
        case 'f':
            if(parse_format(optarg, &opts->format) != 0) {
                return -1;
            }
            break;
        case 'o':
            opts->output_path = optarg;
            break;
        case 't':
            opts->thumb_path = optarg;
            break;
        case 1000:
            if(parse_uint(optarg, &opts->thumb_width) != 0) {
                return -1;
            }
            break;
        case 1001:
            if(parse_uint(optarg, &opts->thumb_height) != 0) {
                return -1;
            }
            break;
        case 1002:
            if(parse_uint(optarg, &opts->skip_frames) != 0) {
                return -1;
            }
            break;
        default:
            return -1;
        }
    }

    if(!opts->output_path) {
        return -1;
    }
    if(opts->width == 0 || opts->height == 0 ||
       opts->thumb_width == 0 || opts->thumb_height == 0) {
        return -1;
    }

    return 0;
}

static uint8_t clip8(int value)
{
    if(value < 0) {
        return 0;
    }
    if(value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;

    if(c < 0) {
        c = 0;
    }

    *r = clip8((298 * c + 409 * e + 128) >> 8);
    *g = clip8((298 * c - 100 * d - 208 * e + 128) >> 8);
    *b = clip8((298 * c + 516 * d + 128) >> 8);
}

static void sample_yuv(const uint8_t *frame, unsigned width, unsigned height,
                       capture_format_t format, unsigned x, unsigned y,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    const uint8_t *y_plane = frame;
    const uint8_t *uv_plane = frame + width * height;
    unsigned uv_index;
    uint8_t yy = y_plane[y * width + x];
    uint8_t uu;
    uint8_t vv;

    if(format == FORMAT_NV12) {
        uv_index = (y / 2) * width + (x & ~1U);
    } else {
        uv_index = y * width + (x & ~1U);
    }

    uu = uv_plane[uv_index + 0];
    vv = uv_plane[uv_index + 1];
    yuv_to_rgb(yy, uu, vv, r, g, b);
}

static int save_ppm_rotated(const char *path, const uint8_t *frame,
                            const capture_options_t *opts)
{
    FILE *fp = fopen(path, "wb");
    unsigned out_w = opts->height;
    unsigned out_h = opts->width;

    if(!fp) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "P6\n%u %u\n255\n", out_w, out_h);

    for(unsigned y = 0; y < out_h; y++) {
        for(unsigned x = 0; x < out_w; x++) {
            uint8_t rgb[3];
            unsigned src_x = y;
            unsigned src_y = opts->height - 1U - x;

            sample_yuv(frame, opts->width, opts->height, opts->format,
                       src_x, src_y, &rgb[0], &rgb[1], &rgb[2]);
            if(fwrite(rgb, 1, sizeof(rgb), fp) != sizeof(rgb)) {
                fprintf(stderr, "write %s failed: %s\n", path, strerror(errno));
                fclose(fp);
                return -1;
            }
        }
    }

    if(fclose(fp) != 0) {
        fprintf(stderr, "close %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int save_thumb_rgb565_rotated(const char *path, const uint8_t *frame,
                                     const capture_options_t *opts)
{
    FILE *fp = fopen(path, "wb");
    unsigned out_w = opts->thumb_width;
    unsigned out_h = opts->thumb_height;

    if(!fp) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    for(unsigned y = 0; y < out_h; y++) {
        for(unsigned x = 0; x < out_w; x++) {
            uint8_t r;
            uint8_t g;
            uint8_t b;
            uint16_t rgb565;
            uint8_t packed[2];
            unsigned src_x = (unsigned)(((uint64_t)y * opts->width) / out_h);
            unsigned src_y = opts->height - 1U -
                             (unsigned)(((uint64_t)x * opts->height) / out_w);

            if(src_x >= opts->width) {
                src_x = opts->width - 1U;
            }
            if(src_y >= opts->height) {
                src_y = opts->height - 1U;
            }

            sample_yuv(frame, opts->width, opts->height, opts->format,
                       src_x, src_y, &r, &g, &b);
            rgb565 = (uint16_t)(((r & 0xF8U) << 8) |
                                ((g & 0xFCU) << 3) |
                                (b >> 3));
            packed[0] = (uint8_t)(rgb565 & 0xFFU);
            packed[1] = (uint8_t)(rgb565 >> 8);

            if(fwrite(packed, 1, sizeof(packed), fp) != sizeof(packed)) {
                fprintf(stderr, "write %s failed: %s\n", path, strerror(errno));
                fclose(fp);
                return -1;
            }
        }
    }

    if(fclose(fp) != 0) {
        fprintf(stderr, "close %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int capture_frame(const capture_options_t *opts)
{
    struct v4l2_drm_context ctx;
    uint32_t v4l2_format = opts->format == FORMAT_NV12 ? V4L2_PIX_FMT_NV12 :
                                                      V4L2_PIX_FMT_NV16;
    const uint8_t *frame = NULL;
    int ret = -1;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    memset(&ctx, 0, sizeof(ctx));
    v4l2_drm_default_context(&ctx);
    ctx.device = opts->device;
    ctx.width = opts->width;
    ctx.height = opts->height;
    ctx.video_format = v4l2_format;
    ctx.display = false;
    ctx.buffer_num = 5;

    if(v4l2_drm_setup(&ctx, 1, NULL) != 0) {
        fprintf(stderr, "v4l2 setup failed for /dev/video%u\n", opts->device);
        return -1;
    }

    if(v4l2_drm_start(&ctx) != 0) {
        fprintf(stderr, "stream start failed: %s\n", strerror(errno));
        v4l2_drm_stop(&ctx);
        return -1;
    }

    for(unsigned i = 0; i <= opts->skip_frames; i++) {
        memset(&ctx.vbuffer, 0, sizeof(ctx.vbuffer));
        ctx.vbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ctx.vbuffer.memory = V4L2_MEMORY_MMAP;

        if(v4l2_drm_dump(&ctx, 3000) != 0) {
            fprintf(stderr, "frame dequeue failed: %s\n", strerror(errno));
            goto out_streamoff;
        }

        if(i < opts->skip_frames) {
            v4l2_drm_dump_release(&ctx);
            continue;
        }

        if(ctx.vbuffer.index >= ctx.buffer_num || !ctx.buffers[ctx.vbuffer.index].mmap) {
            fprintf(stderr, "invalid frame buffer index %u\n", ctx.vbuffer.index);
            goto out_release;
        }

        frame = (const uint8_t *)ctx.buffers[ctx.vbuffer.index].mmap;
        if(save_ppm_rotated(opts->output_path, frame, opts) != 0) {
            goto out_release;
        }
        if(opts->thumb_path &&
           save_thumb_rgb565_rotated(opts->thumb_path, frame, opts) != 0) {
            goto out_release;
        }

        ret = 0;

out_release:
        v4l2_drm_dump_release(&ctx);
        break;
    }

out_streamoff:
    ioctl(ctx.video_fd, VIDIOC_STREAMOFF, &type);
    v4l2_drm_stop(&ctx);
    return ret;
}

int main(int argc, char **argv)
{
    capture_options_t opts;

    if(parse_args(argc, argv, &opts) != 0) {
        usage(argv[0]);
        return 2;
    }

    if(capture_frame(&opts) != 0) {
        return 1;
    }

    printf("saved %s\n", opts.output_path);
    if(opts.thumb_path) {
        printf("thumb %s %ux%u RGB565\n", opts.thumb_path,
               opts.thumb_width, opts.thumb_height);
    }

    return 0;
}
