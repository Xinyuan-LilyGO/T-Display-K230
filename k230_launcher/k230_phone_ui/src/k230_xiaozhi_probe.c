#define _GNU_SOURCE
#include <arpa/inet.h>
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <opus.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define APP_VERSION "0.1.0"
#define DEFAULT_URL "wss://api.tenclass.net:443/xiaozhi/v1/"
#define DEFAULT_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#define DEFAULT_CAPTURE_DEV "default"
#define DEFAULT_PLAYBACK_DEV "default"
#define DEFAULT_SAMPLE_RATE 16000
#define DEFAULT_CHANNELS 1
#define OPUS_FRAME_MS 60
#define OPUS_FRAME_SAMPLES ((DEFAULT_SAMPLE_RATE * OPUS_FRAME_MS) / 1000)
#define OPUS_MAX_PACKET 1276
#define WS_MAX_FRAME (1024 * 1024)
#define HTTP_MAX_RESPONSE (512 * 1024)
#define WAKE_VAD_WARMUP_FRAMES 4
#define WAKE_VAD_PREROLL_FRAMES 5
#define WAKE_VAD_START_FRAMES 2
#define WAKE_VAD_START_MEAN 550
#define WAKE_VAD_START_PEAK 2400
#define WAKE_VAD_STOP_MEAN 450
#define WAKE_VAD_MIN_ACTIVE_FRAMES 8
#define WAKE_VAD_SILENCE_FRAMES 12
#define WAKE_VAD_IDLE_TIMEOUT_FRAMES \
    ((5 * DEFAULT_SAMPLE_RATE + OPUS_FRAME_SAMPLES - 1) / OPUS_FRAME_SAMPLES)
#define WAKE_VAD_MAX_ACTIVE_FRAMES \
    ((10 * DEFAULT_SAMPLE_RATE + OPUS_FRAME_SAMPLES - 1) / OPUS_FRAME_SAMPLES)

typedef struct {
    int tls;
    char host[256];
    char port[16];
    char path[512];
} ws_url_t;

typedef struct {
    int fd;
    int tls;
    int timeout_ms;
    SSL_CTX *ctx;
    SSL *ssl;
} ws_conn_t;

typedef struct {
    const char *mode;
    const char *url;
    const char *ota_url;
    const char *token;
    const char *device_id;
    const char *client_id;
    const char *capture_dev;
    const char *playback_dev;
    const char *control_path;
    int seconds;
    int wait_seconds;
    int timeout_ms;
    int activate_timeout;
    int tls_verify;
    int verbose;
} app_opts_t;

typedef struct {
    int status;
    char *body;
    size_t body_len;
    char headers[8192];
} http_response_t;

typedef struct {
    char websocket_url[512];
    char websocket_token[1024];
    int websocket_version;
    char activation_code[64];
    char activation_message[256];
    char activation_challenge[256];
    int activation_timeout_ms;
} ota_config_t;

typedef struct {
    snd_pcm_t *pcm;
    const char *name;
    unsigned int rate;
    int channels;
    snd_pcm_stream_t stream;
} pcm_handle_t;

typedef struct {
    int read_fd;
    int keep_fd;
    char buf[256];
    size_t len;
} control_reader_t;

typedef enum {
    SESSION_RECORD_MANUAL = 0,
    SESSION_RECORD_WAKE_VAD,
} session_record_mode_t;

typedef struct {
    unsigned char data[OPUS_MAX_PACKET];
    int len;
} opus_preroll_frame_t;

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_record_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void on_record_stop_signal(int sig)
{
    (void)sig;
    g_record_stop = 1;
}

static void log_line(const char *level, const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);

    fprintf(stderr, "[%s.%03ld] %-5s ", stamp, ts.tv_nsec / 1000000, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static const char *env_or_default(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

static void trim_newline(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || isspace((unsigned char)s[n - 1]))) {
        s[--n] = '\0';
    }
}

static int read_text_file(const char *path, char *buf, size_t len)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    if (!fgets(buf, (int)len, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    trim_newline(buf);
    return buf[0] ? 0 : -1;
}

static void default_device_id(char *buf, size_t len)
{
    const char *paths[] = {
        "/sys/class/net/eth0/address",
        "/sys/class/net/wlan0/address",
        "/sys/class/net/usb0/address",
        NULL
    };
    for (int i = 0; paths[i]; ++i) {
        if (read_text_file(paths[i], buf, len) == 0) {
            return;
        }
    }
    snprintf(buf, len, "02:00:00:%02x:%02x:%02x", rand() & 0xff, rand() & 0xff, rand() & 0xff);
}

static void default_client_id(char *buf, size_t len)
{
    if (read_text_file("/etc/machine-id", buf, len) == 0) {
        return;
    }

    unsigned char r[16];
    if (RAND_bytes(r, sizeof(r)) != 1) {
        for (size_t i = 0; i < sizeof(r); ++i) {
            r[i] = (unsigned char)(rand() & 0xff);
        }
    }
    snprintf(buf, len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

static int parse_url(const char *url, ws_url_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = NULL;
    if (strncmp(url, "wss://", 6) == 0) {
        out->tls = 1;
        p = url + 6;
        snprintf(out->port, sizeof(out->port), "443");
    } else if (strncmp(url, "ws://", 5) == 0) {
        out->tls = 0;
        p = url + 5;
        snprintf(out->port, sizeof(out->port), "80");
    } else {
        log_line("ERR", "URL must start with ws:// or wss://");
        return -1;
    }

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(host_end - p));
    if (colon) {
        size_t host_len = (size_t)(colon - p);
        size_t port_len = (size_t)(host_end - colon - 1);
        if (host_len == 0 || host_len >= sizeof(out->host) || port_len == 0 || port_len >= sizeof(out->port)) {
            return -1;
        }
        memcpy(out->host, p, host_len);
        out->host[host_len] = '\0';
        memcpy(out->port, colon + 1, port_len);
        out->port[port_len] = '\0';
    } else {
        size_t host_len = (size_t)(host_end - p);
        if (host_len == 0 || host_len >= sizeof(out->host)) {
            return -1;
        }
        memcpy(out->host, p, host_len);
        out->host[host_len] = '\0';
    }

    if (slash && *slash) {
        snprintf(out->path, sizeof(out->path), "%s", slash);
    } else {
        snprintf(out->path, sizeof(out->path), "/");
    }
    return 0;
}

static int parse_http_url(const char *url, ws_url_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = NULL;
    if (strncmp(url, "https://", 8) == 0) {
        out->tls = 1;
        p = url + 8;
        snprintf(out->port, sizeof(out->port), "443");
    } else if (strncmp(url, "http://", 7) == 0) {
        out->tls = 0;
        p = url + 7;
        snprintf(out->port, sizeof(out->port), "80");
    } else {
        log_line("ERR", "OTA URL must start with http:// or https://");
        return -1;
    }

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(host_end - p));
    if (colon) {
        size_t host_len = (size_t)(colon - p);
        size_t port_len = (size_t)(host_end - colon - 1);
        if (host_len == 0 || host_len >= sizeof(out->host) ||
            port_len == 0 || port_len >= sizeof(out->port)) {
            return -1;
        }
        memcpy(out->host, p, host_len);
        out->host[host_len] = '\0';
        memcpy(out->port, colon + 1, port_len);
        out->port[port_len] = '\0';
    } else {
        size_t host_len = (size_t)(host_end - p);
        if (host_len == 0 || host_len >= sizeof(out->host)) {
            return -1;
        }
        memcpy(out->host, p, host_len);
        out->host[host_len] = '\0';
    }

    if (slash && *slash) {
        snprintf(out->path, sizeof(out->path), "%s", slash);
    } else {
        snprintf(out->path, sizeof(out->path), "/");
    }
    return 0;
}

static const char g_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int remain = (int)(in_len - i);
        if (remain > 1) v |= (uint32_t)in[i + 1] << 8;
        if (remain > 2) v |= in[i + 2];

        if (j + 4 >= out_len) break;
        out[j++] = g_b64[(v >> 18) & 0x3f];
        out[j++] = g_b64[(v >> 12) & 0x3f];
        out[j++] = (remain > 1) ? g_b64[(v >> 6) & 0x3f] : '=';
        out[j++] = (remain > 2) ? g_b64[v & 0x3f] : '=';
    }
    out[j] = '\0';
}

static int tcp_connect_host(const char *host, const char *port, int timeout_ms)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        log_line("ERR", "getaddrinfo(%s:%s): %s", host, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        log_line("ERR", "connect(%s:%s) failed: %s", host, port, strerror(errno));
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static int stream_connect(ws_conn_t *ws, const char *host, const char *port,
                          int tls, const app_opts_t *opts)
{
    memset(ws, 0, sizeof(*ws));
    ws->fd = -1;
    ws->fd = tcp_connect_host(host, port, opts->timeout_ms);
    if (ws->fd < 0) {
        return -1;
    }
    ws->tls = tls;
    ws->timeout_ms = opts->timeout_ms;

    if (!ws->tls) {
        return 0;
    }

    SSL_library_init();
    SSL_load_error_strings();
    const SSL_METHOD *method = TLS_client_method();
    ws->ctx = SSL_CTX_new(method);
    if (!ws->ctx) {
        log_line("ERR", "SSL_CTX_new failed");
        return -1;
    }
    if (opts->tls_verify) {
        SSL_CTX_set_default_verify_paths(ws->ctx);
        SSL_CTX_set_verify(ws->ctx, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_CTX_set_verify(ws->ctx, SSL_VERIFY_NONE, NULL);
        log_line("WARN", "TLS certificate verification is disabled for PoC");
    }
    ws->ssl = SSL_new(ws->ctx);
    SSL_set_fd(ws->ssl, ws->fd);
    SSL_set_tlsext_host_name(ws->ssl, host);
    if (SSL_connect(ws->ssl) != 1) {
        unsigned long err = ERR_get_error();
        log_line("ERR", "SSL_connect failed: %s", ERR_error_string(err, NULL));
        return -1;
    }
    return 0;
}

static int ws_raw_read(ws_conn_t *ws, void *buf, size_t len)
{
    if (ws->tls) {
        int rc = SSL_read(ws->ssl, buf, (int)len);
        if (rc <= 0) {
            int err = SSL_get_error(ws->ssl, rc);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                errno = EAGAIN;
            }
            return -1;
        }
        return rc;
    }
    return (int)recv(ws->fd, buf, len, 0);
}

static int ws_raw_write(ws_conn_t *ws, const void *buf, size_t len)
{
    if (ws->tls) {
        int rc = SSL_write(ws->ssl, buf, (int)len);
        if (rc <= 0) {
            return -1;
        }
        return rc;
    }
    return (int)send(ws->fd, buf, len, 0);
}

static int ws_write_all(ws_conn_t *ws, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        int n = ws_raw_write(ws, p, len);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int ws_read_exact(ws_conn_t *ws, void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    while (len > 0) {
        int n = ws_raw_read(ws, p, len);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int ws_connect(ws_conn_t *ws, const ws_url_t *url, const app_opts_t *opts)
{
    if (stream_connect(ws, url->host, url->port, url->tls, opts) < 0) {
        return -1;
    }

    unsigned char rnd[16];
    char key[32];
    if (RAND_bytes(rnd, sizeof(rnd)) != 1) {
        for (size_t i = 0; i < sizeof(rnd); ++i) rnd[i] = rand() & 0xff;
    }
    base64_encode(rnd, sizeof(rnd), key, sizeof(key));

    char req[4096];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "Protocol-Version: 1\r\n"
                     "Device-Id: %s\r\n"
                     "Client-Id: %s\r\n"
                     "User-Agent: k230-xiaozhi-lab/%s\r\n",
                     url->path, url->host, url->port, key,
                     opts->device_id, opts->client_id, APP_VERSION);
    if (n < 0 || (size_t)n >= sizeof(req)) return -1;
    size_t off = (size_t)n;
    if (opts->token && opts->token[0]) {
        if (strchr(opts->token, ' ')) {
            n = snprintf(req + off, sizeof(req) - off,
                         "Authorization: %s\r\n", opts->token);
        } else {
            n = snprintf(req + off, sizeof(req) - off,
                         "Authorization: Bearer %s\r\n", opts->token);
        }
        if (n < 0 || off + (size_t)n >= sizeof(req)) return -1;
        off += (size_t)n;
    }
    n = snprintf(req + off, sizeof(req) - off, "\r\n");
    if (n < 0 || off + (size_t)n >= sizeof(req)) return -1;
    off += (size_t)n;

    if (ws_write_all(ws, req, off) < 0) {
        log_line("ERR", "send HTTP upgrade failed");
        return -1;
    }

    char hdr[8192];
    size_t used = 0;
    while (used + 1 < sizeof(hdr)) {
        char c;
        if (ws_read_exact(ws, &c, 1) < 0) {
            log_line("ERR", "read HTTP upgrade response failed");
            return -1;
        }
        hdr[used++] = c;
        hdr[used] = '\0';
        if (used >= 4 && strstr(hdr, "\r\n\r\n")) {
            break;
        }
    }

    int status = 0;
    sscanf(hdr, "HTTP/%*s %d", &status);
    if (opts->verbose || status != 101) {
        log_line(status == 101 ? "INFO" : "ERR", "HTTP upgrade status=%d", status);
        if (status != 101) {
            fprintf(stderr, "%s\n", hdr);
        }
    }
    if (status != 101) {
        return -1;
    }
    return 0;
}

static void ws_close(ws_conn_t *ws)
{
    if (!ws) return;
    if (ws->ssl) {
        SSL_shutdown(ws->ssl);
        SSL_free(ws->ssl);
    }
    if (ws->ctx) SSL_CTX_free(ws->ctx);
    if (ws->fd >= 0) close(ws->fd);
    memset(ws, 0, sizeof(*ws));
    ws->fd = -1;
}

static void http_response_free(http_response_t *resp)
{
    if (!resp) {
        return;
    }
    free(resp->body);
    memset(resp, 0, sizeof(*resp));
}

static int http_decode_chunked(const char *body, size_t body_len,
                               char **out_body, size_t *out_len)
{
    char *out = calloc(1, body_len + 1);
    const char *p = body;
    const char *end = body + body_len;
    size_t used = 0;

    if (!out) {
        return -1;
    }

    while (p < end) {
        char *chunk_end = NULL;
        unsigned long chunk_len;

        while (p < end && (*p == '\r' || *p == '\n')) {
            p++;
        }
        if (p >= end) {
            break;
        }
        chunk_len = strtoul(p, &chunk_end, 16);
        if (chunk_end == p) {
            free(out);
            return -1;
        }
        p = chunk_end;
        while (p < end && *p != '\n') {
            p++;
        }
        if (p < end && *p == '\n') {
            p++;
        }
        if (chunk_len == 0) {
            break;
        }
        if ((size_t)(end - p) < chunk_len) {
            free(out);
            return -1;
        }
        memcpy(out + used, p, chunk_len);
        used += chunk_len;
        p += chunk_len;
    }

    out[used] = '\0';
    *out_body = out;
    *out_len = used;
    return 0;
}

static int http_request_json(const app_opts_t *opts, const char *method,
                             const char *url, const char *body,
                             http_response_t *resp)
{
    ws_url_t parsed;
    ws_conn_t conn;
    char *raw = NULL;
    size_t raw_used = 0;
    size_t raw_cap = 0;
    int rc = -1;

    memset(resp, 0, sizeof(*resp));
    if (parse_http_url(url, &parsed) != 0) {
        return -1;
    }
    if (stream_connect(&conn, parsed.host, parsed.port, parsed.tls, opts) < 0) {
        ws_close(&conn);
        return -1;
    }

    const char *payload = body ? body : "";
    size_t payload_len = strlen(payload);
    char req[8192];
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s:%s\r\n"
                     "Activation-Version: 1\r\n"
                     "Device-Id: %s\r\n"
                     "Client-Id: %s\r\n"
                     "User-Agent: T-Display-K230/%s\r\n"
                     "Accept-Language: zh-CN\r\n"
                     "Accept: application/json\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     method, parsed.path, parsed.host, parsed.port,
                     opts->device_id, opts->client_id, APP_VERSION,
                     payload_len, payload);
    if (n < 0 || (size_t)n >= sizeof(req)) {
        log_line("ERR", "HTTP request too large");
        goto out;
    }
    if (ws_write_all(&conn, req, (size_t)n) < 0) {
        log_line("ERR", "HTTP request write failed");
        goto out;
    }

    raw_cap = 16384;
    raw = calloc(1, raw_cap);
    if (!raw) {
        goto out;
    }
    while (raw_used + 1 < HTTP_MAX_RESPONSE) {
        if (raw_used + 4096 + 1 > raw_cap) {
            size_t new_cap = raw_cap * 2;
            char *next;
            if (new_cap > HTTP_MAX_RESPONSE + 1) {
                new_cap = HTTP_MAX_RESPONSE + 1;
            }
            next = realloc(raw, new_cap);
            if (!next) {
                goto out;
            }
            raw = next;
            raw_cap = new_cap;
        }
        int got = ws_raw_read(&conn, raw + raw_used, raw_cap - raw_used - 1);
        if (got <= 0) {
            break;
        }
        raw_used += (size_t)got;
        raw[raw_used] = '\0';
    }
    if (raw_used == 0) {
        log_line("ERR", "HTTP response is empty");
        goto out;
    }

    char *header_end = strstr(raw, "\r\n\r\n");
    size_t header_len = 0;
    if (!header_end) {
        header_end = strstr(raw, "\n\n");
        if (!header_end) {
            log_line("ERR", "HTTP response has no header separator");
            goto out;
        }
        header_len = (size_t)(header_end - raw);
        header_end += 2;
    } else {
        header_len = (size_t)(header_end - raw);
        header_end += 4;
    }
    if (header_len >= sizeof(resp->headers)) {
        header_len = sizeof(resp->headers) - 1;
    }
    memcpy(resp->headers, raw, header_len);
    resp->headers[header_len] = '\0';
    sscanf(resp->headers, "HTTP/%*s %d", &resp->status);

    const char *body_start = header_end;
    size_t body_len = raw_used - (size_t)(body_start - raw);
    if (strcasestr(resp->headers, "Transfer-Encoding:") &&
        strcasestr(resp->headers, "chunked")) {
        if (http_decode_chunked(body_start, body_len, &resp->body,
                                &resp->body_len) != 0) {
            log_line("ERR", "chunked response decode failed");
            goto out;
        }
    } else {
        resp->body = calloc(1, body_len + 1);
        if (!resp->body) {
            goto out;
        }
        memcpy(resp->body, body_start, body_len);
        resp->body[body_len] = '\0';
        resp->body_len = body_len;
    }

    if (opts->verbose || resp->status < 200 || resp->status >= 300) {
        log_line((resp->status >= 200 && resp->status < 300) ? "INFO" : "ERR",
                 "HTTP %s %s status=%d body_len=%zu", method, url,
                 resp->status, resp->body_len);
    }
    rc = 0;

out:
    free(raw);
    ws_close(&conn);
    if (rc != 0) {
        http_response_free(resp);
    }
    return rc;
}

static const char *json_find_object_range(const char *json, const char *key,
                                          const char **obj_end)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) {
        return NULL;
    }
    p = strchr(p + strlen(needle), ':');
    if (!p) {
        return NULL;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '{') {
        return NULL;
    }

    const char *start = p;
    int depth = 0;
    int in_string = 0;
    int escape = 0;
    for (; *p; ++p) {
        char c = *p;
        if (in_string) {
            if (escape) {
                escape = 0;
            } else if (c == '\\') {
                escape = 1;
            } else if (c == '"') {
                in_string = 0;
            }
            continue;
        }
        if (c == '"') {
            in_string = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                *obj_end = p + 1;
                return start;
            }
        }
    }
    return NULL;
}

static int json_get_string_range(const char *start, const char *end,
                                 const char *key, char *out, size_t out_len)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = start;
    while (p && p < end) {
        p = strstr(p, needle);
        if (!p || p >= end) {
            break;
        }
        p = strchr(p + strlen(needle), ':');
        if (!p || p >= end) {
            break;
        }
        p++;
        while (p < end && isspace((unsigned char)*p)) {
            p++;
        }
        if (p >= end || *p != '"') {
            continue;
        }
        p++;
        size_t n = 0;
        while (p < end && *p && *p != '"' && n + 1 < out_len) {
            if (*p == '\\' && p + 1 < end) {
                p++;
                if (*p == 'n') {
                    out[n++] = '\n';
                    p++;
                    continue;
                }
                if (*p == 'r') {
                    out[n++] = '\r';
                    p++;
                    continue;
                }
                if (*p == 't') {
                    out[n++] = '\t';
                    p++;
                    continue;
                }
            }
            out[n++] = *p++;
        }
        out[n] = '\0';
        return n ? 0 : -1;
    }
    return -1;
}

static int json_get_int_range(const char *start, const char *end,
                              const char *key, int fallback)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(start, needle);
    if (!p || p >= end) {
        return fallback;
    }
    p = strchr(p + strlen(needle), ':');
    if (!p || p >= end) {
        return fallback;
    }
    p++;
    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }
    if (p >= end || (!isdigit((unsigned char)*p) && *p != '-')) {
        return fallback;
    }
    return atoi(p);
}

static void ota_parse_config(const char *json, ota_config_t *cfg)
{
    const char *obj = NULL;
    const char *obj_end = NULL;

    memset(cfg, 0, sizeof(*cfg));
    cfg->websocket_version = 1;
    cfg->activation_timeout_ms = 30000;

    obj = json_find_object_range(json, "activation", &obj_end);
    if (obj) {
        json_get_string_range(obj, obj_end, "message", cfg->activation_message,
                              sizeof(cfg->activation_message));
        json_get_string_range(obj, obj_end, "code", cfg->activation_code,
                              sizeof(cfg->activation_code));
        json_get_string_range(obj, obj_end, "challenge",
                              cfg->activation_challenge,
                              sizeof(cfg->activation_challenge));
        cfg->activation_timeout_ms =
            json_get_int_range(obj, obj_end, "timeout_ms", 30000);
    }

    obj = json_find_object_range(json, "websocket", &obj_end);
    if (obj) {
        json_get_string_range(obj, obj_end, "url", cfg->websocket_url,
                              sizeof(cfg->websocket_url));
        json_get_string_range(obj, obj_end, "token", cfg->websocket_token,
                              sizeof(cfg->websocket_token));
        cfg->websocket_version =
            json_get_int_range(obj, obj_end, "version", 1);
    }
}

static void ota_print_config(const ota_config_t *cfg)
{
    if (cfg->websocket_url[0]) {
        printf("CONFIG websocket.url=%s\n", cfg->websocket_url);
    }
    if (cfg->websocket_token[0]) {
        printf("CONFIG websocket.token=%s\n", cfg->websocket_token);
    }
    printf("CONFIG websocket.version=%d\n",
           cfg->websocket_version > 0 ? cfg->websocket_version : 1);
    fflush(stdout);
}

static int ws_send_frame(ws_conn_t *ws, int opcode, const void *payload, size_t len)
{
    unsigned char hdr[14];
    size_t h = 0;
    hdr[h++] = 0x80 | (opcode & 0x0f);
    if (len < 126) {
        hdr[h++] = 0x80 | (unsigned char)len;
    } else if (len <= 0xffff) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)((len >> 8) & 0xff);
        hdr[h++] = (unsigned char)(len & 0xff);
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; --i) {
            hdr[h++] = (unsigned char)((len >> (i * 8)) & 0xff);
        }
    }

    unsigned char mask[4];
    if (RAND_bytes(mask, sizeof(mask)) != 1) {
        for (int i = 0; i < 4; ++i) mask[i] = rand() & 0xff;
    }
    memcpy(hdr + h, mask, 4);
    h += 4;

    unsigned char *buf = malloc(h + len);
    if (!buf) return -1;
    memcpy(buf, hdr, h);
    for (size_t i = 0; i < len; ++i) {
        buf[h + i] = ((const unsigned char *)payload)[i] ^ mask[i & 3];
    }
    int rc = ws_write_all(ws, buf, h + len);
    free(buf);
    return rc;
}

static int ws_read_frame(ws_conn_t *ws, int *opcode, unsigned char **payload, size_t *len)
{
    unsigned char h[2];
    if (ws_read_exact(ws, h, 2) < 0) {
        return -1;
    }
    *opcode = h[0] & 0x0f;
    int masked = h[1] & 0x80;
    uint64_t plen = h[1] & 0x7f;
    if (plen == 126) {
        unsigned char e[2];
        if (ws_read_exact(ws, e, 2) < 0) return -1;
        plen = ((uint64_t)e[0] << 8) | e[1];
    } else if (plen == 127) {
        unsigned char e[8];
        if (ws_read_exact(ws, e, 8) < 0) return -1;
        plen = 0;
        for (int i = 0; i < 8; ++i) plen = (plen << 8) | e[i];
    }
    if (plen > WS_MAX_FRAME) {
        log_line("ERR", "WebSocket frame too large: %llu", (unsigned long long)plen);
        return -1;
    }

    unsigned char mask[4] = {0};
    if (masked && ws_read_exact(ws, mask, 4) < 0) {
        return -1;
    }

    unsigned char *buf = calloc(1, (size_t)plen + 1);
    if (!buf) return -1;
    if (plen && ws_read_exact(ws, buf, (size_t)plen) < 0) {
        free(buf);
        return -1;
    }
    if (masked) {
        for (size_t i = 0; i < (size_t)plen; ++i) buf[i] ^= mask[i & 3];
    }
    *payload = buf;
    *len = (size_t)plen;
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p = strchr(p + strlen(needle), ':');
    if (!p) return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_len) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = '\0';
    return n ? 0 : -1;
}

static int json_get_int_after(const char *json, const char *key, int fallback)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return fallback;
    p = strchr(p + strlen(needle), ':');
    if (!p) return fallback;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p)) return fallback;
    return atoi(p);
}

static void log_chat_message_from_json(const char *json)
{
    char type[32];
    char state[32];
    char text[512];
    char emotion[64];

    if (!json || json_get_string(json, "type", type, sizeof(type)) != 0) {
        return;
    }

    if (strcmp(type, "stt") == 0) {
        if (json_get_string(json, "text", text, sizeof(text)) == 0) {
            trim_newline(text);
            if (text[0]) {
                log_line("CHAT", "user: %s", text);
            }
        }
        return;
    }

    if (strcmp(type, "llm") == 0) {
        if (json_get_string(json, "emotion", emotion, sizeof(emotion)) == 0) {
            trim_newline(emotion);
            if (emotion[0]) {
                log_line("EMOTION", "%s", emotion);
            }
        }
        return;
    }

    if (strcmp(type, "tts") == 0) {
        if (json_get_string(json, "state", state, sizeof(state)) == 0 &&
            strcmp(state, "sentence_start") != 0) {
            return;
        }
        if (json_get_string(json, "text", text, sizeof(text)) == 0) {
            trim_newline(text);
            if (text[0]) {
                log_line("CHAT", "assistant: %s", text);
            }
        }
    }
}

static int json_has_type_state(const char *json, const char *type_value,
                               const char *state_value)
{
    char type[32];
    char state[32];

    if (!json || !type_value || !state_value) {
        return 0;
    }
    if (json_get_string(json, "type", type, sizeof(type)) != 0 ||
        strcmp(type, type_value) != 0) {
        return 0;
    }
    if (json_get_string(json, "state", state, sizeof(state)) != 0) {
        return 0;
    }
    return strcmp(state, state_value) == 0;
}

static void make_hello(char *buf, size_t len)
{
    snprintf(buf, len,
             "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
             "\"channels\":1,\"frame_duration\":60}}");
}

static void make_listen(char *buf, size_t len, const char *session_id, const char *state)
{
    snprintf(buf, len,
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"manual\"}",
             session_id ? session_id : "", state);
}

static int open_pcm(pcm_handle_t *ph, snd_pcm_stream_t stream, const char *name, unsigned int rate, int channels)
{
    memset(ph, 0, sizeof(*ph));
    ph->name = name;
    ph->rate = rate;
    ph->channels = channels;
    ph->stream = stream;

    int rc = snd_pcm_open(&ph->pcm, name, stream, 0);
    if (rc < 0) {
        log_line("ERR", "snd_pcm_open(%s): %s", name, snd_strerror(rc));
        return -1;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(ph->pcm, params);
    snd_pcm_hw_params_set_access(ph->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(ph->pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(ph->pcm, params, channels);
    unsigned int near_rate = rate;
    snd_pcm_hw_params_set_rate_near(ph->pcm, params, &near_rate, 0);
    snd_pcm_uframes_t period = OPUS_FRAME_SAMPLES;
    snd_pcm_hw_params_set_period_size_near(ph->pcm, params, &period, 0);
    snd_pcm_uframes_t buffer = period * 4;
    snd_pcm_hw_params_set_buffer_size_near(ph->pcm, params, &buffer);

    rc = snd_pcm_hw_params(ph->pcm, params);
    if (rc < 0) {
        log_line("ERR", "snd_pcm_hw_params(%s): %s", name, snd_strerror(rc));
        snd_pcm_close(ph->pcm);
        ph->pcm = NULL;
        return -1;
    }
    snd_pcm_prepare(ph->pcm);
    log_line("INFO", "%s PCM %s opened: rate=%u channels=%d period=%lu",
             stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
             name, near_rate, channels, (unsigned long)period);
    return 0;
}

static void close_pcm(pcm_handle_t *ph)
{
    if (ph && ph->pcm) {
        snd_pcm_drop(ph->pcm);
        snd_pcm_close(ph->pcm);
        ph->pcm = NULL;
    }
}

static int pcm_read_frames(pcm_handle_t *ph, int16_t *buf, snd_pcm_uframes_t frames)
{
    snd_pcm_uframes_t done = 0;
    while (done < frames && !g_stop) {
        snd_pcm_sframes_t rc = snd_pcm_readi(ph->pcm, buf + done * ph->channels, frames - done);
        if (rc == -EPIPE) {
            snd_pcm_prepare(ph->pcm);
            continue;
        }
        if (rc < 0) {
            log_line("ERR", "snd_pcm_readi: %s", snd_strerror((int)rc));
            snd_pcm_prepare(ph->pcm);
            return -1;
        }
        done += (snd_pcm_uframes_t)rc;
    }
    return 0;
}

static int pcm_write_frames(pcm_handle_t *ph, const int16_t *buf, snd_pcm_uframes_t frames)
{
    snd_pcm_uframes_t done = 0;
    while (done < frames && !g_stop) {
        snd_pcm_sframes_t rc = snd_pcm_writei(ph->pcm, buf + done * ph->channels, frames - done);
        if (rc == -EPIPE) {
            snd_pcm_prepare(ph->pcm);
            continue;
        }
        if (rc < 0) {
            log_line("ERR", "snd_pcm_writei: %s", snd_strerror((int)rc));
            snd_pcm_prepare(ph->pcm);
            return -1;
        }
        done += (snd_pcm_uframes_t)rc;
    }
    return 0;
}

static void control_close(control_reader_t *ctl)
{
    if (!ctl) return;
    if (ctl->read_fd >= 0) close(ctl->read_fd);
    if (ctl->keep_fd >= 0) close(ctl->keep_fd);
    memset(ctl, 0, sizeof(*ctl));
    ctl->read_fd = -1;
    ctl->keep_fd = -1;
}

static int control_open(control_reader_t *ctl, const char *path)
{
    memset(ctl, 0, sizeof(*ctl));
    ctl->read_fd = -1;
    ctl->keep_fd = -1;

    if (!path || !path[0]) {
        log_line("ERR", "missing --control FIFO path");
        return -1;
    }

    if (mkfifo(path, 0600) != 0 && errno != EEXIST) {
        log_line("ERR", "mkfifo(%s): %s", path, strerror(errno));
        return -1;
    }

    ctl->read_fd = open(path, O_RDONLY | O_NONBLOCK);
    if (ctl->read_fd < 0) {
        log_line("ERR", "open control read %s: %s", path, strerror(errno));
        control_close(ctl);
        return -1;
    }

    ctl->keep_fd = open(path, O_WRONLY | O_NONBLOCK);
    if (ctl->keep_fd < 0) {
        log_line("WARN", "open control keepalive %s: %s", path, strerror(errno));
    }
    return 0;
}

static int control_pop_line(control_reader_t *ctl, char *line, size_t line_len)
{
    if (!ctl || ctl->read_fd < 0 || !line || line_len == 0) {
        return -1;
    }

    for (;;) {
        for (size_t i = 0; i < ctl->len; ++i) {
            if (ctl->buf[i] == '\n') {
                size_t n = i;
                if (n > 0 && ctl->buf[n - 1] == '\r') n--;
                if (n >= line_len) n = line_len - 1;
                memcpy(line, ctl->buf, n);
                line[n] = '\0';
                memmove(ctl->buf, ctl->buf + i + 1, ctl->len - i - 1);
                ctl->len -= i + 1;
                trim_newline(line);
                if (!line[0]) {
                    continue;
                }
                return 1;
            }
        }

        char tmp[96];
        ssize_t rc = read(ctl->read_fd, tmp, sizeof(tmp));
        if (rc > 0) {
            if (ctl->len + (size_t)rc >= sizeof(ctl->buf)) {
                log_line("WARN", "control line overflow, dropping buffer");
                ctl->len = 0;
            }
            size_t copy = (size_t)rc;
            if (copy > sizeof(ctl->buf) - ctl->len) {
                copy = sizeof(ctl->buf) - ctl->len;
            }
            memcpy(ctl->buf + ctl->len, tmp, copy);
            ctl->len += copy;
            continue;
        }
        if (rc == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        log_line("ERR", "read control FIFO: %s", strerror(errno));
        return -1;
    }
}

static int control_wait_line(control_reader_t *ctl, char *line,
                             size_t line_len, int timeout_ms)
{
    int got = control_pop_line(ctl, line, line_len);
    if (got != 0) return got;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctl->read_fd, &rfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(ctl->read_fd + 1, &rfds, NULL, NULL, &tv);
    if (rc < 0) {
        if (errno == EINTR) return 0;
        log_line("ERR", "select control FIFO: %s", strerror(errno));
        return -1;
    }
    if (rc == 0) return 0;
    return control_pop_line(ctl, line, line_len);
}

static int command_audio_loopback(const app_opts_t *opts)
{
    pcm_handle_t cap, play;
    if (open_pcm(&cap, SND_PCM_STREAM_CAPTURE, opts->capture_dev, DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS) < 0) {
        return 1;
    }
    if (open_pcm(&play, SND_PCM_STREAM_PLAYBACK, opts->playback_dev, DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS) < 0) {
        close_pcm(&cap);
        return 1;
    }

    int err = 0;
    OpusEncoder *enc = opus_encoder_create(DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS, OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        log_line("ERR", "opus_encoder_create: %s", opus_strerror(err));
        close_pcm(&play);
        close_pcm(&cap);
        return 1;
    }
    OpusDecoder *dec = opus_decoder_create(DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS, &err);
    if (!dec || err != OPUS_OK) {
        log_line("ERR", "opus_decoder_create: %s", opus_strerror(err));
        opus_encoder_destroy(enc);
        close_pcm(&play);
        close_pcm(&cap);
        return 1;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(30000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));

    int16_t pcm[OPUS_FRAME_SAMPLES];
    int16_t decoded[OPUS_FRAME_SAMPLES * 2];
    unsigned char packet[OPUS_MAX_PACKET];
    int frames = (opts->seconds * DEFAULT_SAMPLE_RATE + OPUS_FRAME_SAMPLES - 1) / OPUS_FRAME_SAMPLES;

    log_line("INFO", "audio-loopback start: seconds=%d frames=%d", opts->seconds, frames);
    for (int i = 0; i < frames && !g_stop; ++i) {
        if (pcm_read_frames(&cap, pcm, OPUS_FRAME_SAMPLES) < 0) break;
        int nb = opus_encode(enc, pcm, OPUS_FRAME_SAMPLES, packet, sizeof(packet));
        if (nb < 0) {
            log_line("ERR", "opus_encode: %s", opus_strerror(nb));
            break;
        }
        int samples = opus_decode(dec, packet, nb, decoded, OPUS_FRAME_SAMPLES * 2, 0);
        if (samples < 0) {
            log_line("ERR", "opus_decode: %s", opus_strerror(samples));
            break;
        }
        if (pcm_write_frames(&play, decoded, (snd_pcm_uframes_t)samples) < 0) break;
        if ((i % 10) == 0) log_line("INFO", "loop frame=%d opus_bytes=%d", i, nb);
    }

    opus_decoder_destroy(dec);
    opus_encoder_destroy(enc);
    close_pcm(&play);
    close_pcm(&cap);
    log_line("INFO", "audio-loopback done");
    return 0;
}

static int connect_and_hello(const app_opts_t *opts, ws_conn_t *ws, char *session_id, size_t session_len, int *server_rate)
{
    ws_url_t url;
    if (parse_url(opts->url, &url) < 0) return -1;
    log_line("INFO", "connect %s://%s:%s%s", url.tls ? "wss" : "ws", url.host, url.port, url.path);
    if (ws_connect(ws, &url, opts) < 0) return -1;

    char hello[512];
    make_hello(hello, sizeof(hello));
    log_line("INFO", "send hello: %s", hello);
    if (ws_send_frame(ws, 1, hello, strlen(hello)) < 0) {
        log_line("ERR", "send hello failed");
        return -1;
    }

    time_t deadline = time(NULL) + opts->timeout_ms / 1000;
    while (!g_stop && time(NULL) <= deadline) {
        int opcode = 0;
        unsigned char *payload = NULL;
        size_t len = 0;
        if (ws_read_frame(ws, &opcode, &payload, &len) < 0) {
            log_line("ERR", "read frame while waiting hello failed");
            return -1;
        }
        if (opcode == 1) {
            log_line("INFO", "text: %s", payload);
            log_chat_message_from_json((const char *)payload);
            if (strstr((const char *)payload, "\"type\"") && strstr((const char *)payload, "\"hello\"")) {
                json_get_string((const char *)payload, "session_id", session_id, session_len);
                *server_rate = json_get_int_after((const char *)payload, "sample_rate", 24000);
                log_line("INFO", "server hello ok: session=%s server_rate=%d", session_id[0] ? session_id : "(empty)", *server_rate);
                free(payload);
                return 0;
            }
        } else if (opcode == 8) {
            log_line("ERR", "server closed connection during hello");
            free(payload);
            return -1;
        } else if (opcode == 9) {
            ws_send_frame(ws, 10, payload, len);
        }
        free(payload);
    }
    log_line("ERR", "timeout waiting server hello");
    return -1;
}

static int command_probe(const app_opts_t *opts)
{
    ws_conn_t ws;
    char session_id[128] = "";
    int server_rate = 24000;
    int rc = connect_and_hello(opts, &ws, session_id, sizeof(session_id), &server_rate);
    ws_close(&ws);
    if (rc == 0) {
        log_line("INFO", "probe success");
        return 0;
    }
    log_line("ERR", "probe failed");
    return 1;
}

static void build_ota_system_info(const app_opts_t *opts, char *buf,
                                  size_t len)
{
    snprintf(buf, len,
             "{"
             "\"version\":2,"
             "\"mac_address\":\"%s\","
             "\"uuid\":\"%s\","
             "\"chip_model_name\":\"k230\","
             "\"application\":{"
             "\"name\":\"k230_phone_ui\","
             "\"version\":\"%s\","
             "\"idf_version\":\"linux\""
             "},"
             "\"board\":{"
             "\"name\":\"T-Display-K230\","
             "\"type\":\"k230-linux\""
             "}"
             "}",
             opts->device_id, opts->client_id, APP_VERSION);
}

static void build_activate_url(const char *ota_url, char *buf, size_t len)
{
    size_t url_len = strlen(ota_url);
    if (url_len > 0 && ota_url[url_len - 1] == '/') {
        snprintf(buf, len, "%sactivate", ota_url);
    } else {
        snprintf(buf, len, "%s/activate", ota_url);
    }
}

static int command_activate(const app_opts_t *opts)
{
    char body[1024];
    char activate_url[768];
    char last_code[64] = "";
    time_t deadline = time(NULL) + opts->activate_timeout;

    build_ota_system_info(opts, body, sizeof(body));
    build_activate_url(opts->ota_url, activate_url, sizeof(activate_url));

    log_line("INFO", "OTA activation check: %s", opts->ota_url);
    while (!g_stop && time(NULL) <= deadline) {
        http_response_t check_resp;
        ota_config_t cfg;

        if (http_request_json(opts, "POST", opts->ota_url, body,
                              &check_resp) != 0) {
            log_line("ERR", "OTA check request failed");
            sleep(3);
            continue;
        }
        if (check_resp.status != 200) {
            log_line("ERR", "OTA check status=%d body=%s",
                     check_resp.status, check_resp.body ? check_resp.body : "");
            http_response_free(&check_resp);
            sleep(3);
            continue;
        }

        ota_parse_config(check_resp.body ? check_resp.body : "", &cfg);
        if (cfg.websocket_url[0] && cfg.websocket_token[0]) {
            log_line("INFO", "ACTIVATION_DONE websocket config ready");
            ota_print_config(&cfg);
            http_response_free(&check_resp);
            return 0;
        }

        if (cfg.activation_code[0] &&
            strcmp(cfg.activation_code, last_code) != 0) {
            snprintf(last_code, sizeof(last_code), "%s",
                     cfg.activation_code);
            log_line("INFO", "ACTIVATION_CODE: %s", cfg.activation_code);
            if (cfg.activation_message[0]) {
                log_line("INFO", "ACTIVATION_MESSAGE: %s",
                         cfg.activation_message);
            }
            log_line("INFO",
                     "Open xiaozhi.me console, add device, and enter the activation code");
        }

        if (!cfg.activation_challenge[0]) {
            log_line("WARN", "OTA response has no websocket config or activation challenge");
            http_response_free(&check_resp);
            sleep(3);
            continue;
        }
        http_response_free(&check_resp);

        http_response_t act_resp;
        if (http_request_json(opts, "POST", activate_url, "{}",
                              &act_resp) != 0) {
            log_line("WARN", "activation poll request failed");
            sleep(3);
            continue;
        }
        if (act_resp.status == 200) {
            log_line("INFO", "activation accepted, waiting websocket config");
            http_response_free(&act_resp);
            sleep(1);
            continue;
        }
        if (act_resp.status == 202) {
            log_line("INFO", "waiting for user binding in console");
            http_response_free(&act_resp);
            sleep(3);
            continue;
        }

        log_line("WARN", "activation status=%d body=%s", act_resp.status,
                 act_resp.body ? act_resp.body : "");
        http_response_free(&act_resp);
        sleep(5);
    }

    log_line("ERR", "ACTIVATION_TIMEOUT");
    return 1;
}

static int command_ptt(const app_opts_t *opts)
{
    ws_conn_t ws;
    char session_id[128] = "";
    int server_rate = 24000;
    if (connect_and_hello(opts, &ws, session_id, sizeof(session_id), &server_rate) < 0) {
        ws_close(&ws);
        return 1;
    }

    char listen[256];
    make_listen(listen, sizeof(listen), session_id, "start");
    log_line("INFO", "send listen start");
    ws_send_frame(&ws, 1, listen, strlen(listen));

    pcm_handle_t cap;
    if (open_pcm(&cap, SND_PCM_STREAM_CAPTURE, opts->capture_dev, DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS) < 0) {
        ws_close(&ws);
        return 1;
    }

    int err = 0;
    OpusEncoder *enc = opus_encoder_create(DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS, OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        log_line("ERR", "opus_encoder_create: %s", opus_strerror(err));
        close_pcm(&cap);
        ws_close(&ws);
        return 1;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(30000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));

    int16_t pcm[OPUS_FRAME_SAMPLES];
    unsigned char packet[OPUS_MAX_PACKET];
    int frames = (opts->seconds * DEFAULT_SAMPLE_RATE + OPUS_FRAME_SAMPLES - 1) / OPUS_FRAME_SAMPLES;
    log_line("INFO", "record and send: seconds=%d frames=%d", opts->seconds, frames);
    for (int i = 0; i < frames && !g_stop && !g_record_stop; ++i) {
        if (pcm_read_frames(&cap, pcm, OPUS_FRAME_SAMPLES) < 0) break;
        int nb = opus_encode(enc, pcm, OPUS_FRAME_SAMPLES, packet, sizeof(packet));
        if (nb < 0) {
            log_line("ERR", "opus_encode: %s", opus_strerror(nb));
            break;
        }
        if (ws_send_frame(&ws, 2, packet, (size_t)nb) < 0) {
            log_line("ERR", "send opus frame failed");
            break;
        }
        if ((i % 10) == 0) log_line("INFO", "tx frame=%d opus_bytes=%d", i, nb);
    }
    opus_encoder_destroy(enc);
    close_pcm(&cap);

    make_listen(listen, sizeof(listen), session_id, "stop");
    log_line("INFO", "send listen stop");
    ws_send_frame(&ws, 1, listen, strlen(listen));
    log_line("EVENT", "request_sent");

    pcm_handle_t play;
    int playback_open = (open_pcm(&play, SND_PCM_STREAM_PLAYBACK, opts->playback_dev, (unsigned int)server_rate, DEFAULT_CHANNELS) == 0);
    OpusDecoder *dec = opus_decoder_create(server_rate, DEFAULT_CHANNELS, &err);
    if (!dec || err != OPUS_OK) {
        log_line("ERR", "opus_decoder_create(%d): %s", server_rate, opus_strerror(err));
        playback_open = 0;
    }

    int16_t decoded[5760];
    time_t deadline = time(NULL) + opts->wait_seconds;
    int rx_audio_seen = 0;
    log_line("INFO", "receive response: wait=%d", opts->wait_seconds);
    while (!g_stop && time(NULL) <= deadline) {
        int opcode = 0;
        unsigned char *payload = NULL;
        size_t len = 0;
        if (ws_read_frame(&ws, &opcode, &payload, &len) < 0) {
            log_line("WARN", "read frame ended");
            break;
        }
        if (opcode == 1) {
            log_line("INFO", "text: %s", payload);
            log_chat_message_from_json((const char *)payload);
            if (json_has_type_state((const char *)payload, "tts", "start")) {
                log_line("EVENT", "response_start");
            } else if (json_has_type_state((const char *)payload, "tts",
                                           "stop")) {
                log_line("EVENT", "response_done");
                free(payload);
                break;
            }
        } else if (opcode == 2) {
            log_line("INFO", "rx opus bytes=%zu", len);
            if (!rx_audio_seen) {
                rx_audio_seen = 1;
                log_line("EVENT", "audio_start");
            }
            if (dec && playback_open) {
                int samples = opus_decode(dec, payload, (opus_int32)len, decoded, 5760, 0);
                if (samples > 0) {
                    pcm_write_frames(&play, decoded, (snd_pcm_uframes_t)samples);
                } else {
                    log_line("ERR", "opus_decode rx: %s", opus_strerror(samples));
                }
            }
        } else if (opcode == 8) {
            log_line("INFO", "server close");
            free(payload);
            break;
        } else if (opcode == 9) {
            ws_send_frame(&ws, 10, payload, len);
        }
        free(payload);
    }

    if (dec) opus_decoder_destroy(dec);
    if (playback_open) close_pcm(&play);
    ws_close(&ws);
    log_line("INFO", "ptt done");
    return 0;
}

static void pcm_level_stats(const int16_t *pcm, int samples, int *mean_abs,
                            int *peak_abs)
{
    long long sum = 0;
    int peak = 0;

    if (!pcm || samples <= 0) {
        if (mean_abs) *mean_abs = 0;
        if (peak_abs) *peak_abs = 0;
        return;
    }

    for (int i = 0; i < samples; ++i) {
        int v = pcm[i];
        int a = v < 0 ? -v : v;
        sum += a;
        if (a > peak) {
            peak = a;
        }
    }

    if (mean_abs) *mean_abs = (int)(sum / samples);
    if (peak_abs) *peak_abs = peak;
}

static int session_control_should_stop(control_reader_t *ctl)
{
    char cmd[64];

    while (ctl && control_pop_line(ctl, cmd, sizeof(cmd)) > 0) {
        if (strcmp(cmd, "PTT_END") == 0 || strcmp(cmd, "WAKE_END") == 0) {
            log_line("INFO", "control %s", cmd);
            g_record_stop = 1;
            return 1;
        }
        if (strcmp(cmd, "QUIT") == 0) {
            log_line("INFO", "control QUIT");
            g_stop = 1;
            return 1;
        }
    }

    return 0;
}

static int send_listen_state(ws_conn_t *ws, const char *session_id,
                             const char *state)
{
    char listen[256];

    make_listen(listen, sizeof(listen), session_id, state);
    log_line("INFO", "send listen %s", state);
    if (ws_send_frame(ws, 1, listen, strlen(listen)) < 0) {
        log_line("ERR", "send listen %s failed", state);
        return -1;
    }

    return 0;
}

static int session_ptt_turn(const app_opts_t *opts, ws_conn_t *ws,
                            const char *session_id, int server_rate,
                            control_reader_t *ctl,
                            session_record_mode_t record_mode)
{
    pcm_handle_t cap;
    int err = 0;
    OpusEncoder *enc = NULL;
    pcm_handle_t play;
    int playback_open = 0;
    OpusDecoder *dec = NULL;
    int rc = 0;
    int listen_started = 0;
    int wake_no_speech_logged = 0;

    g_record_stop = 0;
    if (record_mode == SESSION_RECORD_MANUAL) {
        if (send_listen_state(ws, session_id, "start") < 0) {
            return 1;
        }
        listen_started = 1;
    } else {
        log_line("INFO", "wake VAD armed: preroll=%d idle_timeout_frames=%d",
                 WAKE_VAD_PREROLL_FRAMES, WAKE_VAD_IDLE_TIMEOUT_FRAMES);
    }

    if (open_pcm(&cap, SND_PCM_STREAM_CAPTURE, opts->capture_dev,
                 DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS) < 0) {
        return 1;
    }

    enc = opus_encoder_create(DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS,
                              OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        log_line("ERR", "opus_encoder_create: %s", opus_strerror(err));
        close_pcm(&cap);
        return 1;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(30000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));

    int16_t pcm[OPUS_FRAME_SAMPLES];
    unsigned char packet[OPUS_MAX_PACKET];
    int frames = (opts->seconds * DEFAULT_SAMPLE_RATE + OPUS_FRAME_SAMPLES - 1) / OPUS_FRAME_SAMPLES;
    int sent_frames = 0;
    int active_frames = 0;
    int speech_frames = 0;
    int start_candidate_frames = 0;
    int silence_frames = 0;
    opus_preroll_frame_t preroll[WAKE_VAD_PREROLL_FRAMES];
    int preroll_count = 0;
    int preroll_next = 0;
    log_line("INFO", "record and send: seconds=%d frames=%d", opts->seconds, frames);
    for (int i = 0; i < frames && !g_stop && !g_record_stop; ++i) {
        int mean_abs = 0;
        int peak_abs = 0;
        int voice_now = 0;
        int silence_now = 0;

        session_control_should_stop(ctl);
        if (g_stop || g_record_stop) break;

        if (pcm_read_frames(&cap, pcm, OPUS_FRAME_SAMPLES) < 0) {
            rc = 1;
            break;
        }
        pcm_level_stats(pcm, OPUS_FRAME_SAMPLES, &mean_abs, &peak_abs);
        int nb = opus_encode(enc, pcm, OPUS_FRAME_SAMPLES, packet, sizeof(packet));
        if (nb < 0) {
            log_line("ERR", "opus_encode: %s", opus_strerror(nb));
            rc = 1;
            break;
        }

        if (record_mode == SESSION_RECORD_WAKE_VAD && !listen_started) {
            memcpy(preroll[preroll_next].data, packet, (size_t)nb);
            preroll[preroll_next].len = nb;
            preroll_next = (preroll_next + 1) % WAKE_VAD_PREROLL_FRAMES;
            if (preroll_count < WAKE_VAD_PREROLL_FRAMES) {
                preroll_count++;
            }

            if (i < WAKE_VAD_WARMUP_FRAMES) {
                if ((i % 2) == 0) {
                    log_line("INFO",
                             "wake VAD warmup frame=%d mean=%d peak=%d",
                             i, mean_abs, peak_abs);
                }
                continue;
            }

            voice_now = mean_abs >= WAKE_VAD_START_MEAN ||
                        peak_abs >= WAKE_VAD_START_PEAK;
            if (!voice_now) {
                start_candidate_frames = 0;
                if ((i % 10) == 0) {
                    log_line("INFO", "wake VAD wait frame=%d mean=%d peak=%d",
                             i, mean_abs, peak_abs);
                }
                if (i >= WAKE_VAD_IDLE_TIMEOUT_FRAMES) {
                    log_line("EVENT", "wake_no_speech");
                    wake_no_speech_logged = 1;
                    log_line("INFO", "wake VAD no speech: frames=%d", i);
                    break;
                }
                continue;
            }
            start_candidate_frames++;
            if (start_candidate_frames < WAKE_VAD_START_FRAMES) {
                log_line("INFO",
                         "wake VAD candidate frame=%d/%d mean=%d peak=%d",
                         start_candidate_frames, WAKE_VAD_START_FRAMES,
                         mean_abs, peak_abs);
                continue;
            }

            if (send_listen_state(ws, session_id, "start") < 0) {
                rc = 1;
                break;
            }
            listen_started = 1;
            log_line("EVENT", "wake_speech_start");
            log_line("INFO", "wake VAD speech_start frame=%d mean=%d peak=%d",
                     i, mean_abs, peak_abs);

            int first = (preroll_next + WAKE_VAD_PREROLL_FRAMES -
                         preroll_count) % WAKE_VAD_PREROLL_FRAMES;
            for (int p = 0; p < preroll_count; ++p) {
                int idx = (first + p) % WAKE_VAD_PREROLL_FRAMES;
                if (preroll[idx].len <= 0) {
                    continue;
                }
                if (ws_send_frame(ws, 2, preroll[idx].data,
                                  (size_t)preroll[idx].len) < 0) {
                    log_line("ERR", "send wake preroll frame failed");
                    rc = 1;
                    break;
                }
                sent_frames++;
            }
            if (rc != 0) {
                break;
            }
            active_frames = 1;
            speech_frames = 1;
            silence_frames = 0;
            continue;
        }

        if (ws_send_frame(ws, 2, packet, (size_t)nb) < 0) {
            log_line("ERR", "send opus frame failed");
            rc = 1;
            break;
        }
        sent_frames++;
        if ((i % 10) == 0) {
            log_line("INFO", "tx frame=%d opus_bytes=%d level=%d/%d", i, nb,
                     mean_abs, peak_abs);
        }

        if (record_mode == SESSION_RECORD_WAKE_VAD) {
            active_frames++;
            voice_now = mean_abs >= WAKE_VAD_START_MEAN ||
                        peak_abs >= WAKE_VAD_START_PEAK;
            silence_now = mean_abs <= WAKE_VAD_STOP_MEAN;
            if (voice_now) {
                speech_frames++;
                silence_frames = 0;
            } else if (silence_now) {
                silence_frames++;
            } else {
                silence_frames = 0;
            }
            if (active_frames >= WAKE_VAD_MAX_ACTIVE_FRAMES) {
                log_line("INFO",
                         "wake VAD max active reached frame=%d active=%d sent=%d",
                         i, active_frames, sent_frames);
                break;
            }
            if (active_frames >= WAKE_VAD_MIN_ACTIVE_FRAMES &&
                silence_frames >= WAKE_VAD_SILENCE_FRAMES) {
                log_line("INFO",
                         "wake VAD speech_end frame=%d active=%d speech=%d silence=%d sent=%d",
                         i, active_frames, speech_frames, silence_frames,
                         sent_frames);
                break;
            }
        }
    }
    opus_encoder_destroy(enc);
    close_pcm(&cap);

    if (!listen_started) {
        if (rc == 0 && !g_stop && !wake_no_speech_logged) {
            log_line("EVENT", "wake_no_speech");
        }
        return rc;
    }

    if (send_listen_state(ws, session_id, "stop") < 0) {
        return 1;
    }
    log_line("EVENT", "request_sent");
    log_line("INFO", "record sent_frames=%d", sent_frames);
    if (rc != 0 || g_stop) {
        return rc ? rc : 1;
    }

    playback_open = (open_pcm(&play, SND_PCM_STREAM_PLAYBACK,
                              opts->playback_dev, (unsigned int)server_rate,
                              DEFAULT_CHANNELS) == 0);
    dec = opus_decoder_create(server_rate, DEFAULT_CHANNELS, &err);
    if (!dec || err != OPUS_OK) {
        log_line("ERR", "opus_decoder_create(%d): %s", server_rate,
                 opus_strerror(err));
        playback_open = 0;
    }

    int16_t decoded[5760];
    time_t deadline = time(NULL) + opts->wait_seconds;
    int rx_audio_seen = 0;
    log_line("INFO", "receive response: wait=%d", opts->wait_seconds);
    while (!g_stop && time(NULL) <= deadline) {
        int opcode = 0;
        unsigned char *payload = NULL;
        size_t len = 0;
        char cmd[64];

        while (ctl && control_pop_line(ctl, cmd, sizeof(cmd)) > 0) {
            if (strcmp(cmd, "QUIT") == 0) {
                log_line("INFO", "control QUIT");
                g_stop = 1;
                break;
            }
        }
        if (g_stop) break;

        if (ws_read_frame(ws, &opcode, &payload, &len) < 0) {
            log_line("WARN", "read frame ended");
            break;
        }
        if (opcode == 1) {
            log_line("INFO", "text: %s", payload);
            log_chat_message_from_json((const char *)payload);
            if (json_has_type_state((const char *)payload, "tts", "start")) {
                log_line("EVENT", "response_start");
            } else if (json_has_type_state((const char *)payload, "tts",
                                           "stop")) {
                log_line("EVENT", "response_done");
                free(payload);
                break;
            }
        } else if (opcode == 2) {
            log_line("INFO", "rx opus bytes=%zu", len);
            if (!rx_audio_seen) {
                rx_audio_seen = 1;
                log_line("EVENT", "audio_start");
            }
            if (dec && playback_open) {
                int samples = opus_decode(dec, payload, (opus_int32)len,
                                          decoded, 5760, 0);
                if (samples > 0) {
                    pcm_write_frames(&play, decoded,
                                     (snd_pcm_uframes_t)samples);
                } else {
                    log_line("ERR", "opus_decode rx: %s",
                             opus_strerror(samples));
                }
            }
        } else if (opcode == 8) {
            log_line("INFO", "server close");
            free(payload);
            rc = 2;
            break;
        } else if (opcode == 9) {
            ws_send_frame(ws, 10, payload, len);
        }
        free(payload);
    }

    if (dec) opus_decoder_destroy(dec);
    if (playback_open) close_pcm(&play);
    return rc;
}

static int command_session(const app_opts_t *opts)
{
    control_reader_t ctl;
    ws_conn_t ws;
    char session_id[128] = "";
    int server_rate = 24000;
    int connected = 0;
    int rc = 0;

    if (control_open(&ctl, opts->control_path) < 0) {
        return 1;
    }

    memset(&ws, 0, sizeof(ws));
    ws.fd = -1;
    if (connect_and_hello(opts, &ws, session_id, sizeof(session_id),
                          &server_rate) == 0) {
        connected = 1;
        log_line("INFO", "session ready: session=%s server_rate=%d",
                 session_id[0] ? session_id : "(empty)", server_rate);
    } else {
        log_line("ERR", "session connect failed");
        control_close(&ctl);
        ws_close(&ws);
        return 1;
    }

    while (!g_stop) {
        char cmd[64];
        int got = control_wait_line(&ctl, cmd, sizeof(cmd), 1000);
        if (got < 0) {
            rc = 1;
            break;
        }
        if (got == 0) {
            continue;
        }

        if (strcmp(cmd, "QUIT") == 0) {
            log_line("INFO", "session quit");
            break;
        }
        if (strcmp(cmd, "PTT_BEGIN") != 0 && strcmp(cmd, "WAKE_BEGIN") != 0) {
            log_line("WARN", "unknown control command: %s", cmd);
            continue;
        }

        if (!connected) {
            if (connect_and_hello(opts, &ws, session_id, sizeof(session_id),
                                  &server_rate) != 0) {
                log_line("ERR", "session reconnect failed");
                ws_close(&ws);
                rc = 1;
                continue;
            }
            connected = 1;
            log_line("INFO", "session ready: session=%s server_rate=%d",
                     session_id[0] ? session_id : "(empty)", server_rate);
        }

        session_record_mode_t record_mode =
            strcmp(cmd, "WAKE_BEGIN") == 0 ? SESSION_RECORD_WAKE_VAD :
                                             SESSION_RECORD_MANUAL;
        log_line("INFO", "%s turn start",
                 record_mode == SESSION_RECORD_WAKE_VAD ? "wake" : "ptt");
        int turn_rc = session_ptt_turn(opts, &ws, session_id, server_rate,
                                       &ctl, record_mode);
        log_line(turn_rc == 0 ? "INFO" : "ERR", "%s turn done rc=%d",
                 record_mode == SESSION_RECORD_WAKE_VAD ? "wake" : "ptt",
                 turn_rc);
        if (turn_rc != 0) {
            ws_close(&ws);
            connected = 0;
        }
    }

    if (connected) {
        ws_close(&ws);
    }
    control_close(&ctl);
    log_line("INFO", "session ended rc=%d", rc);
    return rc;
}

static void print_usage(FILE *out)
{
    fprintf(out,
            "xiaozhi_k230_lab %s\n"
            "Usage:\n"
            "  xiaozhi_k230_lab activate [options]\n"
            "  xiaozhi_k230_lab probe [options]\n"
            "  xiaozhi_k230_lab audio-loopback [options]\n"
            "  xiaozhi_k230_lab ptt [options]\n"
            "  xiaozhi_k230_lab session --control FIFO [options]\n"
            "\n"
            "Options:\n"
            "  --url URL              WebSocket URL, default: %s or XIAOZHI_URL\n"
            "  --ota-url URL          OTA activation URL, default: %s or XIAOZHI_OTA_URL\n"
            "  --token TOKEN          Bearer token, default: XIAOZHI_TOKEN\n"
            "  --control FIFO         control FIFO for session mode\n"
            "  --device-id ID         Device-Id header, default: first MAC address\n"
            "  --client-id ID         Client-Id header, default: /etc/machine-id or random UUID\n"
            "  --capture DEV          ALSA capture device, default: default\n"
            "  --playback DEV         ALSA playback device, default: default\n"
            "  --seconds N            record/loopback seconds, default: 3\n"
            "  --wait N               ptt response wait seconds, default: 12\n"
            "  --timeout-ms N         socket/hello timeout, default: 10000\n"
            "  --activate-timeout N   activation wait seconds, default: 120\n"
            "  --tls-verify           enable TLS certificate verification\n"
            "  -v, --verbose          verbose log\n"
            "  --version              print version\n"
            "  -h, --help             show help\n",
            APP_VERSION, DEFAULT_URL, DEFAULT_OTA_URL);
}

static int parse_args(int argc, char **argv, app_opts_t *opts)
{
    static char device_id_buf[128];
    static char client_id_buf[128];
    default_device_id(device_id_buf, sizeof(device_id_buf));
    default_client_id(client_id_buf, sizeof(client_id_buf));

    memset(opts, 0, sizeof(*opts));
    opts->url = env_or_default("XIAOZHI_URL", DEFAULT_URL);
    opts->ota_url = env_or_default("XIAOZHI_OTA_URL", DEFAULT_OTA_URL);
    opts->token = env_or_default("XIAOZHI_TOKEN", "");
    opts->device_id = env_or_default("XIAOZHI_DEVICE_ID", device_id_buf);
    opts->client_id = env_or_default("XIAOZHI_CLIENT_ID", client_id_buf);
    opts->capture_dev = env_or_default("XIAOZHI_CAPTURE", DEFAULT_CAPTURE_DEV);
    opts->playback_dev = env_or_default("XIAOZHI_PLAYBACK", DEFAULT_PLAYBACK_DEV);
    opts->seconds = 3;
    opts->wait_seconds = 12;
    opts->timeout_ms = 10000;
    opts->activate_timeout = 120;
    opts->tls_verify = 0;

    if (argc < 2) {
        print_usage(stderr);
        return -1;
    }
    opts->mode = argv[1];
    if (strcmp(opts->mode, "--help") == 0 || strcmp(opts->mode, "-h") == 0) {
        print_usage(stdout);
        exit(0);
    }
    if (strcmp(opts->mode, "--version") == 0) {
        printf("xiaozhi_k230_lab %s\n", APP_VERSION);
        exit(0);
    }

    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        const char **target = NULL;
        if (strcmp(a, "--url") == 0) target = &opts->url;
        else if (strcmp(a, "--ota-url") == 0) target = &opts->ota_url;
        else if (strcmp(a, "--token") == 0) target = &opts->token;
        else if (strcmp(a, "--device-id") == 0) target = &opts->device_id;
        else if (strcmp(a, "--client-id") == 0) target = &opts->client_id;
        else if (strcmp(a, "--capture") == 0) target = &opts->capture_dev;
        else if (strcmp(a, "--playback") == 0) target = &opts->playback_dev;
        else if (strcmp(a, "--control") == 0) target = &opts->control_path;
        else if (strcmp(a, "--seconds") == 0) {
            if (++i >= argc) return -1;
            opts->seconds = atoi(argv[i]);
            continue;
        } else if (strcmp(a, "--wait") == 0) {
            if (++i >= argc) return -1;
            opts->wait_seconds = atoi(argv[i]);
            continue;
        } else if (strcmp(a, "--timeout-ms") == 0) {
            if (++i >= argc) return -1;
            opts->timeout_ms = atoi(argv[i]);
            continue;
        } else if (strcmp(a, "--activate-timeout") == 0) {
            if (++i >= argc) return -1;
            opts->activate_timeout = atoi(argv[i]);
            continue;
        } else if (strcmp(a, "--tls-verify") == 0) {
            opts->tls_verify = 1;
            continue;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            opts->verbose = 1;
            continue;
        } else {
            log_line("ERR", "unknown option: %s", a);
            return -1;
        }
        if (target) {
            if (++i >= argc) return -1;
            *target = argv[i];
        }
    }

    if (opts->seconds <= 0) opts->seconds = 1;
    if (opts->wait_seconds <= 0) opts->wait_seconds = 1;
    if (opts->timeout_ms < 1000) opts->timeout_ms = 1000;
    if (opts->activate_timeout < 15) opts->activate_timeout = 15;
    return 0;
}

int main(int argc, char **argv)
{
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_record_stop_signal);

    app_opts_t opts;
    if (parse_args(argc, argv, &opts) < 0) {
        return 2;
    }

    log_line("INFO", "mode=%s url=%s ota_url=%s device_id=%s client_id=%s",
             opts.mode, opts.url, opts.ota_url, opts.device_id,
             opts.client_id);
    if (strcmp(opts.mode, "activate") != 0 && (!opts.token || !opts.token[0])) {
        log_line("WARN", "XIAOZHI_TOKEN/--token is empty; server may reject the handshake");
    }

    if (strcmp(opts.mode, "activate") == 0) {
        return command_activate(&opts);
    }
    if (strcmp(opts.mode, "probe") == 0) {
        return command_probe(&opts);
    }
    if (strcmp(opts.mode, "audio-loopback") == 0) {
        return command_audio_loopback(&opts);
    }
    if (strcmp(opts.mode, "ptt") == 0) {
        return command_ptt(&opts);
    }
    if (strcmp(opts.mode, "session") == 0) {
        return command_session(&opts);
    }

    log_line("ERR", "unknown mode: %s", opts.mode);
    print_usage(stderr);
    return 2;
}
