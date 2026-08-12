#include "camera_face_detect.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

#include <opencv2/core.hpp>

#include "face_detection.h"

#define CAMERA_FACE_MODEL_PATH "/root/app/face_detect/face_detection_320.kmodel"

static std::mutex g_face_lock;
static std::unique_ptr<FaceDetection> g_face_detector;
static bool g_face_init_failed;

static uint8_t rgb565_expand5(uint16_t value)
{
    return static_cast<uint8_t>((value * 255U) / 31U);
}

static uint8_t rgb565_expand6(uint16_t value)
{
    return static_cast<uint8_t>((value * 255U) / 63U);
}

static bool model_file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");

    if(!fp) {
        return false;
    }
    fclose(fp);
    return true;
}

static FaceDetection *camera_face_get_detector(void)
{
    if(g_face_detector) {
        return g_face_detector.get();
    }
    if(g_face_init_failed || !model_file_exists(CAMERA_FACE_MODEL_PATH)) {
        g_face_init_failed = true;
        return nullptr;
    }

    try {
        g_face_detector = std::make_unique<FaceDetection>(
            CAMERA_FACE_MODEL_PATH, 0.60f, 0.20f, 0);
    } catch(...) {
        g_face_init_failed = true;
        g_face_detector.reset();
        return nullptr;
    }

    return g_face_detector.get();
}

extern "C" int camera_face_detect_rgb565(const uint8_t *rgb565, unsigned width,
                                         unsigned height,
                                         int *boxes_xywh_score,
                                         int max_boxes)
{
    FaceDetection *detector;
    cv::Mat frame;
    std::vector<FaceDetectionInfo> results;
    int out_count = 0;

    if(!rgb565 || !boxes_xywh_score || max_boxes <= 0 ||
       width == 0 || height == 0) {
        return -1;
    }

    std::lock_guard<std::mutex> guard(g_face_lock);
    detector = camera_face_get_detector();
    if(!detector) {
        return -2;
    }

    frame.create(static_cast<int>(height), static_cast<int>(width), CV_8UC3);
    for(unsigned y = 0; y < height; y++) {
        uint8_t *row = frame.ptr<uint8_t>(static_cast<int>(y));
        for(unsigned x = 0; x < width; x++) {
            size_t off = ((size_t)y * width + x) * 2U;
            uint16_t value = static_cast<uint16_t>(rgb565[off]) |
                             (static_cast<uint16_t>(rgb565[off + 1]) << 8);

            row[x * 3U + 0U] = rgb565_expand5(value & 0x1FU);
            row[x * 3U + 1U] = rgb565_expand6((value >> 5) & 0x3FU);
            row[x * 3U + 2U] = rgb565_expand5((value >> 11) & 0x1FU);
        }
    }

    try {
        detector->pre_process(frame);
        detector->inference();
        detector->post_process({width, height}, results);
    } catch(...) {
        return -3;
    }

    for(const auto &item : results) {
        int x = static_cast<int>(item.bbox.x);
        int y = static_cast<int>(item.bbox.y);
        int w = static_cast<int>(item.bbox.w);
        int h = static_cast<int>(item.bbox.h);
        int score = static_cast<int>(item.score * 1000.0f);

        if(w <= 0 || h <= 0) {
            continue;
        }
        x = std::clamp(x, 0, static_cast<int>(width) - 1);
        y = std::clamp(y, 0, static_cast<int>(height) - 1);
        if(x + w > static_cast<int>(width)) {
            w = static_cast<int>(width) - x;
        }
        if(y + h > static_cast<int>(height)) {
            h = static_cast<int>(height) - y;
        }
        if(w <= 0 || h <= 0) {
            continue;
        }

        boxes_xywh_score[out_count * 5 + 0] = x;
        boxes_xywh_score[out_count * 5 + 1] = y;
        boxes_xywh_score[out_count * 5 + 2] = w;
        boxes_xywh_score[out_count * 5 + 3] = h;
        boxes_xywh_score[out_count * 5 + 4] = score;
        out_count++;
        if(out_count >= max_boxes) {
            break;
        }
    }

    return out_count;
}
