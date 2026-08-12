#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MAX_DRM_BUFS 3

#ifndef K230_DRM_PROBE_RGB565
#define K230_DRM_PROBE_RGB565 0
#endif

#if K230_DRM_PROBE_RGB565
typedef uint16_t drm_pixel_t;
#define DRM_PROBE_BPP 16
#define DRM_PROBE_FOURCC DRM_FORMAT_RGB565
#define DRM_PROBE_FORMAT_NAME "RGB565"
#else
typedef uint32_t drm_pixel_t;
#define DRM_PROBE_BPP 32
#define DRM_PROBE_FOURCC DRM_FORMAT_XRGB8888
#define DRM_PROBE_FORMAT_NAME "XRGB8888"
#endif

typedef struct {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    uint64_t size;
    drm_pixel_t *map;
} drm_buf_t;

typedef struct {
    int pending;
} flip_state_t;

static volatile int running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void *data)
{
    flip_state_t *state = data;

    (void)fd;
    (void)frame;
    (void)sec;
    (void)usec;

    state->pending = 0;
}

static drm_pixel_t pack_color(uint32_t color)
{
#if K230_DRM_PROBE_RGB565
    uint32_t r = (color >> 16) & 0xff;
    uint32_t g = (color >> 8) & 0xff;
    uint32_t b = color & 0xff;

    return (drm_pixel_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
#else
    return color;
#endif
}

static drmModeConnector *find_connected_connector(int fd, drmModeRes *res)
{
    for(int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);

        if(!conn) {
            continue;
        }
        if(conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            return conn;
        }

        drmModeFreeConnector(conn);
    }

    return NULL;
}

static uint32_t choose_crtc(int fd, drmModeRes *res, drmModeConnector *conn)
{
    if(conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);

        if(enc) {
            uint32_t crtc_id = enc->crtc_id;

            drmModeFreeEncoder(enc);
            if(crtc_id) {
                return crtc_id;
            }
        }
    }

    if(res->count_crtcs > 0) {
        return res->crtcs[0];
    }

    return 0;
}

static int create_buffer(int fd, uint32_t width, uint32_t height, drm_buf_t *buf)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];

    memset(buf, 0, sizeof(*buf));
    memset(&creq, 0, sizeof(creq));
    creq.width = width;
    creq.height = height;
    creq.bpp = DRM_PROBE_BPP;

    if(drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return -1;
    }

    buf->handle = creq.handle;
    buf->pitch = creq.pitch;
    buf->size = creq.size;

    memset(handles, 0, sizeof(handles));
    memset(pitches, 0, sizeof(pitches));
    memset(offsets, 0, sizeof(offsets));
    handles[0] = buf->handle;
    pitches[0] = buf->pitch;

    if(drmModeAddFB2(fd, width, height, DRM_PROBE_FOURCC,
                     handles, pitches, offsets, &buf->fb_id, 0) < 0) {
        perror("drmModeAddFB2 " DRM_PROBE_FORMAT_NAME);
        return -1;
    }

    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = buf->handle;
    if(drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        return -1;
    }

    buf->map = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, mreq.offset);
    if(buf->map == MAP_FAILED) {
        perror("mmap dumb buffer");
        buf->map = NULL;
        return -1;
    }

    return 0;
}

static void destroy_buffer(int fd, drm_buf_t *buf)
{
    struct drm_mode_destroy_dumb dreq;

    if(buf->map) {
        munmap(buf->map, buf->size);
    }
    if(buf->fb_id) {
        drmModeRmFB(fd, buf->fb_id);
    }
    if(buf->handle) {
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = buf->handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }

    memset(buf, 0, sizeof(*buf));
}

static void put_rect(drm_buf_t *buf, uint32_t width, uint32_t height,
                     int x, int y, int w, int h, uint32_t color)
{
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + w > (int)width ? (int)width : x + w;
    int y2 = y + h > (int)height ? (int)height : y + h;
    uint32_t stride = buf->pitch / sizeof(drm_pixel_t);
    drm_pixel_t packed = pack_color(color);

    for(int yy = y1; yy < y2; yy++) {
        drm_pixel_t *row = buf->map + (uint32_t)yy * stride;

        for(int xx = x1; xx < x2; xx++) {
            row[xx] = packed;
        }
    }
}

static void draw_frame(drm_buf_t *buf, uint32_t width, uint32_t height,
                       uint32_t frame)
{
    uint32_t stride = buf->pitch / sizeof(drm_pixel_t);
    int x1 = (int)(frame % width);
    int x2 = (int)((frame * 2U) % width);
    int x4 = (int)((frame * 4U) % width);

    for(uint32_t y = 0; y < height; y++) {
        uint32_t shade = 0x10 + ((y * 0x14) / height);
        uint32_t color = 0xFF000000 | (shade << 16) | (shade << 8) | shade;
        drm_pixel_t *row = buf->map + y * stride;
        drm_pixel_t packed = pack_color(color);

        for(uint32_t x = 0; x < width; x++) {
            row[x] = packed;
        }
    }

    put_rect(buf, width, height, 0, 80, width, 2, 0xFF404040);
    put_rect(buf, width, height, 0, (int)height / 2, width, 2, 0xFF404040);
    put_rect(buf, width, height, 0, (int)height - 160, width, 2, 0xFF404040);

    put_rect(buf, width, height, x1, 110, 10, 80, 0xFF22D3EE);
    put_rect(buf, width, height, x2, (int)height / 2 + 30, 10, 80, 0xFFF5A524);
    put_rect(buf, width, height, x4, (int)height - 130, 10, 80, 0xFFEF4D5A);
}

static void draw_page_pattern(drm_buf_t *buf, uint32_t width, uint32_t height,
                              uint32_t page)
{
    uint32_t stride = buf->pitch / sizeof(drm_pixel_t);
    uint32_t bg0 = page ? 0xFF10151F : 0xFF111827;
    uint32_t bg1 = page ? 0xFF3B0764 : 0xFF0F766E;
    uint32_t bg2 = page ? 0xFF7F1D1D : 0xFF1D4ED8;
    uint32_t mark = page ? 0xFFFFD166 : 0xFF7DD3FC;
    uint32_t accent = page ? 0xFF22C55E : 0xFFF97316;

    for(uint32_t y = 0; y < height; y++) {
        drm_pixel_t *row = buf->map + y * stride;
        uint32_t band = y < height / 3 ? bg0 : (y < (height * 2) / 3 ? bg1 : bg2);
        drm_pixel_t packed_band = pack_color(band);
        drm_pixel_t packed_grid = pack_color(0xFF293241);

        for(uint32_t x = 0; x < width; x++) {
            uint32_t grid = ((x % 64U) == 0U || (y % 64U) == 0U) ? 0xFF293241 : 0;
            row[x] = grid ? packed_grid : packed_band;
        }
    }

    if(page) {
        put_rect(buf, width, height, 338, 120, 112, 520, mark);
        put_rect(buf, width, height, 338, 120, 28, 520, 0xFF0B0D10);
        put_rect(buf, width, height, 338, 352, 112, 36, 0xFF0B0D10);
        put_rect(buf, width, height, 86, 800, 396, 100, accent);
    } else {
        put_rect(buf, width, height, 118, 120, 92, 520, mark);
        put_rect(buf, width, height, 210, 120, 160, 48, mark);
        put_rect(buf, width, height, 210, 352, 160, 48, mark);
        put_rect(buf, width, height, 210, 592, 160, 48, mark);
        put_rect(buf, width, height, 86, 800, 396, 100, accent);
    }

    put_rect(buf, width, height, 0, 0, width, 8, 0xFFFFFFFF);
    put_rect(buf, width, height, 0, (int)height - 8, width, 8, 0xFFFFFFFF);
    put_rect(buf, width, height, 0, (int)height / 2 - 2, width, 4, 0xFFFFFFFF);
}

static void draw_fixed_page_pattern(drm_buf_t *buf, uint32_t width, uint32_t height,
                                    uint32_t page)
{
    uint32_t stride = buf->pitch / sizeof(drm_pixel_t);
    uint32_t bg = page ? 0xFF3B0764 : 0xFF0F766E;
    uint32_t panel = page ? 0xFF7F1D1D : 0xFF1D4ED8;
    uint32_t accent = page ? 0xFF22C55E : 0xFFF97316;
    int cx = (int)width / 2;
    int cy = (int)height / 2;

    for(uint32_t y = 0; y < height; y++) {
        drm_pixel_t *row = buf->map + y * stride;
        drm_pixel_t packed_bg = pack_color(bg);

        for(uint32_t x = 0; x < width; x++) {
            row[x] = packed_bg;
        }
    }

    put_rect(buf, width, height, cx - 160, cy - 260, 320, 520, panel);
    put_rect(buf, width, height, cx - 120, cy - 220, 240, 48, accent);
    put_rect(buf, width, height, cx - 120, cy - 24, 240, 48, accent);
    put_rect(buf, width, height, cx - 120, cy + 172, 240, 48, accent);

    put_rect(buf, width, height, 0, 0, width, 8, 0xFFFFFFFF);
    put_rect(buf, width, height, 0, (int)height - 8, width, 8, 0xFFFFFFFF);
    put_rect(buf, width, height, 0, cy - 2, width, 4, 0xFFFFFFFF);
    put_rect(buf, width, height, cx - 2, 0, 4, height, 0xFFFFFFFF);
}

static uint32_t fixed_page_pixel(uint32_t width, uint32_t height, uint32_t page,
                                 uint32_t x, uint32_t y)
{
    uint32_t bg = page ? 0xFF3B0764 : 0xFF0F766E;
    uint32_t panel = page ? 0xFF7F1D1D : 0xFF1D4ED8;
    uint32_t accent = page ? 0xFF22C55E : 0xFFF97316;
    int cx = (int)width / 2;
    int cy = (int)height / 2;
    int ix = (int)x;
    int iy = (int)y;

    if(iy < 8 || iy >= (int)height - 8 || (iy >= cy - 2 && iy < cy + 2) ||
       (ix >= cx - 2 && ix < cx + 2)) {
        return 0xFFFFFFFF;
    }

    if(ix >= cx - 120 && ix < cx + 120) {
        if((iy >= cy - 220 && iy < cy - 172) ||
           (iy >= cy - 24 && iy < cy + 24) ||
           (iy >= cy + 172 && iy < cy + 220)) {
            return accent;
        }
    }

    if(ix >= cx - 160 && ix < cx + 160 && iy >= cy - 260 && iy < cy + 260) {
        return panel;
    }

    return bg;
}

static void draw_fixed_page_rows(drm_buf_t *buf, uint32_t width, uint32_t height,
                                 uint32_t page, uint32_t y_start, uint32_t y_end)
{
    uint32_t stride = buf->pitch / sizeof(drm_pixel_t);

    if(y_start > height) {
        y_start = height;
    }
    if(y_end > height) {
        y_end = height;
    }

    for(uint32_t y = y_start; y < y_end; y++) {
        drm_pixel_t *row = buf->map + y * stride;

        for(uint32_t x = 0; x < width; x++) {
            row[x] = pack_color(fixed_page_pixel(width, height, page, x, y));
        }
    }
}

static void copy_frame_rows(drm_buf_t *dst, const drm_buf_t *src,
                            uint32_t width, uint32_t height)
{
    uint32_t bytes = width * sizeof(drm_pixel_t);

    for(uint32_t y = 0; y < height; y++) {
        memcpy((uint8_t *)dst->map + (size_t)y * dst->pitch,
               (const uint8_t *)src->map + (size_t)y * src->pitch, bytes);
    }
}

static void copy_frame_pixels_volatile(drm_buf_t *dst, const drm_buf_t *src,
                                       uint32_t width, uint32_t height)
{
    for(uint32_t y = 0; y < height; y++) {
        volatile drm_pixel_t *drow =
            (volatile drm_pixel_t *)((uint8_t *)dst->map + (size_t)y * dst->pitch);
        const drm_pixel_t *srow =
            (const drm_pixel_t *)((const uint8_t *)src->map + (size_t)y * src->pitch);

        for(uint32_t x = 0; x < width; x++) {
            drow[x] = srow[x];
        }
    }
}

static void write_stage_fill_load(drm_buf_t *buf, uint32_t width, uint32_t height,
                                  uint32_t load_lines, uint32_t chunk_lines,
                                  useconds_t chunk_delay_us, uint32_t seed)
{
    uint32_t stride = buf->pitch / sizeof(drm_pixel_t);

    if(load_lines == 0 || load_lines > height) {
        load_lines = height;
    }

    for(uint32_t y = 0; y < load_lines; y++) {
        volatile drm_pixel_t *row = buf->map + y * stride;
        uint32_t value = 0x9E3779B9U ^ seed ^ (y * 0x45D9F3BU);

        for(uint32_t x = 0; x < width; x++) {
            row[x] = (drm_pixel_t)(value + x * 0x10204081U);
        }

        if(chunk_lines && chunk_delay_us && ((y + 1U) % chunk_lines) == 0U) {
            usleep(chunk_delay_us);
        }
    }
}

static void draw_hidden_striped(drm_buf_t *buf, uint32_t width, uint32_t height,
                                uint32_t frame, uint32_t stripe_lines,
                                useconds_t stripe_delay_us)
{
    uint32_t stride = buf->pitch / sizeof(drm_pixel_t);
    drm_pixel_t color = pack_color((frame & 1U) ? 0xFF202830 : 0xFF303820);

    for(uint32_t y = 0; y < height; y++) {
        drm_pixel_t *row = buf->map + y * stride;

        for(uint32_t x = 0; x < width; x++) {
            row[x] = color;
        }

        if(stripe_lines && ((y + 1U) % stripe_lines) == 0U) {
            usleep(stripe_delay_us);
        }
    }
}

static int wait_flip(int fd, flip_state_t *state)
{
    drmEventContext ev;
    struct pollfd pfd;

    memset(&ev, 0, sizeof(ev));
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = page_flip_handler;

    while(running && state->pending) {
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if(poll(&pfd, 1, 1000) < 0) {
            if(errno == EINTR) {
                continue;
            }
            perror("poll DRM event");
            return -1;
        }
        if(pfd.revents & POLLIN) {
            if(drmHandleEvent(fd, &ev) < 0) {
                perror("drmHandleEvent");
                return -1;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *card = "/dev/dri/card0";
    int seconds = 30;
    int fd = -1;
    drmModeRes *res = NULL;
    drmModeConnector *conn = NULL;
    drmModeCrtc *old_crtc = NULL;
    drmModeModeInfo mode;
    drm_buf_t bufs[MAX_DRM_BUFS];
    drm_buf_t stage;
    flip_state_t flip;
    uint32_t crtc_id;
    uint32_t frame = 0;
    int front = 0;
    int page_mode = 0;
    int static_page_mode = 0;
    int msync_page_mode = 0;
    int fixed_page_mode = 0;
    int buffer_count = 2;
    int pre_final_delay_ms = 0;
    int post_draw_delay_ms = 0;
    int same_page_mode = 0;
    int repaint_front_mode = 0;
    int offscreen_write_mode = 0;
    int no_msync_mode = 0;
    int striped_write_mode = 0;
    int same_stripe_page_mode = 0;
    int staged_copy_mode = 0;
    int staged_pixel_copy_mode = 0;
    int staged_compute_mode = 0;
    int staged_fill_load_mode = 0;
    uint32_t stripe_lines = 4;
    uint32_t same_stripe_lines = 128;
    uint32_t staged_fill_lines = 0;
    uint32_t staged_fill_chunk_lines = 0;
    useconds_t staged_fill_chunk_delay_us = 0;
    useconds_t stripe_delay_us = 1000;
    useconds_t offscreen_write_period_us = 500000;
    uint32_t stats_interval_frames = 0;
    uint64_t start_us;
    uint64_t last_us;
    uint64_t sum_us = 0;
    uint64_t min_us = 0;
    uint64_t max_us = 0;
    uint32_t dt_count = 0;

    memset(bufs, 0, sizeof(bufs));
    memset(&stage, 0, sizeof(stage));
    memset(&flip, 0, sizeof(flip));

    if(argc > 1) {
        seconds = atoi(argv[1]);
        if(seconds <= 0) {
            seconds = 30;
        }
    }
    if(argc > 2 && strcmp(argv[2], "page") == 0) {
        page_mode = 1;
    } else if(argc > 2 && strcmp(argv[2], "page-static") == 0) {
        page_mode = 1;
        static_page_mode = 1;
    } else if(argc > 2 && strcmp(argv[2], "page-msync") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
    } else if(argc > 2 && strcmp(argv[2], "page-fixed-msync") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
    } else if(argc > 2 && strcmp(argv[2], "page-fixed-msync-3") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        buffer_count = 3;
    } else if(argc > 2 && strcmp(argv[2], "page-fixed-msync-3-delay") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        buffer_count = 3;
        post_draw_delay_ms = 100;
    } else if(argc > 2 && strcmp(argv[2], "page-same-msync-3") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        buffer_count = 3;
        same_page_mode = 1;
    } else if(argc > 2 && strcmp(argv[2], "page-same-stripe-msync-3") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        buffer_count = 3;
        same_page_mode = 1;
        same_stripe_page_mode = 1;
        if(argc > 3) {
            int value = atoi(argv[3]);

            if(value > 0) {
                same_stripe_lines = (uint32_t)value;
            }
        }
    } else if(argc > 2 && strcmp(argv[2], "page-staged-copy-msync-3") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        buffer_count = 3;
        same_page_mode = 1;
        staged_copy_mode = 1;
    } else if(argc > 2 && strcmp(argv[2], "page-staged-pixelcopy-msync-3") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        buffer_count = 3;
        same_page_mode = 1;
        staged_copy_mode = 1;
        staged_pixel_copy_mode = 1;
    } else if(argc > 2 && strcmp(argv[2], "page-staged-compute-msync-3") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        buffer_count = 3;
        same_page_mode = 1;
        staged_copy_mode = 1;
        staged_compute_mode = 1;
    } else if(argc > 2 && strcmp(argv[2], "page-staged-compute-delay-msync-3") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        buffer_count = 3;
        same_page_mode = 1;
        staged_copy_mode = 1;
        staged_compute_mode = 1;
        if(argc > 3) {
            int value = atoi(argv[3]);

            if(value >= 0) {
                pre_final_delay_ms = value;
            }
        }
        if(argc > 4) {
            int value = atoi(argv[4]);

            if(value >= 0) {
                post_draw_delay_ms = value;
            }
        }
    } else if(argc > 2 && strcmp(argv[2], "page-memfill-compute-msync-3") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        buffer_count = 3;
        same_page_mode = 1;
        staged_copy_mode = 1;
        staged_compute_mode = 1;
        staged_fill_load_mode = 1;
        if(argc > 3) {
            int value = atoi(argv[3]);

            if(value > 0) {
                staged_fill_lines = (uint32_t)value;
            }
        }
        if(argc > 4) {
            int value = atoi(argv[4]);

            if(value > 0) {
                staged_fill_chunk_lines = (uint32_t)value;
            }
        }
        if(argc > 5) {
            int value = atoi(argv[5]);

            if(value >= 0) {
                staged_fill_chunk_delay_us = (useconds_t)value;
            }
        }
    } else if(argc > 2 && strcmp(argv[2], "front-repaint") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        same_page_mode = 1;
        repaint_front_mode = 1;
    } else if(argc > 2 && strcmp(argv[2], "offscreen-write") == 0) {
        page_mode = 1;
        msync_page_mode = 1;
        fixed_page_mode = 1;
        same_page_mode = 1;
        offscreen_write_mode = 1;
        buffer_count = 3;
    } else if(argc > 2 && strcmp(argv[2], "offscreen-write-nosync") == 0) {
        page_mode = 1;
        fixed_page_mode = 1;
        same_page_mode = 1;
        offscreen_write_mode = 1;
        no_msync_mode = 1;
        buffer_count = 3;
    } else if(argc > 2 && strcmp(argv[2], "offscreen-write-striped") == 0) {
        page_mode = 1;
        fixed_page_mode = 1;
        same_page_mode = 1;
        offscreen_write_mode = 1;
        no_msync_mode = 1;
        striped_write_mode = 1;
        buffer_count = 3;
        if(argc > 3) {
            int value = atoi(argv[3]);

            if(value > 0) {
                stripe_lines = (uint32_t)value;
            }
        }
        if(argc > 4) {
            int value = atoi(argv[4]);

            if(value >= 0) {
                stripe_delay_us = (useconds_t)value;
            }
        }
    } else if(argc > 2 && strcmp(argv[2], "offscreen-write-striped-fps") == 0) {
        page_mode = 1;
        fixed_page_mode = 1;
        same_page_mode = 1;
        offscreen_write_mode = 1;
        no_msync_mode = 1;
        striped_write_mode = 1;
        buffer_count = 3;
        stripe_lines = 128;
        stripe_delay_us = 30;
        offscreen_write_period_us = 19000;
        if(argc > 3) {
            int value = atoi(argv[3]);

            if(value > 0) {
                stripe_lines = (uint32_t)value;
            }
        }
        if(argc > 4) {
            int value = atoi(argv[4]);

            if(value >= 0) {
                stripe_delay_us = (useconds_t)value;
            }
        }
        if(argc > 5) {
            int value = atoi(argv[5]);

            if(value > 0) {
                offscreen_write_period_us = (useconds_t)value;
            }
        }
    }

    stats_interval_frames = page_mode ? 4U : 42U;
    if(offscreen_write_mode && offscreen_write_period_us < 100000U) {
        stats_interval_frames = 52U;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    fd = open(card, O_RDWR | O_CLOEXEC);
    if(fd < 0) {
        perror(card);
        return 1;
    }

    res = drmModeGetResources(fd);
    if(!res) {
        perror("drmModeGetResources");
        close(fd);
        return 1;
    }

    conn = find_connected_connector(fd, res);
    if(!conn) {
        fprintf(stderr, "no connected DRM connector\n");
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    crtc_id = choose_crtc(fd, res, conn);
    if(!crtc_id) {
        fprintf(stderr, "no usable CRTC\n");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    mode = conn->modes[0];
    old_crtc = drmModeGetCrtc(fd, crtc_id);

    for(int i = 0; i < buffer_count; i++) {
        if(create_buffer(fd, mode.hdisplay, mode.vdisplay, &bufs[i])) {
            for(int j = 0; j < buffer_count; j++) {
                destroy_buffer(fd, &bufs[j]);
            }
            drmModeFreeCrtc(old_crtc);
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            close(fd);
            return 1;
        }
    }

    if(!bufs[0].fb_id || !bufs[1].fb_id) {
        for(int j = 0; j < buffer_count; j++) {
            destroy_buffer(fd, &bufs[j]);
        }
        drmModeFreeCrtc(old_crtc);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    if(staged_copy_mode) {
        stage.pitch = mode.hdisplay * sizeof(drm_pixel_t);
        stage.size = (uint64_t)stage.pitch * (uint64_t)mode.vdisplay;
        stage.map = malloc((size_t)stage.size);
        if(!stage.map) {
            fprintf(stderr, "failed to allocate staging buffer (%llu bytes)\n",
                    (unsigned long long)stage.size);
            for(int j = 0; j < buffer_count; j++) {
                destroy_buffer(fd, &bufs[j]);
            }
            drmModeFreeCrtc(old_crtc);
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            close(fd);
            return 1;
        }
    }

    printf("DRM %s probe: %ux%u crtc=%u connector=%u duration=%ds format=%s bpp=%u\n",
           page_mode ? "page-flip" : "motion", mode.hdisplay, mode.vdisplay,
           crtc_id, conn->connector_id, seconds, DRM_PROBE_FORMAT_NAME,
           DRM_PROBE_BPP);
    if(static_page_mode) {
        printf("alternating two prefilled full-screen patterns every 500ms\n");
    } else if(repaint_front_mode) {
        printf("repainting the visible framebuffer every 500ms, no page flip\n");
    } else if(offscreen_write_mode) {
        if(striped_write_mode) {
            printf("repainting hidden framebuffers in %u-line stripes, %uus pause, %uus target period, no page flip, no msync\n",
                   stripe_lines, (unsigned)stripe_delay_us,
                   (unsigned)offscreen_write_period_us);
        } else {
            printf("repainting hidden framebuffers every 500ms, no page flip%s\n",
                   no_msync_mode ? ", no msync" : "");
        }
    } else if(same_stripe_page_mode) {
        printf("repainting identical bottom %u rows of hidden framebuffer every 500ms with msync, then page flip\n",
               same_stripe_lines);
    } else if(staged_copy_mode) {
        printf("%s, single %s into hidden framebuffer, msync, then page flip\n",
               staged_fill_load_mode ? "volatile sequential memfill staging workload" :
               "multi-pass repaint into staging buffer",
               staged_compute_mode ? "computed final-pixel write" :
               (staged_pixel_copy_mode ? "volatile 32-bit pixel copy" : "memcpy"));
        if(staged_fill_load_mode) {
            printf("staging memfill rows=%u (0 means full height)\n", staged_fill_lines);
            if(staged_fill_chunk_lines && staged_fill_chunk_delay_us) {
                printf("staging memfill pacing: chunk_rows=%u delay=%uus\n",
                       staged_fill_chunk_lines, (unsigned)staged_fill_chunk_delay_us);
            }
        }
        if(pre_final_delay_ms || post_draw_delay_ms) {
            printf("delay: after staging=%dms after msync=%dms\n",
                   pre_final_delay_ms, post_draw_delay_ms);
        }
    } else if(msync_page_mode) {
        printf("%s %d %sfull-screen patterns every 500ms with msync%s\n",
               same_page_mode ? "repainting identical" : "alternating",
               buffer_count, fixed_page_mode ? "fixed-geometry " : "",
               post_draw_delay_ms ? " and post-draw delay" : "");
    } else if(page_mode) {
        printf("alternating two full-screen patterns every 500ms\n");
    } else {
        printf("cyan=1px/frame amber=2px/frame red=4px/frame\n");
    }
    fflush(stdout);

    if(static_page_mode) {
        draw_page_pattern(&bufs[0], mode.hdisplay, mode.vdisplay, 0);
        draw_page_pattern(&bufs[1], mode.hdisplay, mode.vdisplay, 1);
        msync(bufs[0].map, bufs[0].size, MS_SYNC);
        msync(bufs[1].map, bufs[1].size, MS_SYNC);
        front = 0;
        frame = 2;
    } else if(page_mode) {
        if(same_stripe_page_mode) {
            for(int i = 0; i < buffer_count; i++) {
                draw_fixed_page_pattern(&bufs[i], mode.hdisplay, mode.vdisplay, 0);
                if(msync_page_mode) {
                    msync(bufs[i].map, bufs[i].size, MS_SYNC);
                }
            }
            front = 0;
        } else if(staged_copy_mode) {
            if(staged_fill_load_mode) {
                write_stage_fill_load(&stage, mode.hdisplay, mode.vdisplay,
                                      staged_fill_lines, staged_fill_chunk_lines,
                                      staged_fill_chunk_delay_us, frame);
            } else {
                draw_fixed_page_pattern(&stage, mode.hdisplay, mode.vdisplay, 0);
            }
            if(pre_final_delay_ms) {
                usleep((useconds_t)pre_final_delay_ms * 1000U);
            }
            if(staged_compute_mode) {
                draw_fixed_page_rows(&bufs[front], mode.hdisplay, mode.vdisplay, 0,
                                     0, mode.vdisplay);
            } else if(staged_pixel_copy_mode) {
                copy_frame_pixels_volatile(&bufs[front], &stage, mode.hdisplay, mode.vdisplay);
            } else {
                copy_frame_rows(&bufs[front], &stage, mode.hdisplay, mode.vdisplay);
            }
        } else if(fixed_page_mode) {
            uint32_t page = same_page_mode ? 0 : frame++;

            draw_fixed_page_pattern(&bufs[front], mode.hdisplay, mode.vdisplay, page);
        } else {
            draw_page_pattern(&bufs[front], mode.hdisplay, mode.vdisplay, frame++);
        }
        if(msync_page_mode) {
            msync(bufs[front].map, bufs[front].size, MS_SYNC);
        }
        if(post_draw_delay_ms) {
            usleep((useconds_t)post_draw_delay_ms * 1000U);
        }
    } else {
        draw_frame(&bufs[front], mode.hdisplay, mode.vdisplay, frame++);
    }
    if(drmModeSetCrtc(fd, crtc_id, bufs[front].fb_id, 0, 0,
                      &conn->connector_id, 1, &mode) < 0) {
        perror("drmModeSetCrtc");
        free(stage.map);
        for(int j = 0; j < buffer_count; j++) {
            destroy_buffer(fd, &bufs[j]);
        }
        drmModeFreeCrtc(old_crtc);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    start_us = monotonic_us();
    last_us = start_us;

    while(running && monotonic_us() - start_us < (uint64_t)seconds * 1000000ULL) {
        int back = buffer_count == 3 ? (front + 1) % buffer_count : front ^ 1;
        uint64_t loop_start_us = monotonic_us();
        uint64_t now;
        uint64_t dt;

        if(repaint_front_mode) {
            back = front;
            draw_fixed_page_pattern(&bufs[front], mode.hdisplay, mode.vdisplay, 0);
            msync(bufs[front].map, bufs[front].size, MS_SYNC);
        } else if(offscreen_write_mode) {
            back = 1 + (int)(frame++ % 2U);
            if(striped_write_mode) {
                draw_hidden_striped(&bufs[back], mode.hdisplay, mode.vdisplay,
                                    frame, stripe_lines, stripe_delay_us);
            } else {
                draw_fixed_page_pattern(&bufs[back], mode.hdisplay, mode.vdisplay, 0);
            }
            if(!no_msync_mode) {
                msync(bufs[back].map, bufs[back].size, MS_SYNC);
            }
        } else if(static_page_mode) {
            /* Buffers are intentionally left untouched between flips. */
        } else if(page_mode) {
            if(fixed_page_mode) {
                uint32_t page = same_page_mode ? 0 : frame++;

                if(staged_copy_mode) {
                    if(staged_fill_load_mode) {
                        write_stage_fill_load(&stage, mode.hdisplay, mode.vdisplay,
                                              staged_fill_lines, staged_fill_chunk_lines,
                                              staged_fill_chunk_delay_us, frame);
                    } else {
                        draw_fixed_page_pattern(&stage, mode.hdisplay, mode.vdisplay, page);
                    }
                    if(pre_final_delay_ms) {
                        usleep((useconds_t)pre_final_delay_ms * 1000U);
                    }
                    if(staged_compute_mode) {
                        draw_fixed_page_rows(&bufs[back], mode.hdisplay, mode.vdisplay, page,
                                             0, mode.vdisplay);
                    } else if(staged_pixel_copy_mode) {
                        copy_frame_pixels_volatile(&bufs[back], &stage, mode.hdisplay, mode.vdisplay);
                    } else {
                        copy_frame_rows(&bufs[back], &stage, mode.hdisplay, mode.vdisplay);
                    }
                } else if(same_stripe_page_mode) {
                    uint32_t lines = same_stripe_lines > mode.vdisplay ? mode.vdisplay : same_stripe_lines;

                    draw_fixed_page_rows(&bufs[back], mode.hdisplay, mode.vdisplay, page,
                                         mode.vdisplay - lines, mode.vdisplay);
                } else {
                    draw_fixed_page_pattern(&bufs[back], mode.hdisplay, mode.vdisplay, page);
                }
            } else {
                draw_page_pattern(&bufs[back], mode.hdisplay, mode.vdisplay, frame++);
            }
            if(msync_page_mode) {
                msync(bufs[back].map, bufs[back].size, MS_SYNC);
            }
            if(post_draw_delay_ms) {
                usleep((useconds_t)post_draw_delay_ms * 1000U);
            }
        } else {
            draw_frame(&bufs[back], mode.hdisplay, mode.vdisplay, frame++);
        }
        if(repaint_front_mode) {
            usleep(500000);
        } else if(offscreen_write_mode) {
            uint64_t elapsed_us = monotonic_us() - loop_start_us;

            if(elapsed_us < (uint64_t)offscreen_write_period_us) {
                usleep((useconds_t)((uint64_t)offscreen_write_period_us - elapsed_us));
            }
        } else {
            flip.pending = 1;
            if(drmModePageFlip(fd, crtc_id, bufs[back].fb_id,
                               DRM_MODE_PAGE_FLIP_EVENT, &flip) < 0) {
                perror("drmModePageFlip");
                break;
            }
            if(wait_flip(fd, &flip) < 0) {
                break;
            }
        }

        now = monotonic_us();
        dt = now - last_us;
        last_us = now;
        sum_us += dt;
        if(min_us == 0 || dt < min_us) {
            min_us = dt;
        }
        if(dt > max_us) {
            max_us = dt;
        }
        dt_count++;
        if(!offscreen_write_mode) {
            front = back;
        }

        if(page_mode && !repaint_front_mode && !offscreen_write_mode) {
            usleep(500000);
        }

        if(dt_count % stats_interval_frames == 0) {
            double avg_ms = (double)sum_us / (double)dt_count / 1000.0;

            printf("frames=%u avg=%.3fms min=%.3fms max=%.3fms\n",
                   dt_count, avg_ms, (double)min_us / 1000.0,
                   (double)max_us / 1000.0);
            fflush(stdout);
        }
    }

    if(old_crtc) {
        drmModeSetCrtc(fd, old_crtc->crtc_id, old_crtc->buffer_id,
                       old_crtc->x, old_crtc->y, &conn->connector_id, 1,
                       &old_crtc->mode);
    }

    printf("done frames=%u avg=%.3fms min=%.3fms max=%.3fms\n",
           dt_count,
           dt_count ? (double)sum_us / (double)dt_count / 1000.0 : 0.0,
           (double)min_us / 1000.0, (double)max_us / 1000.0);
    fflush(stdout);

    for(int i = 0; i < buffer_count; i++) {
        destroy_buffer(fd, &bufs[i]);
    }
    free(stage.map);
    drmModeFreeCrtc(old_crtc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    close(fd);
    return 0;
}
