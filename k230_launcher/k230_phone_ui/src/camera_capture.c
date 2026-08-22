#include <errno.h>
#include <dirent.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <jpeglib.h>
#include <v4l2-drm.h>

#define DEFAULT_DEVICE 1
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define DEFAULT_THUMB_WIDTH 360
#define DEFAULT_THUMB_HEIGHT 640
#define DEFAULT_SKIP_FRAMES 3
#define DEFAULT_STREAM_FPS 4
#define DEFAULT_STREAM_MAX_FILES 6

typedef enum {
    FORMAT_NV12 = 0,
    FORMAT_NV16,
} capture_format_t;

typedef enum {
    STREAM_OUTPUT_RAW = 0,
    STREAM_OUTPUT_JPEG,
} stream_output_t;

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
    const char *stream_dir;
    const char *stream_prefix;
    const char *stream_preview_path;
    const char *stream_meta_path;
    unsigned stream_count;
    unsigned stream_duration_s;
    unsigned stream_fps;
    unsigned stream_max_files;
    stream_output_t stream_output;
    unsigned jpeg_quality;
    unsigned rotate_degrees;
    int flip_x;
    int flip_y;
} capture_options_t;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [-d video] [-w width] [-h height] [-f NV12|NV16] "
            "-o photo.ppm|photo.jpg [-t thumb.rgb565] "
            "[--thumb-width N] [--thumb-height N]\n"
            "       %s ... --stream-dir DIR [--stream-count N] [--stream-duration SEC]\n",
            argv0,
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
        {"stream-dir", required_argument, NULL, 1003},
        {"stream-prefix", required_argument, NULL, 1004},
        {"stream-count", required_argument, NULL, 1005},
        {"stream-duration", required_argument, NULL, 1006},
        {"stream-fps", required_argument, NULL, 1007},
        {"stream-preview", required_argument, NULL, 1008},
        {"stream-meta", required_argument, NULL, 1009},
        {"stream-max-files", required_argument, NULL, 1010},
        {"stream-output", required_argument, NULL, 1011},
        {"jpeg-quality", required_argument, NULL, 1012},
        {"rotate", required_argument, NULL, 1013},
        {"flip-x", no_argument, NULL, 1014},
        {"flip-y", no_argument, NULL, 1015},
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
    opts->stream_dir = NULL;
    opts->stream_prefix = "frame";
    opts->stream_preview_path = NULL;
    opts->stream_meta_path = NULL;
    opts->stream_count = 0;
    opts->stream_duration_s = 0;
    opts->stream_fps = DEFAULT_STREAM_FPS;
    opts->stream_max_files = DEFAULT_STREAM_MAX_FILES;
    opts->stream_output = STREAM_OUTPUT_RAW;
    opts->jpeg_quality = 32;
    opts->rotate_degrees = 90;
    opts->flip_x = 0;
    opts->flip_y = 0;

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
        case 1003:
            opts->stream_dir = optarg;
            break;
        case 1004:
            opts->stream_prefix = optarg;
            break;
        case 1005:
            if(parse_uint(optarg, &opts->stream_count) != 0) {
                return -1;
            }
            break;
        case 1006:
            if(parse_uint(optarg, &opts->stream_duration_s) != 0) {
                return -1;
            }
            break;
        case 1007:
            if(parse_uint(optarg, &opts->stream_fps) != 0) {
                return -1;
            }
            break;
        case 1008:
            opts->stream_preview_path = optarg;
            break;
        case 1009:
            opts->stream_meta_path = optarg;
            break;
        case 1010:
            if(parse_uint(optarg, &opts->stream_max_files) != 0) {
                return -1;
            }
            break;
        case 1011:
            if(strcmp(optarg, "raw") == 0 || strcmp(optarg, "rgb565") == 0) {
                opts->stream_output = STREAM_OUTPUT_RAW;
            } else if(strcmp(optarg, "jpg") == 0 ||
                      strcmp(optarg, "jpeg") == 0) {
                opts->stream_output = STREAM_OUTPUT_JPEG;
            } else {
                return -1;
            }
            break;
        case 1012:
            if(parse_uint(optarg, &opts->jpeg_quality) != 0 ||
               opts->jpeg_quality < 5 || opts->jpeg_quality > 95) {
                return -1;
            }
            break;
        case 1013:
            if(parse_uint(optarg, &opts->rotate_degrees) != 0 ||
               (opts->rotate_degrees != 0 && opts->rotate_degrees != 90 &&
                opts->rotate_degrees != 180 && opts->rotate_degrees != 270)) {
                return -1;
            }
            break;
        case 1014:
            opts->flip_x = 1;
            break;
        case 1015:
            opts->flip_y = 1;
            break;
        default:
            return -1;
        }
    }

    if(!opts->output_path && !opts->stream_dir) {
        return -1;
    }
    if(opts->width == 0 || opts->height == 0 ||
       opts->thumb_width == 0 || opts->thumb_height == 0) {
        return -1;
    }

    return 0;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void sleep_until_us(uint64_t target_us)
{
    uint64_t now_us = monotonic_us();

    if(target_us <= now_us) {
        return;
    }
    usleep((useconds_t)(target_us - now_us));
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

static void cover_crop(unsigned src_w, unsigned src_h, unsigned dst_w,
                       unsigned dst_h, unsigned *crop_x, unsigned *crop_y,
                       unsigned *crop_w, unsigned *crop_h)
{
    unsigned x = 0;
    unsigned y = 0;
    unsigned w = src_w;
    unsigned h = src_h;

    if(src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        if(crop_x) {
            *crop_x = 0;
        }
        if(crop_y) {
            *crop_y = 0;
        }
        if(crop_w) {
            *crop_w = src_w;
        }
        if(crop_h) {
            *crop_h = src_h;
        }
        return;
    }

    if((uint64_t)src_w * dst_h > (uint64_t)dst_w * src_h) {
        w = (unsigned)(((uint64_t)src_h * dst_w) / dst_h);
        if(w == 0) {
            w = 1;
        }
        if(w > src_w) {
            w = src_w;
        }
        x = (src_w - w) / 2U;
    } else {
        h = (unsigned)(((uint64_t)src_w * dst_h) / dst_w);
        if(h == 0) {
            h = 1;
        }
        if(h > src_h) {
            h = src_h;
        }
        y = (src_h - h) / 2U;
    }

    if(crop_x) {
        *crop_x = x;
    }
    if(crop_y) {
        *crop_y = y;
    }
    if(crop_w) {
        *crop_w = w;
    }
    if(crop_h) {
        *crop_h = h;
    }
}

static void sample_transformed_yuv(const uint8_t *frame,
                                   const capture_options_t *opts,
                                   unsigned out_w, unsigned out_h,
                                   unsigned x, unsigned y,
                                   uint8_t *r, uint8_t *g, uint8_t *b)
{
    unsigned base_w = (opts->rotate_degrees == 90 ||
                       opts->rotate_degrees == 270) ? opts->height :
                                                      opts->width;
    unsigned base_h = (opts->rotate_degrees == 90 ||
                       opts->rotate_degrees == 270) ? opts->width :
                                                      opts->height;
    unsigned crop_x = 0;
    unsigned crop_y = 0;
    unsigned crop_w = base_w;
    unsigned crop_h = base_h;
    unsigned sample_x = opts->flip_x ? (out_w - 1U - x) : x;
    unsigned sample_y = opts->flip_y ? (out_h - 1U - y) : y;
    unsigned base_x;
    unsigned base_y;
    unsigned src_x = 0;
    unsigned src_y = 0;

    cover_crop(base_w, base_h, out_w, out_h, &crop_x, &crop_y, &crop_w,
               &crop_h);

    base_x = crop_x + (unsigned)(((uint64_t)sample_x * crop_w) / out_w);
    base_y = crop_y + (unsigned)(((uint64_t)sample_y * crop_h) / out_h);
    if(base_x >= base_w) {
        base_x = base_w - 1U;
    }
    if(base_y >= base_h) {
        base_y = base_h - 1U;
    }

    switch(opts->rotate_degrees) {
    case 0:
        src_x = base_x;
        src_y = base_y;
        break;
    case 180:
        src_x = opts->width - 1U - base_x;
        src_y = opts->height - 1U - base_y;
        break;
    case 270:
        src_x = opts->width - 1U - base_y;
        src_y = base_x;
        break;
    case 90:
    default:
        src_x = base_y;
        src_y = opts->height - 1U - base_x;
        break;
    }

    if(src_x >= opts->width) {
        src_x = opts->width - 1U;
    }
    if(src_y >= opts->height) {
        src_y = opts->height - 1U;
    }

    sample_yuv(frame, opts->width, opts->height, opts->format,
               src_x, src_y, r, g, b);
}

static int save_ppm_rotated(const char *path, const uint8_t *frame,
                            const capture_options_t *opts)
{
    FILE *fp = fopen(path, "wb");
    unsigned out_w = (opts->rotate_degrees == 90 ||
                      opts->rotate_degrees == 270) ? opts->height :
                                                     opts->width;
    unsigned out_h = (opts->rotate_degrees == 90 ||
                      opts->rotate_degrees == 270) ? opts->width :
                                                     opts->height;

    if(!fp) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "P6\n%u %u\n255\n", out_w, out_h);

    for(unsigned y = 0; y < out_h; y++) {
        for(unsigned x = 0; x < out_w; x++) {
            uint8_t rgb[3];

            sample_transformed_yuv(frame, opts, out_w, out_h, x, y,
                                   &rgb[0], &rgb[1], &rgb[2]);
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

            sample_transformed_yuv(frame, opts, out_w, out_h, x, y,
                                   &r, &g, &b);
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

static int save_thumb_jpeg_rotated(const char *path, const uint8_t *frame,
                                   const capture_options_t *opts)
{
    FILE *fp = fopen(path, "wb");
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned out_w = opts->thumb_width;
    unsigned out_h = opts->thumb_height;
    uint8_t *row;

    if(!fp) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    row = (uint8_t *)malloc((size_t)out_w * 3U);
    if(!row) {
        fclose(fp);
        return -1;
    }

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = out_w;
    cinfo.image_height = out_h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, (int)opts->jpeg_quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while(cinfo.next_scanline < cinfo.image_height) {
        unsigned y = cinfo.next_scanline;
        JSAMPROW row_pointer[1];

        for(unsigned x = 0; x < out_w; x++) {
            uint8_t r;
            uint8_t g;
            uint8_t b;

            sample_transformed_yuv(frame, opts, out_w, out_h, x, y,
                                   &r, &g, &b);
            row[x * 3U + 0U] = r;
            row[x * 3U + 1U] = g;
            row[x * 3U + 2U] = b;
        }
        row_pointer[0] = row;
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(row);
    if(fclose(fp) != 0) {
        fprintf(stderr, "close %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int save_jpeg_rotated(const char *path, const uint8_t *frame,
                             const capture_options_t *opts)
{
    FILE *fp = fopen(path, "wb");
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned out_w = (opts->rotate_degrees == 90 ||
                      opts->rotate_degrees == 270) ? opts->height :
                                                     opts->width;
    unsigned out_h = (opts->rotate_degrees == 90 ||
                      opts->rotate_degrees == 270) ? opts->width :
                                                     opts->height;
    uint8_t *row;

    if(!fp) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    row = (uint8_t *)malloc((size_t)out_w * 3U);
    if(!row) {
        fclose(fp);
        return -1;
    }

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = out_w;
    cinfo.image_height = out_h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, (int)opts->jpeg_quality, TRUE);
    cinfo.optimize_coding = TRUE;
    jpeg_start_compress(&cinfo, TRUE);

    while(cinfo.next_scanline < cinfo.image_height) {
        unsigned y = cinfo.next_scanline;
        JSAMPROW row_pointer[1];

        for(unsigned x = 0; x < out_w; x++) {
            uint8_t r;
            uint8_t g;
            uint8_t b;

            sample_transformed_yuv(frame, opts, out_w, out_h, x, y,
                                   &r, &g, &b);
            row[x * 3U + 0U] = r;
            row[x * 3U + 1U] = g;
            row[x * 3U + 2U] = b;
        }
        row_pointer[0] = row;
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(row);
    if(fclose(fp) != 0) {
        fprintf(stderr, "close %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int ends_with(const char *text, const char *suffix)
{
    size_t text_len;
    size_t suffix_len;

    if(!text || !suffix) {
        return 0;
    }
    text_len = strlen(text);
    suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
           strcmp(text + text_len - suffix_len, suffix) == 0;
}

static void stream_trim_dir(const char *dir, unsigned max_files)
{
    DIR *dp;
    struct dirent *de;

    if(!dir || max_files == 0) {
        return;
    }
    for(;;) {
        char oldest[NAME_MAX + 1] = "";
        unsigned count = 0;

        dp = opendir(dir);
        if(!dp) {
            return;
        }
        while((de = readdir(dp)) != NULL) {
            if(!ends_with(de->d_name, ".raw") &&
               !ends_with(de->d_name, ".jpg") &&
               !ends_with(de->d_name, ".jpeg")) {
                continue;
            }
            count++;
            if(oldest[0] == '\0' || strcmp(de->d_name, oldest) < 0) {
                snprintf(oldest, sizeof(oldest), "%s", de->d_name);
            }
        }
        closedir(dp);

        if(count <= max_files || oldest[0] == '\0') {
            return;
        }
        {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dir, oldest);
            unlink(path);
        }
    }
}

static int save_stream_thumb(const capture_options_t *opts,
                             const uint8_t *frame, unsigned index)
{
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    const char *ext = opts->stream_output == STREAM_OUTPUT_JPEG ? "jpg" :
                      "raw";
    int ret;

    snprintf(path, sizeof(path), "%s/%s_%06u.%s", opts->stream_dir,
             opts->stream_prefix ? opts->stream_prefix : "frame", index, ext);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    unlink(tmp);
    if(opts->stream_output == STREAM_OUTPUT_JPEG) {
        ret = save_thumb_jpeg_rotated(tmp, frame, opts);
    } else {
        ret = save_thumb_rgb565_rotated(tmp, frame, opts);
    }
    if(ret != 0) {
        unlink(tmp);
        return ret;
    }
    if(rename(tmp, path) != 0) {
        fprintf(stderr, "rename %s -> %s failed: %s\n", tmp, path,
                strerror(errno));
        unlink(tmp);
        return -1;
    }

    if(opts->stream_preview_path && opts->stream_preview_path[0]) {
        char preview_tmp[PATH_MAX];

        snprintf(preview_tmp, sizeof(preview_tmp), "%s.tmp",
                 opts->stream_preview_path);
        unlink(preview_tmp);
        if(save_thumb_rgb565_rotated(preview_tmp, frame, opts) == 0) {
            if(rename(preview_tmp, opts->stream_preview_path) != 0) {
                unlink(preview_tmp);
            }
        } else {
            unlink(preview_tmp);
        }
    }

    if(opts->stream_meta_path && opts->stream_meta_path[0]) {
        FILE *fp = fopen(opts->stream_meta_path, "w");
        if(fp) {
            fprintf(fp,
                    "frame_file=%s\nframes_ready=stream\nwidth=%u\nheight=%u\nrole=tx\n",
                    path, opts->thumb_width, opts->thumb_height);
            fclose(fp);
        }
    }

    stream_trim_dir(opts->stream_dir, opts->stream_max_files);
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
        if(ends_with(opts->output_path, ".jpg") ||
           ends_with(opts->output_path, ".jpeg")) {
            if(save_jpeg_rotated(opts->output_path, frame, opts) != 0) {
                goto out_release;
            }
        } else if(save_ppm_rotated(opts->output_path, frame, opts) != 0) {
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

static int capture_stream(const capture_options_t *opts)
{
    struct v4l2_drm_context ctx;
    uint32_t v4l2_format = opts->format == FORMAT_NV12 ? V4L2_PIX_FMT_NV12 :
                                                      V4L2_PIX_FMT_NV16;
    int ret = -1;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned saved = 0;
    unsigned skipped = 0;
    unsigned fps = opts->stream_fps ? opts->stream_fps : DEFAULT_STREAM_FPS;
    uint64_t frame_interval_us = 1000000ULL / fps;
    uint64_t start_us = monotonic_us();
    uint64_t end_us = opts->stream_duration_s ?
        start_us + (uint64_t)opts->stream_duration_s * 1000000ULL : 0;
    uint64_t next_frame_us = start_us;

    if(!opts->stream_dir || mkdir(opts->stream_dir, 0755) != 0) {
        if(errno != EEXIST) {
            fprintf(stderr, "mkdir %s failed: %s\n",
                    opts->stream_dir ? opts->stream_dir : "(null)",
                    strerror(errno));
            return -1;
        }
    }

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

    while((opts->stream_count == 0 || saved < opts->stream_count) &&
          (end_us == 0 || monotonic_us() < end_us)) {
        const uint8_t *frame;

        memset(&ctx.vbuffer, 0, sizeof(ctx.vbuffer));
        ctx.vbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ctx.vbuffer.memory = V4L2_MEMORY_MMAP;

        if(v4l2_drm_dump(&ctx, 3000) != 0) {
            fprintf(stderr, "frame dequeue failed: %s\n", strerror(errno));
            goto out_streamoff;
        }

        if(skipped < opts->skip_frames) {
            skipped++;
            v4l2_drm_dump_release(&ctx);
            continue;
        }

        if(ctx.vbuffer.index >= ctx.buffer_num ||
           !ctx.buffers[ctx.vbuffer.index].mmap) {
            fprintf(stderr, "invalid frame buffer index %u\n",
                    ctx.vbuffer.index);
            v4l2_drm_dump_release(&ctx);
            goto out_streamoff;
        }

        sleep_until_us(next_frame_us);
        next_frame_us = monotonic_us() + frame_interval_us;

        frame = (const uint8_t *)ctx.buffers[ctx.vbuffer.index].mmap;
        if(save_stream_thumb(opts, frame, saved) != 0) {
            v4l2_drm_dump_release(&ctx);
            goto out_streamoff;
        }
        saved++;
        v4l2_drm_dump_release(&ctx);
    }

    ret = saved > 0 ? 0 : -1;

out_streamoff:
    ioctl(ctx.video_fd, VIDIOC_STREAMOFF, &type);
    v4l2_drm_stop(&ctx);
    printf("stream saved=%u dir=%s %ux%u fps=%u\n", saved,
           opts->stream_dir, opts->thumb_width, opts->thumb_height, fps);
    return ret;
}

int main(int argc, char **argv)
{
    capture_options_t opts;

    if(parse_args(argc, argv, &opts) != 0) {
        usage(argv[0]);
        return 2;
    }

    if(opts.stream_dir) {
        return capture_stream(&opts) == 0 ? 0 : 1;
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
