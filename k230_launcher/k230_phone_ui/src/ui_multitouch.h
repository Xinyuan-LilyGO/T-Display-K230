#ifndef K230_PHONE_UI_MULTITOUCH_H
#define K230_PHONE_UI_MULTITOUCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MULTITOUCH_MAX_POINTS 10

typedef struct {
    int active;
    int slot;
    int tracking_id;
    int raw_x;
    int raw_y;
    int x;
    int y;
    uint64_t kernel_us;
    uint64_t app_us;
    uint32_t seq;
} ui_touch_point_t;

void ui_multitouch_set_transform(int raw_max_x, int raw_max_y, int screen_w,
                                 int screen_h, int rotation_degrees);
int ui_multitouch_start(const char *path);
void ui_multitouch_stop(void);
uint32_t ui_multitouch_get_points(ui_touch_point_t *points, size_t max_points);
uint32_t ui_multitouch_active_count(void);
void ui_multitouch_get_status(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
