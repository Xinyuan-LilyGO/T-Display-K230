#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEFAULT_ROOT "/root/maps"
#define DEFAULT_STYLE "openstreetmap"
#define DEFAULT_STATUS "/tmp/k230_map_download_status.json"
#define DEFAULT_LOG "/tmp/k230_map_tile_downloader.log"
#define DEFAULT_CANCEL "/tmp/k230_map_download.cancel"
#define DEFAULT_MAX_TILES 2000U
#define DEFAULT_DELAY_MS 150U
#define DEFAULT_MIN_FREE_BYTES (16ULL * 1024ULL * 1024ULL)
#define DEFAULT_AVG_TILE_BYTES 120000ULL
#define DEFAULT_WRITE_HEADROOM_BYTES (4ULL * 1024ULL * 1024ULL)

typedef struct {
    int z;
    int x0;
    int x1;
    int y0;
    int y1;
    unsigned int count;
    unsigned int existing;
} tile_range_t;

typedef struct {
    const char *root;
    const char *style;
    const char *url_template;
    const char *status_path;
    const char *log_path;
    const char *cancel_path;
    double lat;
    double lon;
    double radius_km;
    int min_zoom;
    int max_zoom;
    unsigned int max_tiles;
    unsigned int delay_ms;
} options_t;

typedef struct {
    const options_t *opts;
    unsigned int total;
    unsigned int done;
    unsigned int skipped;
    unsigned int failed;
    unsigned long long bytes;
    time_t started;
} progress_t;

static void log_msg(const options_t *opts, const char *fmt, ...)
{
    FILE *fp;
    va_list ap;
    time_t now;
    struct tm tm_now;
    char ts[32];

    if(!opts || !opts->log_path) {
        return;
    }
    fp = fopen(opts->log_path, "a");
    if(!fp) {
        return;
    }
    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(fp, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static void json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t pos = 0;

    if(!dst || dst_len == 0U) {
        return;
    }
    if(!src) {
        src = "";
    }
    while(*src && pos + 2U < dst_len) {
        unsigned char ch = (unsigned char)*src++;
        if(ch == '"' || ch == '\\') {
            if(pos + 2U >= dst_len) {
                break;
            }
            dst[pos++] = '\\';
            dst[pos++] = (char)ch;
        } else if(ch == '\n' || ch == '\r' || ch == '\t') {
            dst[pos++] = ' ';
        } else if(ch >= 0x20U) {
            dst[pos++] = (char)ch;
        }
    }
    dst[pos] = '\0';
}

static void write_status(progress_t *progress, const char *state,
                         const char *current, const char *message)
{
    const options_t *opts;
    FILE *fp;
    char current_json[256];
    char message_json[256];
    double elapsed;
    double speed_kbps = 0.0;
    unsigned int eta_sec = 0;
    time_t now;

    if(!progress || !progress->opts || !progress->opts->status_path) {
        return;
    }
    opts = progress->opts;
    now = time(NULL);
    elapsed = difftime(now, progress->started);
    if(elapsed > 0.25) {
        speed_kbps = (double)progress->bytes / 1024.0 / elapsed;
        if(speed_kbps > 0.1 && progress->done < progress->total) {
            unsigned int remain = progress->total - progress->done;
            eta_sec = (unsigned int)((double)remain *
                                     (elapsed / (double)(progress->done + 1U)));
        }
    }

    json_escape(current, current_json, sizeof(current_json));
    json_escape(message, message_json, sizeof(message_json));
    fp = fopen(opts->status_path, "w");
    if(!fp) {
        return;
    }
    fprintf(fp,
            "{"
            "\"state\":\"%s\","
            "\"total\":%u,"
            "\"done\":%u,"
            "\"skipped\":%u,"
            "\"failed\":%u,"
            "\"bytes\":%llu,"
            "\"speed_kbps\":%.1f,"
            "\"eta_sec\":%u,"
            "\"current\":\"%s\","
            "\"message\":\"%s\""
            "}\n",
            state ? state : "idle", progress->total, progress->done,
            progress->skipped, progress->failed, progress->bytes, speed_kbps,
            eta_sec, current_json, message_json);
    fclose(fp);
}

static double clip_lat(double lat)
{
    if(lat > 85.05112878) {
        return 85.05112878;
    }
    if(lat < -85.05112878) {
        return -85.05112878;
    }
    return lat;
}

static double clip_lon(double lon)
{
    if(lon > 180.0) {
        return 180.0;
    }
    if(lon < -180.0) {
        return -180.0;
    }
    return lon;
}

static int lon_to_tile_x(double lon, int zoom)
{
    double n = (double)(1U << (unsigned)zoom);
    int x = (int)floor((clip_lon(lon) + 180.0) / 360.0 * n);

    if(x < 0) {
        x = 0;
    }
    if(x >= (int)n) {
        x = (int)n - 1;
    }
    return x;
}

static int lat_to_tile_y(double lat, int zoom)
{
    double n = (double)(1U << (unsigned)zoom);
    double lat_rad = clip_lat(lat) * M_PI / 180.0;
    int y = (int)floor((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) /
                        M_PI) / 2.0 * n);

    if(y < 0) {
        y = 0;
    }
    if(y >= (int)n) {
        y = (int)n - 1;
    }
    return y;
}

static int path_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}

static int mkdir_p(const char *path)
{
    char tmp[512];
    size_t len;

    if(!path || !path[0]) {
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if(len == 0U) {
        return -1;
    }
    if(tmp[len - 1U] == '/') {
        tmp[len - 1U] = '\0';
    }
    for(char *p = tmp + 1; *p; p++) {
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

static void tile_path(const options_t *opts, int z, int x, int y,
                      char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s/%d/%d/%d.png", opts->root, opts->style,
             z, x, y);
}

static void tile_parent_dir(const options_t *opts, int z, int x,
                            char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s/%d/%d", opts->root, opts->style, z, x);
}

static int png_file_valid(const char *path)
{
    static const unsigned char png_magic[8] =
        { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    FILE *fp;
    unsigned char magic[8];
    struct stat st;

    if(!path || stat(path, &st) != 0 || st.st_size < 64) {
        return 0;
    }
    fp = fopen(path, "rb");
    if(!fp) {
        return 0;
    }
    if(fread(magic, 1, sizeof(magic), fp) != sizeof(magic)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return memcmp(magic, png_magic, sizeof(magic)) == 0;
}

static int calc_ranges(const options_t *opts, tile_range_t *ranges,
                       size_t max_ranges, unsigned int *total,
                       unsigned int *existing)
{
    double lat_delta;
    double lon_delta;
    double cos_lat;
    double lat_min;
    double lat_max;
    double lon_min;
    double lon_max;
    size_t idx = 0;
    unsigned int total_local = 0;
    unsigned int existing_local = 0;

    if(!opts || !ranges || max_ranges == 0U || opts->min_zoom > opts->max_zoom) {
        return -1;
    }

    lat_delta = opts->radius_km / 111.320;
    cos_lat = cos(clip_lat(opts->lat) * M_PI / 180.0);
    if(fabs(cos_lat) < 0.01) {
        lon_delta = 180.0;
    } else {
        lon_delta = opts->radius_km / (111.320 * cos_lat);
    }
    lat_min = clip_lat(opts->lat - lat_delta);
    lat_max = clip_lat(opts->lat + lat_delta);
    lon_min = clip_lon(opts->lon - lon_delta);
    lon_max = clip_lon(opts->lon + lon_delta);

    for(int z = opts->min_zoom; z <= opts->max_zoom; z++) {
        int x0;
        int x1;
        int y0;
        int y1;
        unsigned int count;
        unsigned int present = 0;

        if(idx >= max_ranges) {
            return -1;
        }
        x0 = lon_to_tile_x(lon_min, z);
        x1 = lon_to_tile_x(lon_max, z);
        y0 = lat_to_tile_y(lat_max, z);
        y1 = lat_to_tile_y(lat_min, z);
        if(x1 < x0) {
            int tmp = x0;
            x0 = x1;
            x1 = tmp;
        }
        if(y1 < y0) {
            int tmp = y0;
            y0 = y1;
            y1 = tmp;
        }
        count = (unsigned int)(x1 - x0 + 1) *
                (unsigned int)(y1 - y0 + 1);
        for(int y = y0; y <= y1; y++) {
            for(int x = x0; x <= x1; x++) {
                char path[512];
                tile_path(opts, z, x, y, path, sizeof(path));
                if(png_file_valid(path)) {
                    present++;
                }
            }
        }
        ranges[idx].z = z;
        ranges[idx].x0 = x0;
        ranges[idx].x1 = x1;
        ranges[idx].y0 = y0;
        ranges[idx].y1 = y1;
        ranges[idx].count = count;
        ranges[idx].existing = present;
        idx++;
        total_local += count;
        existing_local += present;
    }
    if(total) {
        *total = total_local;
    }
    if(existing) {
        *existing = existing_local;
    }
    return (int)idx;
}

static int has_default_route(void)
{
    FILE *fp = fopen("/proc/net/route", "r");
    char line[256];

    if(!fp) {
        return 0;
    }
    while(fgets(line, sizeof(line), fp)) {
        char iface[32];
        unsigned long destination;
        unsigned long flags;

        if(sscanf(line, "%31s %lx %*x %lx", iface, &destination, &flags) == 3) {
            if(strcmp(iface, "lo") != 0 && destination == 0UL &&
               (flags & 0x1UL) != 0UL) {
                fclose(fp);
                return 1;
            }
        }
    }
    fclose(fp);
    return 0;
}

static int check_free_space(const options_t *opts, unsigned int missing)
{
    struct statvfs vfs;
    unsigned long long free_bytes;
    unsigned long long estimate_bytes;
    unsigned long long required_bytes;

    if(!opts || statvfs(opts->root, &vfs) != 0) {
        if(statvfs("/", &vfs) != 0) {
            return -1;
        }
    }
    free_bytes = (unsigned long long)vfs.f_bavail *
                 (unsigned long long)vfs.f_frsize;
    estimate_bytes = (unsigned long long)missing * DEFAULT_AVG_TILE_BYTES;
    required_bytes = estimate_bytes + DEFAULT_MIN_FREE_BYTES +
                     DEFAULT_WRITE_HEADROOM_BYTES;
    log_msg(opts, "free space free=%llu required=%llu estimate=%llu missing=%u",
            free_bytes, required_bytes, estimate_bytes, missing);
    if(free_bytes < required_bytes) {
        return -1;
    }
    return 0;
}

static int url_template_valid(const char *url_template)
{
    if(!url_template || !url_template[0]) {
        return 0;
    }
    if(strncmp(url_template, "http://", 7) != 0 &&
       strncmp(url_template, "https://", 8) != 0) {
        return 0;
    }
    if(strstr(url_template, "example.com")) {
        return 0;
    }
    return strstr(url_template, "{z}") && strstr(url_template, "{x}") &&
           strstr(url_template, "{y}");
}

static int make_tile_url(const char *tmpl, int z, int x, int y,
                         char *out, size_t out_len)
{
    size_t pos = 0;

    if(!tmpl || !out || out_len == 0U) {
        return -1;
    }
    while(*tmpl && pos + 1U < out_len) {
        if(strncmp(tmpl, "{z}", 3) == 0) {
            int written = snprintf(out + pos, out_len - pos, "%d", z);
            if(written < 0 || (size_t)written >= out_len - pos) {
                return -1;
            }
            pos += (size_t)written;
            tmpl += 3;
        } else if(strncmp(tmpl, "{x}", 3) == 0) {
            int written = snprintf(out + pos, out_len - pos, "%d", x);
            if(written < 0 || (size_t)written >= out_len - pos) {
                return -1;
            }
            pos += (size_t)written;
            tmpl += 3;
        } else if(strncmp(tmpl, "{y}", 3) == 0) {
            int written = snprintf(out + pos, out_len - pos, "%d", y);
            if(written < 0 || (size_t)written >= out_len - pos) {
                return -1;
            }
            pos += (size_t)written;
            tmpl += 3;
        } else {
            out[pos++] = *tmpl++;
        }
    }
    out[pos] = '\0';
    return *tmpl == '\0' ? 0 : -1;
}

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb,
                            void *userdata)
{
    FILE *fp = (FILE *)userdata;

    return fwrite(ptr, size, nmemb, fp);
}

static int curl_ca_bundle_available(void)
{
    return access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0 ||
           access("/etc/ssl/cert.pem", R_OK) == 0;
}

static int download_one(CURL *curl, const options_t *opts, int z, int x, int y,
                        unsigned long long *bytes_out, char *error,
                        size_t error_len)
{
    char url[1024];
    char path[512];
    char tmp_path[560];
    char dir[512];
    char curl_error[CURL_ERROR_SIZE] = "";
    FILE *fp;
    CURLcode res;
    long response_code = 0;
    struct stat st;

    if(make_tile_url(opts->url_template, z, x, y, url, sizeof(url)) != 0) {
        snprintf(error, error_len, "url template overflow");
        return -1;
    }
    tile_path(opts, z, x, y, path, sizeof(path));
    tile_parent_dir(opts, z, x, dir, sizeof(dir));
    if(mkdir_p(dir) != 0) {
        snprintf(error, error_len, "mkdir failed: %s", strerror(errno));
        return -1;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    fp = fopen(tmp_path, "wb");
    if(!fp) {
        snprintf(error, error_len, "open tmp failed: %s", strerror(errno));
        return -1;
    }

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if(strncmp(url, "https://", 8) == 0 && !curl_ca_bundle_available()) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "T-Display-K230-Launcher/0.2");
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if(fclose(fp) != 0 && res == CURLE_OK) {
        res = CURLE_WRITE_ERROR;
    }
    if(res != CURLE_OK || response_code < 200 || response_code >= 300) {
        snprintf(error, error_len, "HTTP %ld: %s", response_code,
                 curl_error[0] ? curl_error : curl_easy_strerror(res));
        unlink(tmp_path);
        return -1;
    }
    if(!png_file_valid(tmp_path)) {
        snprintf(error, error_len, "invalid PNG");
        unlink(tmp_path);
        return -1;
    }
    if(stat(tmp_path, &st) == 0 && bytes_out) {
        *bytes_out = (unsigned long long)st.st_size;
    }
    if(rename(tmp_path, path) != 0) {
        snprintf(error, error_len, "rename failed: %s", strerror(errno));
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static int cancel_requested(const options_t *opts)
{
    return opts && opts->cancel_path && path_exists(opts->cancel_path);
}

static int run_estimate(const options_t *opts)
{
    tile_range_t ranges[32];
    unsigned int total = 0;
    unsigned int existing = 0;
    int count = calc_ranges(opts, ranges, sizeof(ranges) / sizeof(ranges[0]),
                            &total, &existing);

    if(count < 0) {
        fprintf(stderr, "estimate failed\n");
        return 2;
    }
    printf("{\"total\":%u,\"existing\":%u,\"missing\":%u,"
           "\"min_zoom\":%d,\"max_zoom\":%d,\"radius_km\":%.2f}\n",
           total, existing, total >= existing ? total - existing : 0,
           opts->min_zoom, opts->max_zoom, opts->radius_km);
    return 0;
}

static int run_download(const options_t *opts)
{
    tile_range_t ranges[32];
    progress_t progress;
    unsigned int total = 0;
    unsigned int existing = 0;
    unsigned int missing;
    int range_count;
    CURL *curl;
    int rc = 0;

    memset(&progress, 0, sizeof(progress));
    progress.opts = opts;
    progress.started = time(NULL);
    range_count = calc_ranges(opts, ranges, sizeof(ranges) / sizeof(ranges[0]),
                              &total, &existing);
    if(range_count < 0) {
        progress.total = 0;
        write_status(&progress, "failed", "", "Estimate failed");
        return 2;
    }
    missing = total >= existing ? total - existing : 0;
    progress.total = total;
    progress.skipped = existing;
    write_status(&progress, "ready", "", "Estimated");

    if(!url_template_valid(opts->url_template)) {
        write_status(&progress, "failed", "", "Set a valid tile URL template");
        log_msg(opts, "invalid url template: %s",
                opts->url_template ? opts->url_template : "");
        return 2;
    }
    if(!has_default_route()) {
        write_status(&progress, "failed", "", "No network route");
        log_msg(opts, "no default route");
        return 2;
    }
    if(total > opts->max_tiles) {
        write_status(&progress, "failed", "", "Tile count exceeds limit");
        log_msg(opts, "tile count too large total=%u limit=%u", total,
                opts->max_tiles);
        return 2;
    }
    if(check_free_space(opts, missing) != 0) {
        write_status(&progress, "failed", "", "Not enough free space");
        log_msg(opts, "free space check failed missing=%u", missing);
        return 2;
    }
    if(mkdir_p(opts->root) != 0) {
        write_status(&progress, "failed", "", "Map directory unavailable");
        log_msg(opts, "mkdir root failed: %s", strerror(errno));
        return 2;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if(!curl) {
        write_status(&progress, "failed", "", "libcurl init failed");
        curl_global_cleanup();
        return 2;
    }

    write_status(&progress, "running", "", "Downloading");
    log_msg(opts,
            "download start lat=%.7f lon=%.7f radius=%.2f z=%d..%d total=%u existing=%u",
            opts->lat, opts->lon, opts->radius_km, opts->min_zoom,
            opts->max_zoom, total, existing);
    for(int i = 0; i < range_count; i++) {
        tile_range_t *range = &ranges[i];

        for(int y = range->y0; y <= range->y1; y++) {
            for(int x = range->x0; x <= range->x1; x++) {
                char path[512];
                char current[96];
                char error[160] = "";
                unsigned long long bytes = 0;

                snprintf(current, sizeof(current), "%d/%d/%d.png", range->z,
                         x, y);
                if(cancel_requested(opts)) {
                    write_status(&progress, "cancelled", current, "Cancelled");
                    log_msg(opts, "download cancelled");
                    rc = 1;
                    goto out;
                }
                tile_path(opts, range->z, x, y, path, sizeof(path));
                if(png_file_valid(path)) {
                    progress.done++;
                    write_status(&progress, "running", current, "Skipped");
                    continue;
                }
                if(download_one(curl, opts, range->z, x, y, &bytes, error,
                                sizeof(error)) == 0) {
                    progress.bytes += bytes;
                    progress.done++;
                    write_status(&progress, "running", current, "Downloaded");
                } else {
                    progress.failed++;
                    progress.done++;
                    write_status(&progress, "running", current,
                                 error[0] ? error : "Download failed");
                    log_msg(opts, "%s failed: %s", current,
                            error[0] ? error : "unknown");
                }
                if(opts->delay_ms > 0U) {
                    usleep(opts->delay_ms * 1000U);
                }
            }
        }
    }

out:
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    if(rc == 0) {
        write_status(&progress, progress.failed ? "failed" : "done", "",
                     progress.failed ? "Finished with failures" : "Done");
        log_msg(opts, "download complete done=%u skipped=%u failed=%u bytes=%llu",
                progress.done, progress.skipped, progress.failed,
                progress.bytes);
    }
    return rc;
}

static int parse_int_arg(const char *text, int fallback)
{
    char *end = NULL;
    long value;

    if(!text) {
        return fallback;
    }
    value = strtol(text, &end, 10);
    if(end == text) {
        return fallback;
    }
    return (int)value;
}

static double parse_double_arg(const char *text, double fallback)
{
    char *end = NULL;
    double value;

    if(!text) {
        return fallback;
    }
    value = strtod(text, &end);
    if(end == text || !isfinite(value)) {
        return fallback;
    }
    return value;
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage:\n"
            "  k230_map_tile_downloader estimate [options]\n"
            "  k230_map_tile_downloader download [options]\n"
            "\n"
            "Options:\n"
            "  --root PATH\n"
            "  --style NAME\n"
            "  --url-template URL_WITH_{z}_{x}_{y}\n"
            "  --lat VALUE --lon VALUE --radius-km VALUE\n"
            "  --min-zoom N --max-zoom N\n"
            "  --status PATH --log PATH --cancel PATH\n"
            "  --max-tiles N --request-delay-ms N\n");
}

int main(int argc, char **argv)
{
    options_t opts;
    const char *mode;

    memset(&opts, 0, sizeof(opts));
    opts.root = DEFAULT_ROOT;
    opts.style = DEFAULT_STYLE;
    opts.status_path = DEFAULT_STATUS;
    opts.log_path = DEFAULT_LOG;
    opts.cancel_path = DEFAULT_CANCEL;
    opts.radius_km = 5.0;
    opts.min_zoom = 8;
    opts.max_zoom = 12;
    opts.max_tiles = DEFAULT_MAX_TILES;
    opts.delay_ms = DEFAULT_DELAY_MS;

    if(argc < 2) {
        usage(stderr);
        return 2;
    }
    mode = argv[1];
    for(int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;

        if(strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if(strcmp(arg, "--root") == 0 && value) {
            opts.root = value;
            i++;
        } else if(strcmp(arg, "--style") == 0 && value) {
            opts.style = value;
            i++;
        } else if(strcmp(arg, "--url-template") == 0 && value) {
            opts.url_template = value;
            i++;
        } else if(strcmp(arg, "--lat") == 0 && value) {
            opts.lat = parse_double_arg(value, opts.lat);
            i++;
        } else if(strcmp(arg, "--lon") == 0 && value) {
            opts.lon = parse_double_arg(value, opts.lon);
            i++;
        } else if(strcmp(arg, "--radius-km") == 0 && value) {
            opts.radius_km = parse_double_arg(value, opts.radius_km);
            i++;
        } else if(strcmp(arg, "--min-zoom") == 0 && value) {
            opts.min_zoom = parse_int_arg(value, opts.min_zoom);
            i++;
        } else if(strcmp(arg, "--max-zoom") == 0 && value) {
            opts.max_zoom = parse_int_arg(value, opts.max_zoom);
            i++;
        } else if(strcmp(arg, "--status") == 0 && value) {
            opts.status_path = value;
            i++;
        } else if(strcmp(arg, "--log") == 0 && value) {
            opts.log_path = value;
            i++;
        } else if(strcmp(arg, "--cancel") == 0 && value) {
            opts.cancel_path = value;
            i++;
        } else if(strcmp(arg, "--max-tiles") == 0 && value) {
            int parsed = parse_int_arg(value, (int)opts.max_tiles);
            opts.max_tiles = parsed > 0 ? (unsigned int)parsed : opts.max_tiles;
            i++;
        } else if(strcmp(arg, "--request-delay-ms") == 0 && value) {
            int parsed = parse_int_arg(value, (int)opts.delay_ms);
            opts.delay_ms = parsed >= 0 ? (unsigned int)parsed : opts.delay_ms;
            i++;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", arg);
            return 2;
        }
    }

    opts.lat = clip_lat(opts.lat);
    opts.lon = clip_lon(opts.lon);
    if(opts.radius_km < 0.2) {
        opts.radius_km = 0.2;
    }
    if(opts.radius_km > 100.0) {
        opts.radius_km = 100.0;
    }
    if(opts.min_zoom < 0) {
        opts.min_zoom = 0;
    }
    if(opts.max_zoom > 20) {
        opts.max_zoom = 20;
    }
    if(opts.min_zoom > opts.max_zoom) {
        int tmp = opts.min_zoom;
        opts.min_zoom = opts.max_zoom;
        opts.max_zoom = tmp;
    }

    if(strcmp(mode, "estimate") == 0) {
        return run_estimate(&opts);
    }
    if(strcmp(mode, "download") == 0) {
        return run_download(&opts);
    }

    usage(stderr);
    return 2;
}
