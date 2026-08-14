#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <jpeglib.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HALOW_MAGIC "KHWV"
#define HALOW_VERSION 1U
#define HALOW_TYPE_DATA 1U
#define HALOW_HEADER_SIZE 40U
#define HALOW_DEFAULT_PORT 5600
#define HALOW_DEFAULT_PAYLOAD 1200U
#define HALOW_DEFAULT_PREVIEW_W 640U
#define HALOW_DEFAULT_PREVIEW_H 360U
#define HALOW_MAX_FRAME_SIZE (4U * 1024U * 1024U)
#define HALOW_MAX_CHUNKS 4096U
#define HALOW_STATS_INTERVAL_US 1000000ULL

typedef enum {
    HALOW_ROLE_TX = 0,
    HALOW_ROLE_RX,
} halow_role_t;

typedef struct {
    halow_role_t role;
    char host[96];
    int port;
    char in_dir[256];
    char out_dir[256];
    char preview_file[256];
    char meta_file[256];
    char stop_file[256];
    unsigned payload_size;
    unsigned preview_w;
    unsigned preview_h;
    unsigned stream_w;
    unsigned stream_h;
    unsigned idle_us;
    unsigned packet_gap_us;
    unsigned repeat;
} halow_options_t;

typedef struct {
    unsigned frame_id;
    unsigned stream_w;
    unsigned stream_h;
    unsigned frame_size;
    unsigned frame_crc;
    unsigned stride;
    unsigned chunk_count;
    unsigned received_count;
    uint64_t first_us;
    uint8_t *data;
    uint8_t *seen;
} halow_rx_frame_t;

typedef struct {
    uint64_t start_us;
    uint64_t last_log_us;
    uint64_t bytes;
    uint64_t packets;
    uint64_t frames;
    uint64_t dropped;
    uint64_t errors;
} halow_stats_t;

struct jpeg_jmp_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static volatile sig_atomic_t g_stop;

static uint64_t monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void signal_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    uint16_t n = htons(v);
    memcpy(p, &n, sizeof(n));
}

static void put_u32(uint8_t *p, uint32_t v)
{
    uint32_t n = htonl(v);
    memcpy(p, &n, sizeof(n));
}

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)(v >> 32));
    put_u32(p + 4, (uint32_t)(v & 0xFFFFFFFFU));
}

static uint16_t get_u16(const uint8_t *p)
{
    uint16_t v;

    memcpy(&v, p, sizeof(v));
    return ntohs(v);
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v;

    memcpy(&v, p, sizeof(v));
    return ntohl(v);
}

static uint64_t get_u64(const uint8_t *p)
{
    return ((uint64_t)get_u32(p) << 32) | get_u32(p + 4);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --role tx --host IP --in-dir DIR [options]\n"
            "  %s --role rx --out-dir DIR [options]\n"
            "\n"
            "Options:\n"
            "  --port N             UDP port. Default %d\n"
            "  --payload N          UDP payload bytes per packet. Default %u\n"
            "  --preview-file PATH  RGB565 preview file path.\n"
            "  --meta-file PATH     Preview/status metadata file path.\n"
            "  --preview-width N    RGB565 preview width. Default %u\n"
            "  --preview-height N   RGB565 preview height. Default %u\n"
            "  --stream-width N     Encoded stream width, for metadata.\n"
            "  --stream-height N    Encoded stream height, for metadata.\n"
            "  --stop-file PATH     Stop marker path.\n"
            "  --packet-gap-us N    TX delay between packets. Default 0.\n"
            "  --repeat N           Repeat each UDP frame. Default 1.\n",
            argv0, argv0, HALOW_DEFAULT_PORT, HALOW_DEFAULT_PAYLOAD,
            HALOW_DEFAULT_PREVIEW_W, HALOW_DEFAULT_PREVIEW_H);
}

static int parse_uint_arg(const char *text, unsigned *out)
{
    char *end = NULL;
    unsigned long v;

    if(!text || !out) {
        return -1;
    }
    errno = 0;
    v = strtoul(text, &end, 10);
    if(errno || !end || *end != '\0' || v > 10000000UL) {
        return -1;
    }
    *out = (unsigned)v;
    return 0;
}

static void options_init(halow_options_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->role = HALOW_ROLE_RX;
    opts->port = HALOW_DEFAULT_PORT;
    snprintf(opts->preview_file, sizeof(opts->preview_file),
             "/tmp/k230_halow_preview.rgb565");
    snprintf(opts->meta_file, sizeof(opts->meta_file),
             "/tmp/k230_halow_preview.meta");
    snprintf(opts->stop_file, sizeof(opts->stop_file),
             "/tmp/k230_halow_camera_stream.stop");
    opts->payload_size = HALOW_DEFAULT_PAYLOAD;
    opts->preview_w = HALOW_DEFAULT_PREVIEW_W;
    opts->preview_h = HALOW_DEFAULT_PREVIEW_H;
    opts->stream_w = 320;
    opts->stream_h = 240;
    opts->idle_us = 20000;
    opts->repeat = 1;
}

static int parse_args(int argc, char **argv, halow_options_t *opts)
{
    int ch;
    int option_index = 0;
    unsigned value;
    static const struct option long_options[] = {
        {"role", required_argument, NULL, 1000},
        {"host", required_argument, NULL, 1001},
        {"port", required_argument, NULL, 1002},
        {"in-dir", required_argument, NULL, 1003},
        {"out-dir", required_argument, NULL, 1004},
        {"preview-file", required_argument, NULL, 1005},
        {"meta-file", required_argument, NULL, 1006},
        {"stop-file", required_argument, NULL, 1007},
        {"payload", required_argument, NULL, 1008},
        {"preview-width", required_argument, NULL, 1009},
        {"preview-height", required_argument, NULL, 1010},
        {"stream-width", required_argument, NULL, 1011},
        {"stream-height", required_argument, NULL, 1012},
        {"idle-us", required_argument, NULL, 1013},
        {"packet-gap-us", required_argument, NULL, 1014},
        {"repeat", required_argument, NULL, 1015},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    options_init(opts);
    while((ch = getopt_long(argc, argv, "h", long_options,
                            &option_index)) != -1) {
        switch(ch) {
        case 1000:
            if(strcmp(optarg, "tx") == 0) {
                opts->role = HALOW_ROLE_TX;
            } else if(strcmp(optarg, "rx") == 0) {
                opts->role = HALOW_ROLE_RX;
            } else {
                return -1;
            }
            break;
        case 1001:
            snprintf(opts->host, sizeof(opts->host), "%s", optarg);
            break;
        case 1002:
            if(parse_uint_arg(optarg, &value) != 0 || value == 0 ||
               value > 65535U) {
                return -1;
            }
            opts->port = (int)value;
            break;
        case 1003:
            snprintf(opts->in_dir, sizeof(opts->in_dir), "%s", optarg);
            break;
        case 1004:
            snprintf(opts->out_dir, sizeof(opts->out_dir), "%s", optarg);
            break;
        case 1005:
            snprintf(opts->preview_file, sizeof(opts->preview_file), "%s",
                     optarg);
            break;
        case 1006:
            snprintf(opts->meta_file, sizeof(opts->meta_file), "%s", optarg);
            break;
        case 1007:
            snprintf(opts->stop_file, sizeof(opts->stop_file), "%s", optarg);
            break;
        case 1008:
            if(parse_uint_arg(optarg, &opts->payload_size) != 0 ||
               opts->payload_size < 256U || opts->payload_size > 1400U) {
                return -1;
            }
            break;
        case 1009:
            if(parse_uint_arg(optarg, &opts->preview_w) != 0 ||
               opts->preview_w < 160U || opts->preview_w > 1920U) {
                return -1;
            }
            break;
        case 1010:
            if(parse_uint_arg(optarg, &opts->preview_h) != 0 ||
               opts->preview_h < 90U || opts->preview_h > 1080U) {
                return -1;
            }
            break;
        case 1011:
            if(parse_uint_arg(optarg, &opts->stream_w) != 0 ||
               opts->stream_w < 160U || opts->stream_w > 4096U) {
                return -1;
            }
            break;
        case 1012:
            if(parse_uint_arg(optarg, &opts->stream_h) != 0 ||
               opts->stream_h < 90U || opts->stream_h > 2160U) {
                return -1;
            }
            break;
        case 1013:
            if(parse_uint_arg(optarg, &opts->idle_us) != 0 ||
               opts->idle_us > 1000000U) {
                return -1;
            }
            break;
        case 1014:
            if(parse_uint_arg(optarg, &opts->packet_gap_us) != 0 ||
               opts->packet_gap_us > 1000000U) {
                return -1;
            }
            break;
        case 1015:
            if(parse_uint_arg(optarg, &opts->repeat) != 0 ||
               opts->repeat < 1U || opts->repeat > 4U) {
                return -1;
            }
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            return -1;
        }
    }

    if(opts->role == HALOW_ROLE_TX &&
       (!opts->host[0] || !opts->in_dir[0])) {
        return -1;
    }
    if(opts->role == HALOW_ROLE_RX && !opts->out_dir[0]) {
        return -1;
    }
    return 0;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

static int stop_requested(const halow_options_t *opts)
{
    return g_stop || (opts->stop_file[0] && access(opts->stop_file, F_OK) == 0);
}

static int mkdir_p(const char *path)
{
    char tmp[512];
    char *p;

    if(!path || !path[0]) {
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s", path);
    for(p = tmp + 1; *p; p++) {
        if(*p == '/') {
            *p = '\0';
            if(mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if(mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int has_jpeg_suffix(const char *name)
{
    size_t len;

    if(!name) {
        return 0;
    }
    len = strlen(name);
    return (len > 4U && strcmp(name + len - 4U, ".jpg") == 0) ||
           (len > 5U && strcmp(name + len - 5U, ".jpeg") == 0);
}

static int find_newest_jpeg(const char *dir, char *out, size_t out_len)
{
    DIR *dp;
    struct dirent *de;
    char best[256] = "";

    dp = opendir(dir);
    if(!dp) {
        return -1;
    }

    while((de = readdir(dp)) != NULL) {
        if(de->d_name[0] == '.' || strstr(de->d_name, ".tmp") ||
           !has_jpeg_suffix(de->d_name)) {
            continue;
        }
        if(!best[0] || strcmp(de->d_name, best) > 0) {
            snprintf(best, sizeof(best), "%s", de->d_name);
        }
    }
    closedir(dp);

    if(!best[0]) {
        return -1;
    }
    snprintf(out, out_len, "%s/%s", dir, best);
    return 0;
}

static void trim_older_jpegs(const char *dir, const char *keep_path)
{
    DIR *dp;
    struct dirent *de;
    const char *keep_name = strrchr(keep_path, '/');

    keep_name = keep_name ? keep_name + 1 : keep_path;
    dp = opendir(dir);
    if(!dp) {
        return;
    }
    while((de = readdir(dp)) != NULL) {
        char path[512];

        if(de->d_name[0] == '.' || !has_jpeg_suffix(de->d_name) ||
           strcmp(de->d_name, keep_name) >= 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        unlink(path);
    }
    closedir(dp);
}

static int read_file(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *fp;
    struct stat st;
    uint8_t *buf;
    size_t n;

    *out = NULL;
    *out_len = 0;
    if(stat(path, &st) != 0 || st.st_size <= 0 ||
       st.st_size > (off_t)HALOW_MAX_FRAME_SIZE) {
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)st.st_size);
    if(!buf) {
        return -1;
    }
    fp = fopen(path, "rb");
    if(!fp) {
        free(buf);
        return -1;
    }
    n = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if(n != (size_t)st.st_size) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = n;
    return 0;
}

static uint16_t rgb565_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_jmp_error_mgr *err =
        (struct jpeg_jmp_error_mgr *)cinfo->err;

    longjmp(err->setjmp_buffer, 1);
}

static int decode_jpeg_to_preview(const uint8_t *jpeg, size_t jpeg_len,
                                  const char *preview_file,
                                  unsigned preview_w, unsigned preview_h)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_jmp_error_mgr jerr;
    uint8_t *rgb = NULL;
    uint8_t *row = NULL;
    uint8_t *rgb565 = NULL;
    FILE *fp = NULL;
    char tmp_path[512];
    unsigned src_w;
    unsigned src_h;
    unsigned draw_w;
    unsigned draw_h;
    unsigned off_x;
    unsigned off_y;
    int ret = -1;

    if(!jpeg || jpeg_len == 0 || !preview_file || !preview_file[0] ||
       preview_w == 0 || preview_h == 0) {
        return -1;
    }

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    if(setjmp(jerr.setjmp_buffer)) {
        goto out;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)jpeg, jpeg_len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    while(cinfo.scale_denom < 8 &&
          cinfo.image_width / (cinfo.scale_denom * 2U) >= preview_w &&
          cinfo.image_height / (cinfo.scale_denom * 2U) >= preview_h) {
        cinfo.scale_denom *= 2U;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    src_w = cinfo.output_width;
    src_h = cinfo.output_height;
    if(src_w == 0 || src_h == 0) {
        goto out;
    }
    rgb = (uint8_t *)malloc((size_t)src_w * src_h * 3U);
    row = (uint8_t *)malloc((size_t)src_w * 3U);
    rgb565 = (uint8_t *)calloc((size_t)preview_w * preview_h, 2U);
    if(!rgb || !row || !rgb565) {
        goto out;
    }

    while(cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row_pointer[1];
        unsigned y = cinfo.output_scanline;

        row_pointer[0] = row;
        jpeg_read_scanlines(&cinfo, row_pointer, 1);
        memcpy(rgb + (size_t)y * src_w * 3U, row, (size_t)src_w * 3U);
    }
    jpeg_finish_decompress(&cinfo);

    draw_w = preview_w;
    draw_h = preview_h;
    if((uint64_t)src_w * preview_h > (uint64_t)src_h * preview_w) {
        draw_h = (unsigned)(((uint64_t)src_h * preview_w) / src_w);
        if(draw_h == 0) {
            draw_h = 1;
        }
    } else {
        draw_w = (unsigned)(((uint64_t)src_w * preview_h) / src_h);
        if(draw_w == 0) {
            draw_w = 1;
        }
    }
    off_x = (preview_w - draw_w) / 2U;
    off_y = (preview_h - draw_h) / 2U;

    for(unsigned y = 0; y < draw_h; y++) {
        unsigned sy = (unsigned)(((uint64_t)y * src_h) / draw_h);

        if(sy >= src_h) {
            sy = src_h - 1U;
        }
        for(unsigned x = 0; x < draw_w; x++) {
            unsigned sx = (unsigned)(((uint64_t)x * src_w) / draw_w);
            size_t src_off;
            size_t dst_off;
            uint16_t px;

            if(sx >= src_w) {
                sx = src_w - 1U;
            }
            src_off = ((size_t)sy * src_w + sx) * 3U;
            dst_off = ((size_t)(off_y + y) * preview_w + (off_x + x)) * 2U;
            px = rgb565_from_rgb(rgb[src_off], rgb[src_off + 1],
                                 rgb[src_off + 2]);
            rgb565[dst_off + 0] = (uint8_t)(px & 0xFFU);
            rgb565[dst_off + 1] = (uint8_t)(px >> 8);
        }
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", preview_file);
    fp = fopen(tmp_path, "wb");
    if(!fp) {
        goto out;
    }
    if(fwrite(rgb565, 1, (size_t)preview_w * preview_h * 2U, fp) !=
       (size_t)preview_w * preview_h * 2U) {
        goto out;
    }
    if(fclose(fp) != 0) {
        fp = NULL;
        goto out;
    }
    fp = NULL;
    if(rename(tmp_path, preview_file) != 0) {
        unlink(tmp_path);
        goto out;
    }
    ret = 0;

out:
    if(fp) {
        fclose(fp);
    }
    jpeg_destroy_decompress(&cinfo);
    free(rgb);
    free(row);
    free(rgb565);
    return ret;
}

static void write_meta(const halow_options_t *opts, const char *role,
                       const char *frame_path, const halow_stats_t *stats,
                       unsigned frame_size)
{
    FILE *fp;
    double elapsed_s;
    double mbps;
    double fps;
    char tmp[512];

    if(!opts->meta_file[0] || !stats) {
        return;
    }
    elapsed_s = stats->start_us ?
                (double)(monotonic_us() - stats->start_us) / 1000000.0 : 0.0;
    mbps = elapsed_s > 0.05 ?
           ((double)stats->bytes * 8.0) / elapsed_s / 1000000.0 : 0.0;
    fps = elapsed_s > 0.05 ? (double)stats->frames / elapsed_s : 0.0;

    snprintf(tmp, sizeof(tmp), "%s.tmp", opts->meta_file);
    fp = fopen(tmp, "w");
    if(!fp) {
        return;
    }
    fprintf(fp,
            "role=%s\nframe_file=%s\nwidth=%u\nheight=%u\n"
            "stream_width=%u\nstream_height=%u\nframes=%llu\npackets=%llu\n"
            "bytes=%llu\ndropped=%llu\nerrors=%llu\nmbps=%.3f\nfps=%.2f\n"
            "last_frame_bytes=%u\n",
            role, frame_path ? frame_path : "", opts->preview_w,
            opts->preview_h, opts->stream_w, opts->stream_h,
            (unsigned long long)stats->frames,
            (unsigned long long)stats->packets,
            (unsigned long long)stats->bytes,
            (unsigned long long)stats->dropped,
            (unsigned long long)stats->errors, mbps, fps, frame_size);
    fclose(fp);
    rename(tmp, opts->meta_file);
}

static void log_stats(const char *role, const halow_stats_t *stats,
                      unsigned stream_w, unsigned stream_h,
                      unsigned frame_size)
{
    double elapsed_s = stats->start_us ?
        (double)(monotonic_us() - stats->start_us) / 1000000.0 : 0.0;
    double mbps = elapsed_s > 0.05 ?
        ((double)stats->bytes * 8.0) / elapsed_s / 1000000.0 : 0.0;
    double fps = elapsed_s > 0.05 ? (double)stats->frames / elapsed_s : 0.0;

    printf("STAT role=%s stream=%ux%u mbps=%.3f fps=%.2f frames=%llu "
           "packets=%llu bytes=%llu dropped=%llu errors=%llu last=%u\n",
           role, stream_w, stream_h, mbps, fps,
           (unsigned long long)stats->frames,
           (unsigned long long)stats->packets,
           (unsigned long long)stats->bytes,
           (unsigned long long)stats->dropped,
           (unsigned long long)stats->errors, frame_size);
    fflush(stdout);
}

static void fill_packet_header(uint8_t *packet, unsigned frame_id,
                               unsigned chunk_index, unsigned chunk_count,
                               unsigned payload_len, unsigned stride,
                               unsigned stream_w, unsigned stream_h,
                               unsigned frame_size, unsigned frame_crc)
{
    memcpy(packet, HALOW_MAGIC, 4);
    packet[4] = HALOW_VERSION;
    packet[5] = HALOW_TYPE_DATA;
    put_u16(packet + 6, HALOW_HEADER_SIZE);
    put_u32(packet + 8, frame_id);
    put_u16(packet + 12, (uint16_t)chunk_index);
    put_u16(packet + 14, (uint16_t)chunk_count);
    put_u16(packet + 16, (uint16_t)payload_len);
    put_u16(packet + 18, (uint16_t)stride);
    put_u16(packet + 20, (uint16_t)stream_w);
    put_u16(packet + 22, (uint16_t)stream_h);
    put_u32(packet + 24, frame_size);
    put_u32(packet + 28, frame_crc);
    put_u64(packet + 32, monotonic_us());
}

static int parse_packet_header(const uint8_t *packet, size_t len,
                               unsigned *frame_id, unsigned *chunk_index,
                               unsigned *chunk_count, unsigned *payload_len,
                               unsigned *stride, unsigned *stream_w,
                               unsigned *stream_h, unsigned *frame_size,
                               unsigned *frame_crc, uint64_t *timestamp_us)
{
    unsigned header_len;

    if(len < HALOW_HEADER_SIZE || memcmp(packet, HALOW_MAGIC, 4) != 0 ||
       packet[4] != HALOW_VERSION || packet[5] != HALOW_TYPE_DATA) {
        return -1;
    }
    header_len = get_u16(packet + 6);
    if(header_len != HALOW_HEADER_SIZE || len < header_len) {
        return -1;
    }
    *frame_id = get_u32(packet + 8);
    *chunk_index = get_u16(packet + 12);
    *chunk_count = get_u16(packet + 14);
    *payload_len = get_u16(packet + 16);
    *stride = get_u16(packet + 18);
    *stream_w = get_u16(packet + 20);
    *stream_h = get_u16(packet + 22);
    *frame_size = get_u32(packet + 24);
    *frame_crc = get_u32(packet + 28);
    *timestamp_us = get_u64(packet + 32);
    if(*chunk_count == 0 || *chunk_count > HALOW_MAX_CHUNKS ||
       *chunk_index >= *chunk_count || *payload_len == 0 ||
       *payload_len > *stride || *frame_size == 0 ||
       *frame_size > HALOW_MAX_FRAME_SIZE ||
       HALOW_HEADER_SIZE + *payload_len > len) {
        return -1;
    }
    return 0;
}

static int tx_send_frame(int fd, const struct sockaddr_in *addr,
                         const halow_options_t *opts, const uint8_t *data,
                         size_t len, unsigned frame_id)
{
    unsigned chunk_count;
    uint32_t crc;
    uint8_t *packet;
    int ret = 0;

    chunk_count = (unsigned)((len + opts->payload_size - 1U) /
                             opts->payload_size);
    if(chunk_count == 0 || chunk_count > HALOW_MAX_CHUNKS) {
        return -1;
    }
    packet = (uint8_t *)malloc(HALOW_HEADER_SIZE + opts->payload_size);
    if(!packet) {
        return -1;
    }
    crc = crc32_update(0, data, len);
    for(unsigned repeat = 0; repeat < opts->repeat && ret == 0; repeat++) {
        for(unsigned i = 0; i < chunk_count; i++) {
            size_t off = (size_t)i * opts->payload_size;
            size_t payload_len = len - off;
            ssize_t sent;

            if(payload_len > opts->payload_size) {
                payload_len = opts->payload_size;
            }
            fill_packet_header(packet, frame_id, i, chunk_count,
                               (unsigned)payload_len, opts->payload_size,
                               opts->stream_w, opts->stream_h, (unsigned)len,
                               crc);
            memcpy(packet + HALOW_HEADER_SIZE, data + off, payload_len);
            sent = sendto(fd, packet, HALOW_HEADER_SIZE + payload_len, 0,
                          (const struct sockaddr *)addr, sizeof(*addr));
            if(sent < 0) {
                ret = -1;
                break;
            }
            if(opts->packet_gap_us > 0) {
                usleep((useconds_t)opts->packet_gap_us);
            }
        }
    }
    free(packet);
    return ret;
}

static int run_tx(const halow_options_t *opts)
{
    int fd;
    struct sockaddr_in addr;
    halow_stats_t stats;
    char last_path[512] = "";
    unsigned frame_id = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)opts->port);
    if(inet_pton(AF_INET, opts->host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid host: %s\n", opts->host);
        close(fd);
        return 2;
    }

    memset(&stats, 0, sizeof(stats));
    stats.start_us = monotonic_us();
    stats.last_log_us = stats.start_us;
    printf("Halow UDP TX host=%s port=%d stream=%ux%u preview=%ux%u\n",
           opts->host, opts->port, opts->stream_w, opts->stream_h,
           opts->preview_w, opts->preview_h);
    fflush(stdout);

    while(!stop_requested(opts)) {
        char path[512];
        uint8_t *data = NULL;
        size_t data_len = 0;
        uint64_t now_us;

        if(find_newest_jpeg(opts->in_dir, path, sizeof(path)) != 0) {
            usleep((useconds_t)opts->idle_us);
            continue;
        }
        if(strcmp(path, last_path) == 0) {
            usleep((useconds_t)opts->idle_us);
            continue;
        }
        snprintf(last_path, sizeof(last_path), "%s", path);
        trim_older_jpegs(opts->in_dir, path);

        if(read_file(path, &data, &data_len) != 0) {
            stats.errors++;
            unlink(path);
            continue;
        }

        if(tx_send_frame(fd, &addr, opts, data, data_len, frame_id) == 0) {
            unsigned packets =
                (unsigned)((data_len + opts->payload_size - 1U) /
                           opts->payload_size);

            stats.frames++;
            stats.packets += (uint64_t)packets * opts->repeat;
            stats.bytes += data_len;
            decode_jpeg_to_preview(data, data_len, opts->preview_file,
                                   opts->preview_w, opts->preview_h);
            write_meta(opts, "TX", path, &stats, (unsigned)data_len);
            frame_id++;
        } else {
            stats.errors++;
        }

        free(data);
        unlink(path);

        now_us = monotonic_us();
        if(now_us - stats.last_log_us >= HALOW_STATS_INTERVAL_US) {
            stats.last_log_us = now_us;
            log_stats("TX", &stats, opts->stream_w, opts->stream_h,
                      (unsigned)data_len);
        }
    }
    log_stats("TX", &stats, opts->stream_w, opts->stream_h, 0);
    close(fd);
    return 0;
}

static void rx_frame_reset(halow_rx_frame_t *frame)
{
    if(!frame) {
        return;
    }
    free(frame->data);
    free(frame->seen);
    memset(frame, 0, sizeof(*frame));
}

static int rx_frame_prepare(halow_rx_frame_t *frame, unsigned frame_id,
                            unsigned stream_w, unsigned stream_h,
                            unsigned frame_size, unsigned frame_crc,
                            unsigned stride, unsigned chunk_count)
{
    rx_frame_reset(frame);
    frame->data = (uint8_t *)malloc(frame_size);
    frame->seen = (uint8_t *)calloc(chunk_count, 1U);
    if(!frame->data || !frame->seen) {
        rx_frame_reset(frame);
        return -1;
    }
    frame->frame_id = frame_id;
    frame->stream_w = stream_w;
    frame->stream_h = stream_h;
    frame->frame_size = frame_size;
    frame->frame_crc = frame_crc;
    frame->stride = stride;
    frame->chunk_count = chunk_count;
    frame->first_us = monotonic_us();
    return 0;
}

static int write_rx_jpeg(const halow_options_t *opts, unsigned frame_id,
                         const uint8_t *data, size_t len, char *out_path,
                         size_t out_len)
{
    FILE *fp;
    char tmp[512];

    snprintf(out_path, out_len, "%s/halow_%010u.jpg", opts->out_dir,
             frame_id);
    snprintf(tmp, sizeof(tmp), "%s.tmp", out_path);
    fp = fopen(tmp, "wb");
    if(!fp) {
        return -1;
    }
    if(fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        unlink(tmp);
        return -1;
    }
    if(fclose(fp) != 0) {
        unlink(tmp);
        return -1;
    }
    if(rename(tmp, out_path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int run_rx(const halow_options_t *opts)
{
    int fd;
    struct sockaddr_in addr;
    halow_stats_t stats;
    halow_rx_frame_t frame;
    uint8_t packet[HALOW_HEADER_SIZE + 1500U];

    if(mkdir_p(opts->out_dir) != 0) {
        fprintf(stderr, "mkdir %s failed: %s\n", opts->out_dir,
                strerror(errno));
        return 1;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        perror("socket");
        return 1;
    }
    {
        int yes = 1;
        int buf = 4 * 1024 * 1024;

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)opts->port);
    if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    memset(&stats, 0, sizeof(stats));
    memset(&frame, 0, sizeof(frame));
    stats.start_us = monotonic_us();
    stats.last_log_us = stats.start_us;
    printf("Halow UDP RX port=%d preview=%ux%u\n", opts->port,
           opts->preview_w, opts->preview_h);
    fflush(stdout);

    while(!stop_requested(opts)) {
        fd_set rfds;
        struct timeval tv;
        ssize_t n;
        unsigned frame_id;
        unsigned chunk_index;
        unsigned chunk_count;
        unsigned payload_len;
        unsigned stride;
        unsigned stream_w;
        unsigned stream_h;
        unsigned frame_size;
        unsigned frame_crc;
        uint64_t packet_ts;
        uint64_t now_us;
        int ready;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        ready = select(fd + 1, &rfds, NULL, NULL, &tv);
        if(ready < 0) {
            if(errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }
        now_us = monotonic_us();
        if(ready == 0) {
            if(frame.data && now_us - frame.first_us > 2000000ULL) {
                stats.dropped++;
                rx_frame_reset(&frame);
            }
            if(now_us - stats.last_log_us >= HALOW_STATS_INTERVAL_US) {
                stats.last_log_us = now_us;
                log_stats("RX", &stats, opts->stream_w, opts->stream_h, 0);
            }
            continue;
        }

        n = recv(fd, packet, sizeof(packet), 0);
        if(n <= 0) {
            continue;
        }
        if(parse_packet_header(packet, (size_t)n, &frame_id, &chunk_index,
                               &chunk_count, &payload_len, &stride,
                               &stream_w, &stream_h, &frame_size, &frame_crc,
                               &packet_ts) != 0) {
            stats.errors++;
            continue;
        }
        (void)packet_ts;
        if(!frame.data || frame.frame_id != frame_id) {
            if(frame.data) {
                stats.dropped++;
            }
            if(rx_frame_prepare(&frame, frame_id, stream_w, stream_h,
                                frame_size, frame_crc, stride,
                                chunk_count) != 0) {
                stats.errors++;
                continue;
            }
        }
        if(frame.frame_size != frame_size || frame.frame_crc != frame_crc ||
           frame.stride != stride || frame.chunk_count != chunk_count) {
            stats.errors++;
            continue;
        }
        if(!frame.seen[chunk_index]) {
            size_t off = (size_t)chunk_index * stride;

            if(off + payload_len > frame.frame_size) {
                stats.errors++;
                continue;
            }
            memcpy(frame.data + off, packet + HALOW_HEADER_SIZE, payload_len);
            frame.seen[chunk_index] = 1;
            frame.received_count++;
            stats.packets++;
        }

        if(frame.received_count == frame.chunk_count) {
            uint32_t crc = crc32_update(0, frame.data, frame.frame_size);
            char path[512] = "";

            if(crc == frame.frame_crc &&
               write_rx_jpeg(opts, frame.frame_id, frame.data,
                             frame.frame_size, path, sizeof(path)) == 0) {
                stats.frames++;
                stats.bytes += frame.frame_size;
                decode_jpeg_to_preview(frame.data, frame.frame_size,
                                       opts->preview_file, opts->preview_w,
                                       opts->preview_h);
                write_meta(opts, "RX", path, &stats, frame.frame_size);
            } else {
                stats.errors++;
            }
            rx_frame_reset(&frame);
        }

        now_us = monotonic_us();
        if(now_us - stats.last_log_us >= HALOW_STATS_INTERVAL_US) {
            stats.last_log_us = now_us;
            log_stats("RX", &stats, stream_w, stream_h, frame_size);
        }
    }

    log_stats("RX", &stats, opts->stream_w, opts->stream_h, 0);
    rx_frame_reset(&frame);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    halow_options_t opts;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    if(parse_args(argc, argv, &opts) != 0) {
        usage(argv[0]);
        return 2;
    }
    if(opts.role == HALOW_ROLE_TX) {
        return run_tx(&opts);
    }
    return run_rx(&opts);
}
