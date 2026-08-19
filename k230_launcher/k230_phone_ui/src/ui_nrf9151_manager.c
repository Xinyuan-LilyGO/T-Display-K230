#include "ui_nrf9151_manager.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t nrf9151_manager_monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static void nrf9151_manager_trim_text(char *text)
{
    char *start;
    size_t len;

    if(!text) {
        return;
    }
    start = text;
    while(*start && isspace((unsigned char)*start)) {
        start++;
    }
    if(start != text) {
        memmove(text, start, strlen(start) + 1U);
    }

    len = strlen(text);
    while(len > 0 && isspace((unsigned char)text[len - 1U])) {
        text[--len] = '\0';
    }
}

const char *k230_nrf9151_uart_dev(void)
{
    return K230_NRF9151_UART_DEV;
}

const char *k230_nrf9151_gnss_cache_path(void)
{
    return K230_NRF9151_GNSS_FIX_CACHE;
}

const char *k230_nrf9151_gnss_cache_tmp_path(void)
{
    return K230_NRF9151_GNSS_FIX_CACHE_TMP;
}

int k230_nrf9151_uart_present(void)
{
    return access(K230_NRF9151_UART_DEV, R_OK | W_OK) == 0;
}

int k230_nrf9151_write_gnss_fix(double latitude, double longitude,
                                int has_altitude, double altitude_m,
                                int satellites, const char *source)
{
    FILE *fp;

    if(!isfinite(latitude) || !isfinite(longitude) ||
       latitude < -90.0 || latitude > 90.0 ||
       longitude < -180.0 || longitude > 180.0 ||
       (fabs(latitude) < 0.000001 && fabs(longitude) < 0.000001)) {
        errno = EINVAL;
        return -1;
    }

    fp = fopen(K230_NRF9151_GNSS_FIX_CACHE_TMP, "w");
    if(!fp) {
        return -1;
    }
    fprintf(fp, "version=1\n");
    fprintf(fp, "epoch=%ld\n", (long)time(NULL));
    fprintf(fp, "lat=%.7f\n", latitude);
    fprintf(fp, "lon=%.7f\n", longitude);
    fprintf(fp, "has_alt=%d\n", has_altitude ? 1 : 0);
    fprintf(fp, "alt=%.2f\n", has_altitude ? altitude_m : 0.0);
    fprintf(fp, "sats=%d\n", satellites > 0 ? satellites : 0);
    fprintf(fp, "source=%s\n", source && source[0] ? source : "nrf9151");
    if(fclose(fp) != 0) {
        int saved_errno = errno;

        unlink(K230_NRF9151_GNSS_FIX_CACHE_TMP);
        errno = saved_errno;
        return -1;
    }
    if(rename(K230_NRF9151_GNSS_FIX_CACHE_TMP,
              K230_NRF9151_GNSS_FIX_CACHE) != 0) {
        int saved_errno = errno;

        unlink(K230_NRF9151_GNSS_FIX_CACHE_TMP);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int k230_nrf9151_read_gnss_fix(k230_nrf9151_gnss_fix_t *fix,
                               int max_age_seconds)
{
    struct stat st;
    FILE *fp;
    char line[192];
    double lat = 0.0;
    double lon = 0.0;
    double alt = 0.0;
    int has_alt = 0;
    int sats = 0;
    time_t epoch = 0;
    time_t now;
    char source[48] = "cache";

    if(!fix) {
        return -1;
    }
    memset(fix, 0, sizeof(*fix));
    if(stat(K230_NRF9151_GNSS_FIX_CACHE, &st) != 0) {
        return -1;
    }

    fp = fopen(K230_NRF9151_GNSS_FIX_CACHE, "r");
    if(!fp) {
        return -1;
    }
    while(fgets(line, sizeof(line), fp)) {
        char *eq;

        nrf9151_manager_trim_text(line);
        eq = strchr(line, '=');
        if(!eq) {
            continue;
        }
        *eq++ = '\0';
        if(strcmp(line, "epoch") == 0) {
            epoch = (time_t)strtol(eq, NULL, 10);
        } else if(strcmp(line, "lat") == 0) {
            lat = strtod(eq, NULL);
        } else if(strcmp(line, "lon") == 0) {
            lon = strtod(eq, NULL);
        } else if(strcmp(line, "has_alt") == 0) {
            has_alt = atoi(eq) != 0;
        } else if(strcmp(line, "alt") == 0) {
            alt = strtod(eq, NULL);
        } else if(strcmp(line, "sats") == 0) {
            sats = atoi(eq);
        } else if(strcmp(line, "source") == 0) {
            snprintf(source, sizeof(source), "%s", eq);
        }
    }
    fclose(fp);

    if(epoch <= 0) {
        epoch = st.st_mtime;
    }
    now = time(NULL);
    if(now > 0 && epoch > 0 && now >= epoch) {
        fix->age_seconds = (long)(now - epoch);
    } else {
        fix->age_seconds = 0;
    }
    if(max_age_seconds > 0 && fix->age_seconds > max_age_seconds) {
        return -1;
    }
    if(!isfinite(lat) || !isfinite(lon) || lat < -90.0 || lat > 90.0 ||
       lon < -180.0 || lon > 180.0 ||
       (fabs(lat) < 0.000001 && fabs(lon) < 0.000001)) {
        return -1;
    }

    fix->valid = 1;
    fix->latitude = lat;
    fix->longitude = lon;
    fix->altitude_m = alt;
    fix->has_altitude = has_alt;
    fix->satellites = sats;
    fix->epoch = epoch;
    snprintf(fix->source, sizeof(fix->source), "%s", source);
    return 0;
}

int k230_nrf9151_acquire_uart(const char *owner, int wait_ms)
{
    int fd;
    uint64_t start_us;
    int waited_ms = 0;

    fd = open(K230_NRF9151_UART_LOCK, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if(fd < 0) {
        return -1;
    }
    start_us = nrf9151_manager_monotonic_us();
    for(;;) {
        if(flock(fd, LOCK_EX | LOCK_NB) == 0) {
            if(ftruncate(fd, 0) != 0) {
                flock(fd, LOCK_UN);
                close(fd);
                return -1;
            }
            dprintf(fd, "%s\n", owner && owner[0] ? owner : "unknown");
            return fd;
        }
        if(errno != EWOULDBLOCK && errno != EAGAIN) {
            close(fd);
            return -1;
        }
        if(wait_ms <= 0 || waited_ms >= wait_ms) {
            close(fd);
            errno = EBUSY;
            return -1;
        }
        usleep(20000);
        waited_ms = (int)((nrf9151_manager_monotonic_us() - start_us) /
                          1000ULL);
    }
}

void k230_nrf9151_release_uart(int lock_fd)
{
    if(lock_fd >= 0) {
        (void)flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}
