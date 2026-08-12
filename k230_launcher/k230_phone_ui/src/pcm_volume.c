#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PCM_VOLUME_DEFAULT_PATH "/root/.k230_phone_audio_volume"
#define PCM_VOLUME_DEFAULT_MAX 45
#define PCM_VOLUME_BUFFER_BYTES 8192

static int clamp_int(int value, int min_value, int max_value)
{
    if(value < min_value) {
        return min_value;
    }
    if(value > max_value) {
        return max_value;
    }
    return value;
}

static int read_volume(const char *path, int max_volume, int fallback)
{
    FILE *fp;
    char text[32];
    long value;

    fp = fopen(path, "r");
    if(!fp) {
        return fallback;
    }

    if(!fgets(text, sizeof(text), fp)) {
        fclose(fp);
        return fallback;
    }
    fclose(fp);

    errno = 0;
    value = strtol(text, NULL, 10);
    if(errno != 0) {
        return fallback;
    }
    return clamp_int((int)value, 0, max_volume);
}

static int write_all(int fd, const unsigned char *buf, size_t len)
{
    size_t done = 0;

    while(done < len) {
        ssize_t written = write(fd, buf + done, len - done);
        if(written < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(written == 0) {
            return -1;
        }
        done += (size_t)written;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *volume_path = argc > 1 ? argv[1] : PCM_VOLUME_DEFAULT_PATH;
    int max_volume = argc > 2 ? atoi(argv[2]) : PCM_VOLUME_DEFAULT_MAX;
    int volume;
    unsigned char buf[PCM_VOLUME_BUFFER_BYTES];
    unsigned int loop = 0;

    if(max_volume <= 0) {
        max_volume = PCM_VOLUME_DEFAULT_MAX;
    }

    volume = read_volume(volume_path, max_volume, max_volume);

    while(1) {
        ssize_t got = read(STDIN_FILENO, buf, sizeof(buf));
        if(got < 0) {
            if(errno == EINTR) {
                continue;
            }
            return 1;
        }
        if(got == 0) {
            break;
        }

        if((loop++ & 0x03U) == 0U) {
            volume = read_volume(volume_path, max_volume, volume);
        }

        for(ssize_t i = 0; i + 1 < got; i += 2) {
            int16_t sample = (int16_t)((uint16_t)buf[i] |
                                       ((uint16_t)buf[i + 1] << 8));
            int32_t scaled = ((int32_t)sample * volume) / max_volume;

            if(scaled > INT16_MAX) {
                scaled = INT16_MAX;
            } else if(scaled < INT16_MIN) {
                scaled = INT16_MIN;
            }
            buf[i] = (unsigned char)((uint16_t)scaled & 0xFFU);
            buf[i + 1] = (unsigned char)(((uint16_t)scaled >> 8) & 0xFFU);
        }

        if(write_all(STDOUT_FILENO, buf, (size_t)got) != 0) {
            return 1;
        }
    }

    return 0;
}
