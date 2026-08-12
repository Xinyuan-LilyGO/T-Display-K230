#include "ui_multitouch.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MT_BITS_PER_LONG ((int)(sizeof(unsigned long) * 8U))
#define MT_ABS_BITS_LEN ((ABS_MAX / MT_BITS_PER_LONG) + 1)

typedef struct {
    int active;
    int tracking_id;
    int raw_x;
    int raw_y;
    int x;
    int y;
    uint64_t kernel_us;
    uint64_t app_us;
} ui_touch_slot_t;

static pthread_mutex_t mt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mt_thread;
static int mt_thread_valid;
static volatile int mt_running;
static ui_touch_slot_t mt_slots[UI_MULTITOUCH_MAX_POINTS];
static uint32_t mt_seq;
static int mt_raw_max_x = 1060;
static int mt_raw_max_y = 2400;
static int mt_screen_w = 568;
static int mt_screen_h = 1232;
static int mt_rotation_degrees;
static int mt_cap_slot;
static int mt_cap_tracking_id;
static int mt_cap_pos_x;
static int mt_cap_pos_y;
static int mt_cap_single_x;
static int mt_cap_single_y;
static int mt_slot_count = 1;
static int mt_last_active_count = -1;
static int mt_max_active_count;
static char mt_protocol_name[24] = "unknown";
static char mt_status_text[192] = "mt: stopped";

static uint64_t mt_monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t mt_input_event_us(const struct input_event *event)
{
    return (uint64_t)event->input_event_sec * 1000000ULL +
           (uint64_t)event->input_event_usec;
}

static int mt_clamp_slot(int slot)
{
    if(slot < 0) {
        return 0;
    }
    if(slot >= UI_MULTITOUCH_MAX_POINTS) {
        return UI_MULTITOUCH_MAX_POINTS - 1;
    }
    return slot;
}

static int mt_normalize_rotation(int rotation_degrees)
{
    switch(rotation_degrees) {
    case 90:
    case 180:
    case 270:
        return rotation_degrees;
    default:
        return 0;
    }
}

static int mt_map_axis(int raw, int raw_max, int out_size)
{
    if(raw < 0) {
        return -1;
    }
    if(raw_max <= 0 || out_size <= 1) {
        return 0;
    }
    if(raw > raw_max) {
        raw = raw_max;
    }
    return raw * (out_size - 1) / raw_max;
}

static int mt_map_axis_reverse(int raw, int raw_max, int out_size)
{
    int mapped = mt_map_axis(raw, raw_max, out_size);

    return mapped >= 0 ? (out_size - 1) - mapped : -1;
}

static int mt_bit_is_set(const unsigned long *bits, int bit)
{
    if(!bits || bit < 0) {
        return 0;
    }
    return (bits[bit / MT_BITS_PER_LONG] &
            (1UL << (bit % MT_BITS_PER_LONG))) != 0;
}

static void mt_update_status_locked(int active_count)
{
    if(active_count > mt_max_active_count) {
        mt_max_active_count = active_count;
    }
    snprintf(mt_status_text, sizeof(mt_status_text),
             "mt: %s active=%d max=%d slots=%d cap slot=%d xy=%d/%d track=%d single=%d/%d seq=%lu",
             mt_protocol_name, active_count, mt_max_active_count, mt_slot_count, mt_cap_slot,
             mt_cap_pos_x, mt_cap_pos_y, mt_cap_tracking_id,
             mt_cap_single_x, mt_cap_single_y, (unsigned long)mt_seq);
}

static void mt_format_active_slots(const ui_touch_slot_t *slots, char *buf,
                                   size_t len)
{
    size_t used = 0;
    int any = 0;

    if(!buf || len == 0) {
        return;
    }
    buf[0] = '\0';
    for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
        int written;

        if(!slots[i].active || slots[i].raw_x < 0 || slots[i].raw_y < 0) {
            continue;
        }
        written = snprintf(buf + used, len - used, "%s%d(%d,%d)",
                           any ? "," : "", i, slots[i].raw_x, slots[i].raw_y);
        any = 1;
        if(written < 0 || (size_t)written >= len - used) {
            buf[len - 1] = '\0';
            return;
        }
        used += (size_t)written;
    }
    if(!any) {
        snprintf(buf, len, "-");
    }
}

static void mt_set_protocol(const char *protocol)
{
    pthread_mutex_lock(&mt_lock);
    snprintf(mt_protocol_name, sizeof(mt_protocol_name), "%s",
             protocol && protocol[0] ? protocol : "unknown");
    mt_update_status_locked(mt_last_active_count >= 0 ? mt_last_active_count : 0);
    pthread_mutex_unlock(&mt_lock);
}

static int mt_probe_capabilities(int fd)
{
    unsigned long abs_bits[MT_ABS_BITS_LEN];
    struct input_absinfo absinfo;
    int slot_count = 1;
    int has_slot = 0;
    int has_tracking = 0;
    int has_pos_x = 0;
    int has_pos_y = 0;
    int has_single_x = 0;
    int has_single_y = 0;

    memset(abs_bits, 0, sizeof(abs_bits));
    if(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0) {
        has_slot = mt_bit_is_set(abs_bits, ABS_MT_SLOT);
        has_tracking = mt_bit_is_set(abs_bits, ABS_MT_TRACKING_ID);
        has_pos_x = mt_bit_is_set(abs_bits, ABS_MT_POSITION_X);
        has_pos_y = mt_bit_is_set(abs_bits, ABS_MT_POSITION_Y);
        has_single_x = mt_bit_is_set(abs_bits, ABS_X);
        has_single_y = mt_bit_is_set(abs_bits, ABS_Y);
    }

    if(has_slot && ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &absinfo) == 0) {
        slot_count = absinfo.maximum - absinfo.minimum + 1;
        if(slot_count < 1) {
            slot_count = 1;
        }
        if(slot_count > UI_MULTITOUCH_MAX_POINTS) {
            slot_count = UI_MULTITOUCH_MAX_POINTS;
        }
    }

    pthread_mutex_lock(&mt_lock);
    mt_cap_slot = has_slot;
    mt_cap_tracking_id = has_tracking;
    mt_cap_pos_x = has_pos_x;
    mt_cap_pos_y = has_pos_y;
    mt_cap_single_x = has_single_x;
    mt_cap_single_y = has_single_y;
    mt_slot_count = slot_count;
    snprintf(mt_protocol_name, sizeof(mt_protocol_name), "%s",
             has_slot ? "slot" : (has_pos_x && has_pos_y ? "sync" : "single"));
    mt_update_status_locked(0);
    pthread_mutex_unlock(&mt_lock);

    fprintf(stderr,
            "[multitouch] caps slot=%d tracking=%d pos=%d/%d single=%d/%d slots=%d\n",
            has_slot, has_tracking, has_pos_x, has_pos_y, has_single_x,
            has_single_y, slot_count);
    return has_slot;
}

static void mt_map_locked(ui_touch_slot_t *slot)
{
    switch(mt_rotation_degrees) {
    case 90:
        slot->x = mt_map_axis(slot->raw_y, mt_raw_max_y, mt_screen_w);
        slot->y = mt_map_axis_reverse(slot->raw_x, mt_raw_max_x, mt_screen_h);
        break;
    case 180:
        slot->x = mt_map_axis_reverse(slot->raw_x, mt_raw_max_x, mt_screen_w);
        slot->y = mt_map_axis_reverse(slot->raw_y, mt_raw_max_y, mt_screen_h);
        break;
    case 270:
        slot->x = mt_map_axis_reverse(slot->raw_y, mt_raw_max_y, mt_screen_w);
        slot->y = mt_map_axis(slot->raw_x, mt_raw_max_x, mt_screen_h);
        break;
    default:
        slot->x = mt_map_axis(slot->raw_x, mt_raw_max_x, mt_screen_w);
        slot->y = mt_map_axis(slot->raw_y, mt_raw_max_y, mt_screen_h);
        break;
    }
}

void ui_multitouch_set_transform(int raw_max_x, int raw_max_y, int screen_w,
                                 int screen_h, int rotation_degrees)
{
    pthread_mutex_lock(&mt_lock);
    mt_raw_max_x = raw_max_x > 0 ? raw_max_x : 1060;
    mt_raw_max_y = raw_max_y > 0 ? raw_max_y : 2400;
    mt_screen_w = screen_w > 0 ? screen_w : 568;
    mt_screen_h = screen_h > 0 ? screen_h : 1232;
    mt_rotation_degrees = mt_normalize_rotation(rotation_degrees);
    for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
        mt_map_locked(&mt_slots[i]);
    }
    pthread_mutex_unlock(&mt_lock);
}

static void mt_publish(ui_touch_slot_t *local)
{
    int active_count = 0;

    pthread_mutex_lock(&mt_lock);
    mt_seq++;
    for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
        mt_slots[i] = local[i];
        mt_map_locked(&mt_slots[i]);
        if(mt_slots[i].active && mt_slots[i].x >= 0 && mt_slots[i].y >= 0) {
            active_count++;
        }
    }
    if(active_count != mt_last_active_count) {
        char slots[160];

        mt_format_active_slots(mt_slots, slots, sizeof(slots));
        fprintf(stderr,
                "[multitouch] active=%d max=%d seq=%lu protocol=%s slots=%s\n",
                active_count, mt_max_active_count, (unsigned long)mt_seq,
                mt_protocol_name, slots);
        mt_last_active_count = active_count;
    }
    mt_update_status_locked(active_count);
    pthread_mutex_unlock(&mt_lock);
}

static void mt_clear_local(ui_touch_slot_t *local)
{
    memset(local, 0, sizeof(ui_touch_slot_t) * UI_MULTITOUCH_MAX_POINTS);
    for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
        local[i].tracking_id = -1;
        local[i].raw_x = -1;
        local[i].raw_y = -1;
        local[i].x = -1;
        local[i].y = -1;
    }
}

static int mt_count_local_active(const ui_touch_slot_t *local)
{
    int active_count = 0;

    if(!local) {
        return 0;
    }
    for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
        if(local[i].active && local[i].raw_x >= 0 && local[i].raw_y >= 0) {
            active_count++;
        }
    }
    return active_count;
}

static int mt_query_slots_abs(int fd, int code, int *values, int slot_count)
{
    int data[UI_MULTITOUCH_MAX_POINTS + 1];
    int bytes;

    if(!values || slot_count <= 0) {
        return -1;
    }
    if(slot_count > UI_MULTITOUCH_MAX_POINTS) {
        slot_count = UI_MULTITOUCH_MAX_POINTS;
    }

    for(int i = 0; i <= slot_count; i++) {
        data[i] = -1;
    }
    data[0] = code;
    bytes = (slot_count + 1) * (int)sizeof(int);
    if(ioctl(fd, EVIOCGMTSLOTS(bytes), data) < 0) {
        return -1;
    }
    for(int i = 0; i < slot_count; i++) {
        values[i] = data[i + 1];
    }
    return 0;
}

static int mt_refresh_slots_from_kernel(int fd, ui_touch_slot_t *local,
                                        uint64_t kernel_us, uint64_t app_us)
{
    int slot_count;
    int has_tracking;
    int has_x;
    int has_y;
    int tracking[UI_MULTITOUCH_MAX_POINTS];
    int xs[UI_MULTITOUCH_MAX_POINTS];
    int ys[UI_MULTITOUCH_MAX_POINTS];

    pthread_mutex_lock(&mt_lock);
    slot_count = mt_slot_count;
    has_tracking = mt_cap_tracking_id;
    has_x = mt_cap_pos_x;
    has_y = mt_cap_pos_y;
    pthread_mutex_unlock(&mt_lock);

    if(slot_count <= 0) {
        slot_count = 1;
    }
    if(slot_count > UI_MULTITOUCH_MAX_POINTS) {
        slot_count = UI_MULTITOUCH_MAX_POINTS;
    }
    if(!has_x || !has_y) {
        return -1;
    }

    for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
        tracking[i] = -1;
        xs[i] = -1;
        ys[i] = -1;
    }

    if(has_tracking &&
       mt_query_slots_abs(fd, ABS_MT_TRACKING_ID, tracking, slot_count) != 0) {
        return -1;
    }
    if(mt_query_slots_abs(fd, ABS_MT_POSITION_X, xs, slot_count) != 0 ||
       mt_query_slots_abs(fd, ABS_MT_POSITION_Y, ys, slot_count) != 0) {
        return -1;
    }

    for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
        if(i >= slot_count) {
            local[i].active = 0;
            local[i].tracking_id = -1;
            local[i].raw_x = -1;
            local[i].raw_y = -1;
            continue;
        }
        local[i].raw_x = xs[i];
        local[i].raw_y = ys[i];
        local[i].kernel_us = kernel_us;
        local[i].app_us = app_us;
        if(has_tracking) {
            local[i].tracking_id = tracking[i];
            local[i].active = tracking[i] >= 0 && xs[i] >= 0 && ys[i] >= 0;
        } else {
            local[i].tracking_id = i;
            local[i].active = xs[i] >= 0 && ys[i] >= 0;
        }
    }
    return 0;
}

static void *mt_thread_main(void *arg)
{
    char *path = (char *)arg;
    ui_touch_slot_t local[UI_MULTITOUCH_MAX_POINTS];
    int frame_active[UI_MULTITOUCH_MAX_POINTS] = { 0 };
    int current_slot = 0;
    int has_slot_protocol = 0;
    int logged_slot_protocol = 0;
    int logged_slot_query_fail = 0;
    int logged_slot_query_drop = 0;
    int logged_sync_protocol = 0;
    int fd;
    int clock_id = CLOCK_MONOTONIC;

    mt_clear_local(local);
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) {
        fprintf(stderr, "[multitouch] open failed path=%s errno=%d (%s)\n",
                path, errno, strerror(errno));
        pthread_mutex_lock(&mt_lock);
        snprintf(mt_status_text, sizeof(mt_status_text),
                 "mt: open failed errno=%d", errno);
        pthread_mutex_unlock(&mt_lock);
        free(path);
        mt_running = 0;
        return NULL;
    }

    if(ioctl(fd, EVIOCSCLOCKID, &clock_id) < 0) {
        fprintf(stderr, "[multitouch] EVIOCSCLOCKID failed errno=%d (%s)\n",
                errno, strerror(errno));
    }
    fprintf(stderr, "[multitouch] start path=%s slots=%d\n", path,
            UI_MULTITOUCH_MAX_POINTS);
    has_slot_protocol = mt_probe_capabilities(fd);
    if(has_slot_protocol) {
        mt_set_protocol("slot-query");
        logged_slot_protocol = 1;
    }
    free(path);

    while(mt_running) {
        struct input_event event;
        ssize_t rd = read(fd, &event, sizeof(event));
        uint64_t kernel_us;
        uint64_t app_us;

        if(rd != (ssize_t)sizeof(event)) {
            if(rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                          errno == EINTR)) {
                usleep(2000);
                continue;
            }
            if(rd < 0) {
                fprintf(stderr, "[multitouch] read failed errno=%d (%s)\n",
                        errno, strerror(errno));
            }
            break;
        }

        kernel_us = mt_input_event_us(&event);
        app_us = mt_monotonic_us();

        if(event.type == EV_ABS) {
            if(event.code == ABS_MT_SLOT) {
                has_slot_protocol = 1;
                current_slot = mt_clamp_slot(event.value);
                if(!logged_slot_protocol) {
                    fprintf(stderr, "[multitouch] protocol=slot\n");
                    mt_set_protocol("slot");
                    logged_slot_protocol = 1;
                }
            } else if(event.code == ABS_MT_TRACKING_ID) {
                local[current_slot].tracking_id = event.value;
                local[current_slot].active = event.value >= 0;
                local[current_slot].kernel_us = kernel_us;
                local[current_slot].app_us = app_us;
                if(event.value >= 0) {
                    frame_active[current_slot] = 1;
                } else {
                    local[current_slot].raw_x = -1;
                    local[current_slot].raw_y = -1;
                    frame_active[current_slot] = 0;
                }
            } else if(event.code == ABS_MT_POSITION_X) {
                local[current_slot].raw_x = event.value;
                local[current_slot].kernel_us = kernel_us;
                local[current_slot].app_us = app_us;
                if(has_slot_protocol) {
                    if(local[current_slot].raw_y >= 0) {
                        local[current_slot].active = 1;
                        if(local[current_slot].tracking_id < 0) {
                            local[current_slot].tracking_id = current_slot;
                        }
                    }
                    frame_active[current_slot] = 1;
                } else {
                    local[current_slot].active = 1;
                    if(local[current_slot].tracking_id < 0) {
                        local[current_slot].tracking_id = current_slot;
                    }
                    frame_active[current_slot] = 1;
                }
            } else if(event.code == ABS_MT_POSITION_Y) {
                local[current_slot].raw_y = event.value;
                local[current_slot].kernel_us = kernel_us;
                local[current_slot].app_us = app_us;
                if(has_slot_protocol) {
                    if(local[current_slot].raw_x >= 0) {
                        local[current_slot].active = 1;
                        if(local[current_slot].tracking_id < 0) {
                            local[current_slot].tracking_id = current_slot;
                        }
                    }
                    frame_active[current_slot] = 1;
                } else {
                    local[current_slot].active = 1;
                    if(local[current_slot].tracking_id < 0) {
                        local[current_slot].tracking_id = current_slot;
                    }
                    frame_active[current_slot] = 1;
                }
            } else if(event.code == ABS_X) {
                local[0].raw_x = event.value;
                local[0].kernel_us = kernel_us;
                local[0].app_us = app_us;
            } else if(event.code == ABS_Y) {
                local[0].raw_y = event.value;
                local[0].kernel_us = kernel_us;
                local[0].app_us = app_us;
            }
        } else if(event.type == EV_KEY &&
                  (event.code == BTN_TOUCH || event.code == BTN_MOUSE)) {
            if(event.value == 0 && !has_slot_protocol) {
                for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
                    local[i].active = 0;
                    local[i].tracking_id = -1;
                    local[i].kernel_us = kernel_us;
                    local[i].app_us = app_us;
                    frame_active[i] = 0;
                }
                current_slot = 0;
            } else if(local[0].raw_x >= 0 && local[0].raw_y >= 0) {
                local[0].active = 1;
                local[0].tracking_id = 0;
                local[0].kernel_us = kernel_us;
                local[0].app_us = app_us;
            }
        } else if(event.type == EV_SYN) {
            if(event.code == SYN_MT_REPORT && !has_slot_protocol) {
                if(!logged_sync_protocol) {
                    fprintf(stderr, "[multitouch] protocol=sync-report\n");
                    mt_set_protocol("sync-report");
                    logged_sync_protocol = 1;
                }
                if(current_slot < UI_MULTITOUCH_MAX_POINTS - 1) {
                    current_slot++;
                }
            } else if(event.code == SYN_REPORT) {
                if(has_slot_protocol) {
                    ui_touch_slot_t event_snapshot[UI_MULTITOUCH_MAX_POINTS];
                    int event_active;

                    memcpy(event_snapshot, local, sizeof(event_snapshot));
                    event_active = mt_count_local_active(event_snapshot);
                    if(mt_refresh_slots_from_kernel(fd, local, kernel_us,
                                                    app_us) == 0) {
                        int query_active = mt_count_local_active(local);

                        if(query_active < event_active) {
                            memcpy(local, event_snapshot, sizeof(event_snapshot));
                            if(!logged_slot_query_drop) {
                                fprintf(stderr,
                                        "[multitouch] slot query dropped contacts query=%d events=%d, using event stream\n",
                                        query_active, event_active);
                                mt_set_protocol("slot-events");
                                logged_slot_query_drop = 1;
                            }
                        }
                    } else if(!logged_slot_query_fail) {
                        fprintf(stderr,
                                "[multitouch] slot query unavailable, using event stream\n");
                        mt_set_protocol("slot-events");
                        logged_slot_query_fail = 1;
                    }
                } else {
                    int frame_has_mt = 0;

                    for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
                        if(frame_active[i]) {
                            frame_has_mt = 1;
                            break;
                        }
                    }

                    if(frame_has_mt) {
                        for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
                            if(frame_active[i] && local[i].raw_x >= 0 &&
                               local[i].raw_y >= 0) {
                                local[i].active = 1;
                                if(local[i].tracking_id < 0) {
                                    local[i].tracking_id = i;
                                }
                            } else {
                                local[i].active = 0;
                                local[i].tracking_id = -1;
                            }
                        }
                    }
                }

                mt_publish(local);

                if(!has_slot_protocol) {
                    mt_clear_local(local);
                    memset(frame_active, 0, sizeof(frame_active));
                    current_slot = 0;
                }
            }
        }
    }

    close(fd);
    fprintf(stderr, "[multitouch] stop\n");
    pthread_mutex_lock(&mt_lock);
    snprintf(mt_status_text, sizeof(mt_status_text), "%s", "mt: stopped");
    pthread_mutex_unlock(&mt_lock);
    mt_running = 0;
    return NULL;
}

int ui_multitouch_start(const char *path)
{
    char *copy;

    if(!path || !path[0]) {
        return -1;
    }
    if(mt_thread_valid) {
        return 0;
    }

    copy = strdup(path);
    if(!copy) {
        return -1;
    }

    mt_running = 1;
    pthread_mutex_lock(&mt_lock);
    snprintf(mt_status_text, sizeof(mt_status_text), "%s", "mt: starting");
    mt_last_active_count = -1;
    mt_max_active_count = 0;
    pthread_mutex_unlock(&mt_lock);
    if(pthread_create(&mt_thread, NULL, mt_thread_main, copy) != 0) {
        free(copy);
        mt_running = 0;
        return -1;
    }
    mt_thread_valid = 1;
    return 0;
}

void ui_multitouch_stop(void)
{
    if(!mt_thread_valid) {
        return;
    }
    mt_running = 0;
    pthread_join(mt_thread, NULL);
    mt_thread_valid = 0;
}

uint32_t ui_multitouch_get_points(ui_touch_point_t *points, size_t max_points)
{
    uint32_t active = 0;

    pthread_mutex_lock(&mt_lock);
    for(int i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
        if(mt_slots[i].active && mt_slots[i].x >= 0 && mt_slots[i].y >= 0) {
            if(points && active < max_points) {
                points[active].active = 1;
                points[active].slot = i;
                points[active].tracking_id = mt_slots[i].tracking_id;
                points[active].raw_x = mt_slots[i].raw_x;
                points[active].raw_y = mt_slots[i].raw_y;
                points[active].x = mt_slots[i].x;
                points[active].y = mt_slots[i].y;
                points[active].kernel_us = mt_slots[i].kernel_us;
                points[active].app_us = mt_slots[i].app_us;
                points[active].seq = mt_seq;
            }
            active++;
        }
    }
    pthread_mutex_unlock(&mt_lock);
    return active;
}

uint32_t ui_multitouch_active_count(void)
{
    return ui_multitouch_get_points(NULL, 0);
}

void ui_multitouch_get_status(char *buf, size_t len)
{
    if(!buf || len == 0) {
        return;
    }
    pthread_mutex_lock(&mt_lock);
    snprintf(buf, len, "%s", mt_status_text);
    pthread_mutex_unlock(&mt_lock);
}
