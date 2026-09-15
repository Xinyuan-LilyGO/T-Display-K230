// SPDX-License-Identifier: MIT
/*
 * Read-only camera and VCM probe for the T-Display K230.
 *
 * The probe never writes to the camera or lens controller. A responding
 * address only proves that an I2C device is present; it does not identify
 * the VCM model or prove that a focus command is safe.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_BUS 0
#define DEFAULT_CAMERA_ADDR 0x37
#define DEFAULT_VCM_ADDR 0x0c
#define DEFAULT_INTERVAL_MS 1000
#define GC2093_CHIP_ID_H 0x03f0
#define GC2093_CHIP_ID_L 0x03f1

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void usage(const char *argv0)
{
    printf("Usage: %s [options]\n", argv0);
    printf("\n");
    printf("Read-only probe for the GC2093 camera and its optional VCM.\n");
    printf("Options:\n");
    printf("  -b, --bus N             I2C bus, default %d\n", DEFAULT_BUS);
    printf("  -c, --camera-addr HEX   Camera address, default 0x%02x\n",
           DEFAULT_CAMERA_ADDR);
    printf("  -a, --vcm-addr HEX      VCM address, default 0x%02x\n",
           DEFAULT_VCM_ADDR);
    printf("  -w, --watch             Repeat until interrupted\n");
    printf("  -i, --interval-ms N     Watch interval, default %d ms\n",
           DEFAULT_INTERVAL_MS);
    printf("  -h, --help              Show this help\n");
}

static int parse_number(const char *text, int min_value, int max_value,
                        int *value)
{
    char *end = NULL;
    long parsed;

    if(!text || !value) {
        return -EINVAL;
    }

    errno = 0;
    parsed = strtol(text, &end, 0);
    if(errno != 0 || end == text || *end != '\0' ||
       parsed < min_value || parsed > max_value) {
        return -EINVAL;
    }

    *value = (int)parsed;
    return 0;
}

static int open_bus(int bus)
{
    char path[32];
    int fd;

    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    fd = open(path, O_RDWR);
    if(fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
    }
    return fd;
}

static int select_address(int fd, int address)
{
    if(ioctl(fd, I2C_SLAVE, address) < 0) {
        return -errno;
    }
    return 0;
}

static int read_receive_byte(int fd, uint8_t *value)
{
    union i2c_smbus_data data;
    struct i2c_smbus_ioctl_data args;
    ssize_t read_count;

    if(!value) {
        return -EINVAL;
    }

    memset(&data, 0, sizeof(data));
    memset(&args, 0, sizeof(args));
    args.read_write = I2C_SMBUS_READ;
    args.command = 0;
    args.size = I2C_SMBUS_BYTE;
    args.data = &data;

    if(ioctl(fd, I2C_SMBUS, &args) == 0) {
        *value = data.byte;
        return 0;
    }

    /*
     * Some K230 images expose an adapter that accepts a plain receive-byte
     * read but does not advertise the SMBus ioctl capability.
     */
    read_count = read(fd, value, 1);
    if(read_count == 1) {
        return 0;
    }
    return errno ? -errno : -EIO;
}

static int read_camera_reg8(int fd, int address, uint16_t reg, uint8_t *value)
{
    uint8_t reg_bytes[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xff),
    };
    uint8_t result = 0;
    struct i2c_msg messages[2];
    struct i2c_rdwr_ioctl_data transfer;

    if(!value) {
        return -EINVAL;
    }

    memset(messages, 0, sizeof(messages));
    messages[0].addr = address;
    messages[0].flags = 0;
    messages[0].len = sizeof(reg_bytes);
    messages[0].buf = reg_bytes;
    messages[1].addr = address;
    messages[1].flags = I2C_M_RD;
    messages[1].len = 1;
    messages[1].buf = &result;

    transfer.msgs = messages;
    transfer.nmsgs = 2;
    if(ioctl(fd, I2C_RDWR, &transfer) != 2) {
        return errno ? -errno : -EIO;
    }

    *value = result;
    return 0;
}

static void sleep_milliseconds(int milliseconds)
{
    struct timespec request;

    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while(nanosleep(&request, &request) < 0 && errno == EINTR) {
    }
}

static void print_probe_result(int fd, int bus, int camera_addr, int vcm_addr)
{
    uint8_t value = 0;
    int camera_id_h = -1;
    int camera_id_l = -1;
    int vcm_status;

    if(select_address(fd, camera_addr) == 0 &&
       read_camera_reg8(fd, camera_addr, GC2093_CHIP_ID_H, &value) == 0) {
        camera_id_h = value;
    }
    if(select_address(fd, camera_addr) == 0 &&
       read_camera_reg8(fd, camera_addr, GC2093_CHIP_ID_L, &value) == 0) {
        camera_id_l = value;
    }

    if(select_address(fd, vcm_addr) == 0) {
        vcm_status = read_receive_byte(fd, &value);
    } else {
        vcm_status = -EIO;
    }

    printf("camera: bus=%d addr=0x%02x", bus, camera_addr);
    if(camera_id_h >= 0 && camera_id_l >= 0) {
        printf(" chip_id=0x%02x%02x", camera_id_h, camera_id_l);
    } else {
        printf(" chip_id=read-failed");
    }
    printf("\n");

    printf("vcm: bus=%d addr=0x%02x", bus, vcm_addr);
    if(vcm_status == 0) {
        printf(" responding=yes rx=0x%02x model=unknown\n", value);
        printf("vcm: I2C presence confirmed; no register writes performed\n");
    } else {
        printf(" responding=no\n");
    }
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"bus", required_argument, NULL, 'b'},
        {"camera-addr", required_argument, NULL, 'c'},
        {"vcm-addr", required_argument, NULL, 'a'},
        {"watch", no_argument, NULL, 'w'},
        {"interval-ms", required_argument, NULL, 'i'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int bus = DEFAULT_BUS;
    int camera_addr = DEFAULT_CAMERA_ADDR;
    int vcm_addr = DEFAULT_VCM_ADDR;
    int interval_ms = DEFAULT_INTERVAL_MS;
    int watch = 0;
    int option;
    int fd;

    while((option = getopt_long(argc, argv, "b:c:a:wi:h", options, NULL)) != -1) {
        switch(option) {
        case 'b':
            if(parse_number(optarg, 0, 32, &bus) < 0) {
                fprintf(stderr, "invalid I2C bus: %s\n", optarg);
                return 2;
            }
            break;
        case 'c':
            if(parse_number(optarg, 0x08, 0x77, &camera_addr) < 0) {
                fprintf(stderr, "invalid camera address: %s\n", optarg);
                return 2;
            }
            break;
        case 'a':
            if(parse_number(optarg, 0x08, 0x77, &vcm_addr) < 0) {
                fprintf(stderr, "invalid VCM address: %s\n", optarg);
                return 2;
            }
            break;
        case 'w':
            watch = 1;
            break;
        case 'i':
            if(parse_number(optarg, 50, 60000, &interval_ms) < 0) {
                fprintf(stderr, "invalid interval: %s\n", optarg);
                return 2;
            }
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fd = open_bus(bus);
    if(fd < 0) {
        return 1;
    }

    do {
        print_probe_result(fd, bus, camera_addr, vcm_addr);
        if(!watch || stop_requested) {
            break;
        }
        sleep_milliseconds(interval_ms);
    } while(!stop_requested);

    close(fd);
    return 0;
}
