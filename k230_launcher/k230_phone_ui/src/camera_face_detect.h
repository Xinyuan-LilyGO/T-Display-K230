#ifndef K230_PHONE_UI_CAMERA_FACE_DETECT_H
#define K230_PHONE_UI_CAMERA_FACE_DETECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int camera_face_detect_rgb565(const uint8_t *rgb565, unsigned width,
                              unsigned height, int *boxes_xywh_score,
                              int max_boxes);

#ifdef __cplusplus
}
#endif

#endif
