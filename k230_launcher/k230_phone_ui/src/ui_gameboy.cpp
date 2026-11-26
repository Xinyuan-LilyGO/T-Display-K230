#include "ui_gameboy.h"

#include "ui_audio.h"
#include "ui_hardware.h"
#include "ui_multitouch.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

extern "C" {
#include "third_party/sameboy/Core/gb.h"
#include "third_party/sameboy/Core/timing.h"
}

extern "C" const unsigned char sameboy_dmg_boot[];
extern "C" const unsigned int sameboy_dmg_boot_len;
extern "C" const unsigned char sameboy_cgb_boot[];
extern "C" const unsigned int sameboy_cgb_boot_len;

#define GBUI_ROM_MAX 48
#define GBUI_PATH_MAX 256
#define GBUI_TITLE_MAX 96
#define GBUI_META_MAX 160
#define GBUI_ROM_DIR_PRIMARY "/root/gameboy"
#define GBUI_LOG_PATH "/tmp/k230_phone_gameboy.log"
#define GBUI_VIEW_W 480
#define GBUI_VIEW_H 432
#define GBUI_FRAME_BYTES (GBUI_VIEW_W * GBUI_VIEW_H * 2)
#define GBUI_NATIVE_W 160
#define GBUI_NATIVE_H 144
#define GBUI_REFRESH_TIMER_MS 16
#define GBUI_AUDIO_RATE 48000
#define GBUI_AUDIO_MAX_FRAMES 4096
#define GBUI_AUDIO_LOG_PATH "/tmp/k230_phone_gameboy_audio.log"
#define GBUI_PCM_VOLUME_BIN "/root/app/k230_phone_ui/k230_pcm_volume"
#define GBUI_VOLUME_CONFIG_PATH "/root/.k230_phone_audio_volume"

typedef struct {
    char path[GBUI_PATH_MAX];
    char title[GBUI_TITLE_MAX];
    char cover_path[GBUI_PATH_MAX];
    uint64_t size;
    uint32_t crc32;
    uint8_t valid_header;
    char format[12];
    char region[20];
    char product[24];
    char meta[GBUI_META_MAX];
} gb_rom_item_t;

static const char *const gb_scan_dirs[] = {
    GBUI_ROM_DIR_PRIMARY,
    "/root/roms/gameboy",
    "/mnt/sdcard/gameboy",
    "/mnt/gameboy",
    "/media/gameboy",
};

static gb_rom_item_t gb_roms[GBUI_ROM_MAX];
static int gb_rom_count;
static int gb_selected_index = -1;

static lv_timer_t *gb_ui_timer;
static lv_obj_t *gb_body;
static lv_obj_t *gb_library_panel;
static lv_obj_t *gb_game_panel;
static lv_obj_t *gb_list_panel;
static lv_obj_t *gb_status_label;
static lv_obj_t *gb_count_label;
static lv_obj_t *gb_title_label;
static lv_obj_t *gb_meta_label;
static lv_obj_t *gb_path_label;
static lv_obj_t *gb_cover_image;
static lv_obj_t *gb_cover_placeholder;
static lv_obj_t *gb_frame_image;
static lv_obj_t *gb_frame_placeholder;
static lv_obj_t *gb_game_status_label;
static lv_obj_t *gb_control_btn[8];
static int gb_list_cached_w;

static pthread_t gb_emulator_thread;
static int gb_emulator_thread_valid;
static volatile int gb_emulator_running;
static volatile int gb_emulator_finished;
static volatile int gb_emulator_result;
static volatile int gb_stop_requested;
static uint32_t gb_controller_state = 0xFFFFFFFFU;
static uint32_t gb_input_old_state = 0xFFFFFFFFU;
static uint32_t gb_lvgl_pressed_mask;
static uint32_t gb_touch_pressed_mask;
static uint32_t gb_keyboard_pressed_mask;
static char gb_active_rom_path[GBUI_PATH_MAX];

static pthread_mutex_t gb_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *gb_frame_a;
static uint16_t *gb_frame_b;
static uint16_t *gb_frame_front;
static uint16_t *gb_frame_back;
static uint16_t *gb_frame_display;
static uint32_t gb_frame_seq;
static uint32_t gb_frame_seen_seq;
static lv_image_dsc_t gb_frame_dsc;
static uint32_t gb_native_frame[GBUI_NATIVE_W * GBUI_NATIVE_H];
static FILE *gb_audio_pipe;
static int16_t *gb_audio_buffer;
static size_t gb_audio_frames;

static void gb_log(const char *fmt, ...)
{
    FILE *fp = fopen(GBUI_LOG_PATH, "a");
    va_list ap;

    if(!fp) {
        return;
    }
    fprintf(fp, "[%llu.%03llums] ",
            (unsigned long long)(ui_monotonic_us() / 1000ULL),
            (unsigned long long)(ui_monotonic_us() % 1000ULL));
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static int gb_path_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

static uint64_t gb_file_size(const char *path)
{
    struct stat st;
    if(!path || stat(path, &st) != 0) {
        return 0;
    }
    return (uint64_t)st.st_size;
}

static void gb_format_size(char *out, size_t out_len, uint64_t bytes)
{
    if(bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if(bytes >= 1024ULL) {
        snprintf(out, out_len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    }
}

static const char *gb_base_name(const char *path)
{
    const char *slash;

    if(!path) {
        return "";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int gb_has_ext(const char *path, const char *ext)
{
    size_t plen;
    size_t elen;

    if(!path || !ext) {
        return 0;
    }
    plen = strlen(path);
    elen = strlen(ext);
    return plen >= elen && strcasecmp(path + plen - elen, ext) == 0;
}

static void gb_split_dir_stem(const char *path, char *dir, size_t dir_len,
                               char *stem, size_t stem_len)
{
    const char *base = gb_base_name(path);
    const char *dot = strrchr(base, '.');
    size_t dlen = (size_t)(base - path);
    size_t slen = dot && dot > base ? (size_t)(dot - base) : strlen(base);

    if(dir && dir_len) {
        if(dlen >= dir_len) {
            dlen = dir_len - 1;
        }
        memcpy(dir, path, dlen);
        if(dlen > 1 && dir[dlen - 1] == '/') {
            dlen--;
        }
        dir[dlen] = '\0';
    }
    if(stem && stem_len) {
        if(slen >= stem_len) {
            slen = stem_len - 1;
        }
        memcpy(stem, base, slen);
        stem[slen] = '\0';
    }
}

static void gb_make_title(const char *path, char *out, size_t out_len)
{
    char stem[GBUI_TITLE_MAX];

    gb_split_dir_stem(path, NULL, 0, stem, sizeof(stem));
    for(size_t i = 0; stem[i]; i++) {
        if(stem[i] == '_' || stem[i] == '-') {
            stem[i] = ' ';
        }
    }
    snprintf(out, out_len, "%s", stem[0] ? stem : gb_base_name(path));
}

static uint32_t gb_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)-(int)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static uint32_t gb_crc32_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    uint8_t buf[4096];
    uint32_t crc = 0;

    if(!fp) {
        return 0;
    }
    while(1) {
        size_t got = fread(buf, 1, sizeof(buf), fp);
        if(got > 0) {
            crc = gb_crc32_update(crc, buf, got);
        }
        if(got < sizeof(buf)) {
            break;
        }
    }
    fclose(fp);
    return crc;
}

static void gb_find_cover(gb_rom_item_t *item)
{
    char dir[GBUI_PATH_MAX];
    char stem[GBUI_TITLE_MAX];
    const char *exts[] = {".png", ".jpg", ".jpeg", ".bmp"};

    item->cover_path[0] = '\0';
    gb_split_dir_stem(item->path, dir, sizeof(dir), stem, sizeof(stem));
    for(size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        char path[GBUI_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s%s", dir, stem, exts[i]);
        if(gb_path_exists(path)) {
            snprintf(item->cover_path, sizeof(item->cover_path), "%s", path);
            return;
        }
        snprintf(path, sizeof(path), "%s/covers/%s%s", dir, stem, exts[i]);
        if(gb_path_exists(path)) {
            snprintf(item->cover_path, sizeof(item->cover_path), "%s", path);
            return;
        }
    }
}

static void gb_format_rom_meta(const gb_rom_item_t *item, int compact,
                                char *buf, size_t len)
{
    (void)compact;
    if(!item || !buf || len == 0) {
        return;
    }
    snprintf(buf, len, "%s  %s  %s", item->format,
             item->region[0] ? item->region : "Region unknown",
             item->product[0] ? item->product : "Product unknown");
}

static void gb_copy_header_text(char *out, size_t out_len,
                                const uint8_t *src, size_t src_len)
{
    size_t end = src_len;

    if(!out || out_len == 0) {
        return;
    }
    while(end > 0 && (src[end - 1] == ' ' || src[end - 1] == '\0')) {
        end--;
    }
    if(end >= out_len) {
        end = out_len - 1;
    }
    for(size_t i = 0; i < end; i++) {
        out[i] = isprint(src[i]) ? (char)src[i] : ' ';
    }
    out[end] = '\0';
}

static int gb_parse_header(gb_rom_item_t *item)
{
    FILE *fp;
    uint8_t h[0x150];
    size_t got;
    uint8_t checksum = 0;
    int cgb;

    if(!item) {
        return 0;
    }
    if(item->size == 0) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "Empty ROM file");
        return 0;
    }
    if(item->size < sizeof(h) || item->size > 32U * 1024U * 1024U) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta),
                 item->size < sizeof(h) ? "ROM file is too small" :
                 "ROM exceeds the 32 MB limit");
        return 0;
    }
    fp = fopen(item->path, "rb");
    if(!fp) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "Cannot open ROM file");
        return 0;
    }
    got = fread(h, 1, sizeof(h), fp);
    fclose(fp);
    if(got != sizeof(h)) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "Cannot read ROM header");
        return 0;
    }

    for(unsigned i = 0x134; i <= 0x14C; i++) {
        checksum = (uint8_t)(checksum - h[i] - 1U);
    }

    cgb = (h[0x143] & 0x80U) != 0;
    item->valid_header = 1;
    snprintf(item->format, sizeof(item->format), "%s", cgb ? "GBC" : "GB");
    snprintf(item->region, sizeof(item->region), "%s",
             h[0x14A] == 0 ? "Japan" : "World");
    gb_copy_header_text(item->product, sizeof(item->product), h + 0x134,
                        cgb ? 11 : 15);
    gb_format_rom_meta(item, 0, item->meta, sizeof(item->meta));
    if(checksum != h[0x14D]) {
        snprintf(item->meta, sizeof(item->meta),
                 "%s  Header checksum differs", item->format);
    }
    return 1;
}

static int gb_validate_rom_for_start(const char *path, char *reason,
                                      size_t reason_len)
{
    struct stat st;
    gb_rom_item_t item;

    if(reason && reason_len) {
        reason[0] = '\0';
    }
    if(!path || !path[0]) {
        snprintf(reason, reason_len, "No ROM selected");
        return 0;
    }
    if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(reason, reason_len, "ROM file not found");
        return 0;
    }
    memset(&item, 0, sizeof(item));
    snprintf(item.path, sizeof(item.path), "%s", path);
    item.size = (uint64_t)st.st_size;
    if(!gb_parse_header(&item)) {
        snprintf(reason, reason_len, "%s", item.meta);
        return 0;
    }
    return 1;
}

static int gb_rom_exists(const char *path)
{
    for(int i = 0; i < gb_rom_count; i++) {
        if(strcmp(gb_roms[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void gb_add_rom_path(const char *path)
{
    gb_rom_item_t *item;

    if(!path || gb_rom_count >= GBUI_ROM_MAX || gb_rom_exists(path)) {
        return;
    }
    item = &gb_roms[gb_rom_count];
    memset(item, 0, sizeof(*item));
    snprintf(item->path, sizeof(item->path), "%s", path);
    item->size = gb_file_size(path);
    gb_make_title(path, item->title, sizeof(item->title));
    gb_parse_header(item);
    item->crc32 = gb_crc32_file(path);
    gb_find_cover(item);
    gb_rom_count++;
}

static void gb_scan_dir(const char *dir)
{
    DIR *dp;
    struct dirent *ent;

    dp = opendir(dir);
    if(!dp) {
        return;
    }
    while((ent = readdir(dp)) != NULL && gb_rom_count < GBUI_ROM_MAX) {
        char path[GBUI_PATH_MAX];
        struct stat st;

        if(ent->d_name[0] == '.') {
            continue;
        }
        if(!gb_has_ext(ent->d_name, ".gb") &&
           !gb_has_ext(ent->d_name, ".gbc")) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if(stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            gb_add_rom_path(path);
        }
    }
    closedir(dp);
}

static lv_obj_t *gb_button(lv_obj_t *parent, int x, int y, int w, int h,
                            const char *text, uint32_t color)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = ui_label(btn, text, &lv_font_montserrat_18, 0xFFFFFF);
    lv_obj_center(label);
    return btn;
}

static void gb_set_status(const char *text, uint32_t color)
{
    if(gb_status_label && lv_obj_is_valid(gb_status_label)) {
        lv_label_set_text(gb_status_label, text ? text : "");
        lv_obj_set_style_text_color(gb_status_label, lv_color_hex(color), 0);
    }
}

static void gb_update_cover(const gb_rom_item_t *item)
{
    if(!gb_cover_image || !gb_cover_placeholder) {
        return;
    }
    if(item && item->cover_path[0]) {
        lv_image_set_src(gb_cover_image, item->cover_path);
        lv_obj_clear_flag(gb_cover_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(gb_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(gb_cover_image, NULL);
        lv_obj_add_flag(gb_cover_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(gb_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void gb_update_selected_info(int index)
{
    char text[192];
    char size_buf[32];
    const gb_rom_item_t *item = NULL;

    if(index >= 0 && index < gb_rom_count) {
        item = &gb_roms[index];
        gb_selected_index = index;
    }

    if(gb_title_label && lv_obj_is_valid(gb_title_label)) {
        lv_label_set_text(gb_title_label, item ? item->title : "No ROM selected");
    }
    if(gb_meta_label && lv_obj_is_valid(gb_meta_label)) {
        if(item && item->valid_header) {
            gb_format_rom_meta(item, ui_is_landscape(), text, sizeof(text));
            lv_label_set_text(gb_meta_label, text);
        } else {
            lv_label_set_text(gb_meta_label,
                              item ? item->meta :
                              "Copy .gb or .gbc files to /root/gameboy");
        }
    }
    if(gb_path_label && lv_obj_is_valid(gb_path_label)) {
        if(item) {
            gb_format_size(size_buf, sizeof(size_buf), item->size);
            if(ui_is_landscape()) {
                snprintf(text, sizeof(text), "%s  CRC32 %08X  %s",
                         size_buf, item->crc32,
                         item->cover_path[0] ? "Cover found" : "No cover");
            } else {
                snprintf(text, sizeof(text), "%s\n%s  CRC32 %08X%s",
                         item->path, size_buf, item->crc32,
                         item->cover_path[0] ? "\nSidecar cover found" :
                         "\nNo sidecar cover found");
            }
        } else {
            snprintf(text, sizeof(text), "%s", GBUI_ROM_DIR_PRIMARY);
        }
        lv_label_set_text(gb_path_label, text);
    }
    gb_update_cover(item);
}

static int gb_prepare_frame_buffers(void)
{
    if(!gb_frame_a) {
        gb_frame_a = (uint16_t *)malloc(GBUI_FRAME_BYTES);
    }
    if(!gb_frame_b) {
        gb_frame_b = (uint16_t *)malloc(GBUI_FRAME_BYTES);
    }
    if(!gb_frame_display) {
        gb_frame_display = (uint16_t *)malloc(GBUI_FRAME_BYTES);
    }
    if(!gb_frame_a || !gb_frame_b || !gb_frame_display) {
        return 0;
    }
    memset(gb_frame_a, 0, GBUI_FRAME_BYTES);
    memset(gb_frame_b, 0, GBUI_FRAME_BYTES);
    memset(gb_frame_display, 0, GBUI_FRAME_BYTES);
    gb_frame_front = gb_frame_a;
    gb_frame_back = gb_frame_b;
    gb_frame_seq = 0;
    gb_frame_seen_seq = 0;

    memset(&gb_frame_dsc, 0, sizeof(gb_frame_dsc));
    gb_frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    gb_frame_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    gb_frame_dsc.header.w = GBUI_VIEW_W;
    gb_frame_dsc.header.h = GBUI_VIEW_H;
    gb_frame_dsc.header.stride = GBUI_VIEW_W * 2;
    gb_frame_dsc.data_size = GBUI_FRAME_BYTES;
    gb_frame_dsc.data = (const uint8_t *)gb_frame_display;
    return 1;
}

static void gb_free_frame_buffers(void)
{
    pthread_mutex_lock(&gb_frame_mutex);
    free(gb_frame_a);
    free(gb_frame_b);
    free(gb_frame_display);
    gb_frame_a = NULL;
    gb_frame_b = NULL;
    gb_frame_display = NULL;
    gb_frame_front = NULL;
    gb_frame_back = NULL;
    gb_frame_seq = 0;
    gb_frame_seen_seq = 0;
    pthread_mutex_unlock(&gb_frame_mutex);
}

static void gb_show_game_view(int show)
{
    if(gb_library_panel && lv_obj_is_valid(gb_library_panel)) {
        if(show) {
            lv_obj_add_flag(gb_library_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(gb_library_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(gb_game_panel && lv_obj_is_valid(gb_game_panel)) {
        if(show) {
            lv_obj_clear_flag(gb_game_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(gb_game_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void gb_update_controller_state(void)
{
    uint32_t pressed_mask = (gb_lvgl_pressed_mask |
                             gb_touch_pressed_mask |
                             gb_keyboard_pressed_mask) & 0xFFU;

    __atomic_store_n(&gb_controller_state, ~pressed_mask, __ATOMIC_RELAXED);
}

static void gb_set_controller_bit(unsigned bit, int pressed)
{
    uint32_t mask = 1U << bit;

    if(pressed) {
        __sync_fetch_and_or(&gb_lvgl_pressed_mask, mask);
    } else {
        __sync_fetch_and_and(&gb_lvgl_pressed_mask, ~mask);
    }
    gb_update_controller_state();
}

static void gb_set_touch_pressed_mask(uint32_t mask)
{
    mask &= 0xFFU;
    if(mask == gb_touch_pressed_mask) {
        return;
    }
    gb_touch_pressed_mask = mask;
    gb_update_controller_state();
}

static int gb_point_in_obj(const ui_touch_point_t *point, lv_obj_t *obj)
{
    lv_area_t area;

    if(!point || !obj || !lv_obj_is_valid(obj) ||
       lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }

    lv_obj_get_coords(obj, &area);
    return point->x >= area.x1 && point->x <= area.x2 &&
           point->y >= area.y1 && point->y <= area.y2;
}

static void gb_update_multitouch_controls(void)
{
    ui_touch_point_t points[UI_MULTITOUCH_MAX_POINTS];
    uint32_t count;
    uint32_t mask = 0;

    if(!gb_game_panel || !lv_obj_is_valid(gb_game_panel) ||
       lv_obj_has_flag(gb_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        gb_set_touch_pressed_mask(0);
        return;
    }

    count = ui_multitouch_get_points(points, UI_MULTITOUCH_MAX_POINTS);
    for(uint32_t i = 0; i < count && i < UI_MULTITOUCH_MAX_POINTS; i++) {
        for(unsigned bit = 0; bit < 8; bit++) {
            if(gb_point_in_obj(&points[i], gb_control_btn[bit])) {
                mask |= 1U << bit;
            }
        }
    }
    gb_set_touch_pressed_mask(mask);
}

static void gb_control_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    uintptr_t raw = (uintptr_t)lv_event_get_user_data(event);
    unsigned bit;

    if(raw == 0) {
        return;
    }
    bit = (unsigned)(raw - 1U);
    if(code == LV_EVENT_PRESSED) {
        gb_set_controller_bit(bit, 1);
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        gb_set_controller_bit(bit, 0);
    }
}

static void gb_add_control(lv_obj_t *parent, int x, int y, int w, int h,
                            const char *text, uint32_t color, unsigned bit)
{
    lv_obj_t *btn = gb_button(parent, x, y, w, h, text, color);

    if(bit < 8) {
        gb_control_btn[bit] = btn;
    }
    lv_obj_add_event_cb(btn, gb_control_event, LV_EVENT_PRESSED,
                        (void *)(uintptr_t)(bit + 1U));
    lv_obj_add_event_cb(btn, gb_control_event, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)(bit + 1U));
    lv_obj_add_event_cb(btn, gb_control_event, LV_EVENT_PRESS_LOST,
                        (void *)(uintptr_t)(bit + 1U));
}

static void gb_request_stop(void)
{
    gb_stop_requested = 1;
}

static void gb_set_keyboard_bit(unsigned bit, int pressed)
{
    uint32_t mask;

    if(bit >= 8) {
        return;
    }
    mask = 1U << bit;
    if(pressed) {
        __sync_fetch_and_or(&gb_keyboard_pressed_mask, mask);
    } else {
        __sync_fetch_and_and(&gb_keyboard_pressed_mask, ~mask);
    }
    gb_update_controller_state();
}

static int gb_keyboard_bit_for_key(int code, uint32_t key)
{
    switch(key) {
    case LV_KEY_UP:
        return 0;
    case LV_KEY_DOWN:
        return 1;
    case LV_KEY_LEFT:
        return 2;
    case LV_KEY_RIGHT:
        return 3;
    case ' ':
        return 7;
    case LV_KEY_ENTER:
        return 7;
    default:
        break;
    }

    switch(code) {
    case 22:
    case 39:
        return 0;
    case 12:
    case 28:
        return 1;
    case 2:
    case 29:
        return 2;
    case 1:
    case 27:
        return 3;
    default:
        break;
    }

    if(key == 'w' || key == 'W') {
        return 0;
    }
    if(key == 's' || key == 'S') {
        return 1;
    }
    if(key == 'a' || key == 'A') {
        return 2;
    }
    if(key == 'd' || key == 'D') {
        return 3;
    }
    if(key == 'u' || key == 'U') {
        return 6;
    }
    if(key == 'i' || key == 'I') {
        return 4;
    }
    if(key == 'o' || key == 'O') {
        return 5;
    }
    return -1;
}

static void gb_hardware_key_cb(int code, uint32_t key, int pressed,
                                void *user_data)
{
    int bit;

    (void)user_data;
    if(code == 40 && pressed &&
       gb_game_panel && lv_obj_is_valid(gb_game_panel) &&
       !lv_obj_has_flag(gb_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        if(gb_emulator_running) {
            gb_request_stop();
            if(gb_game_status_label && lv_obj_is_valid(gb_game_status_label)) {
                lv_label_set_text(gb_game_status_label, "Stopping...");
            }
        } else {
            gb_show_game_view(0);
        }
        return;
    }

    bit = gb_keyboard_bit_for_key(code, key);
    if(bit < 0) {
        return;
    }
    gb_set_keyboard_bit((unsigned)bit, pressed);
}

static void gb_audio_close(void)
{
    if(gb_audio_pipe) {
        pclose(gb_audio_pipe);
        gb_audio_pipe = NULL;
    }
    free(gb_audio_buffer);
    gb_audio_buffer = NULL;
    gb_audio_frames = 0;
}

static int gb_audio_open(void)
{
    char cmd[512];

    if(gb_audio_pipe) {
        return 0;
    }
    if(!gb_audio_buffer) {
        gb_audio_buffer = (int16_t *)malloc(GBUI_AUDIO_MAX_FRAMES * 2U *
                                             sizeof(int16_t));
    }
    if(!gb_audio_buffer) {
        gb_log("Game Boy audio buffer allocation failed");
        return -1;
    }

    if(access(GBUI_PCM_VOLUME_BIN, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
                 GBUI_PCM_VOLUME_BIN " " GBUI_VOLUME_CONFIG_PATH
                 " 2>>" GBUI_AUDIO_LOG_PATH " | "
                 "aplay -q -D default -t raw -f S16_LE -c 2 -r %d "
                 "-B 50000 -F 10000 - >>" GBUI_AUDIO_LOG_PATH " 2>&1",
                 GBUI_AUDIO_RATE);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "aplay -q -D default -t raw -f S16_LE -c 2 -r %d "
                 "-B 50000 -F 10000 - >>" GBUI_AUDIO_LOG_PATH " 2>&1",
                 GBUI_AUDIO_RATE);
    }

    gb_audio_pipe = popen(cmd, "w");
    if(!gb_audio_pipe) {
        gb_log("Game Boy audio pipe open failed");
        return -1;
    }
    setvbuf(gb_audio_pipe, NULL, _IONBF, 0);
    gb_log("Game Boy audio pipe opened rate=%d", GBUI_AUDIO_RATE);
    return 0;
}

static void gb_stop_event(lv_event_t *event)
{
    (void)event;
    gb_request_stop();
    if(gb_game_status_label && lv_obj_is_valid(gb_game_status_label)) {
        lv_label_set_text(gb_game_status_label, "Stopping...");
    }
}

static void gb_publish_frame(void)
{
    pthread_mutex_lock(&gb_frame_mutex);
    if(gb_frame_back) {
        for(int y = 0; y < GBUI_VIEW_H; y++) {
            const uint32_t *src = gb_native_frame +
                ((y * GBUI_NATIVE_H) / GBUI_VIEW_H) * GBUI_NATIVE_W;
            uint16_t *dst = gb_frame_back + y * GBUI_VIEW_W;
            for(int x = 0; x < GBUI_VIEW_W; x++) {
                uint32_t rgb = src[(x * GBUI_NATIVE_W) / GBUI_VIEW_W];
                uint16_t pixel = (uint16_t)(((rgb >> 19) & 0x1FU) << 11 |
                                             ((rgb >> 10) & 0x3FU) << 5 |
                                             ((rgb >> 3) & 0x1FU));
                dst[x] = pixel;
            }
        }
        uint16_t *tmp = gb_frame_front;
        gb_frame_front = gb_frame_back;
        gb_frame_back = tmp;
        gb_frame_seq++;
    }
    pthread_mutex_unlock(&gb_frame_mutex);
}

static void gb_audio_sample_cb(GB_gameboy_t *core, GB_sample_t *sample)
{
    (void)core;
    if(!sample || !gb_audio_buffer || gb_audio_frames >= GBUI_AUDIO_MAX_FRAMES) {
        return;
    }
    gb_audio_buffer[gb_audio_frames * 2U] = sample->left;
    gb_audio_buffer[gb_audio_frames * 2U + 1U] = sample->right;
    gb_audio_frames++;
}

static void gb_submit_audio(void)
{
    size_t bytes;

    if(gb_audio_frames == 0 || !gb_audio_pipe) {
        gb_audio_frames = 0;
        return;
    }
    bytes = gb_audio_frames * 2U * sizeof(gb_audio_buffer[0]);
    if(fwrite(gb_audio_buffer, 1, bytes, gb_audio_pipe) != bytes) {
        gb_log("Game Boy audio write failed");
        gb_audio_close();
    }
    gb_audio_frames = 0;
}

static void gb_sync_input(GB_gameboy_t *core)
{
    uint32_t state = __atomic_load_n(&gb_controller_state, __ATOMIC_RELAXED);
    uint32_t pressed = ~state;
    GB_key_mask_t mask = (GB_key_mask_t)0;

    gb_input_old_state = state;
    if(pressed & (1U << 0)) mask = (GB_key_mask_t)(mask | GB_KEY_UP_MASK);
    if(pressed & (1U << 1)) mask = (GB_key_mask_t)(mask | GB_KEY_DOWN_MASK);
    if(pressed & (1U << 2)) mask = (GB_key_mask_t)(mask | GB_KEY_LEFT_MASK);
    if(pressed & (1U << 3)) mask = (GB_key_mask_t)(mask | GB_KEY_RIGHT_MASK);
    if(pressed & (1U << 4)) mask = (GB_key_mask_t)(mask | GB_KEY_B_MASK);
    if(pressed & (1U << 5)) mask = (GB_key_mask_t)(mask | GB_KEY_SELECT_MASK);
    if(pressed & (1U << 6)) mask = (GB_key_mask_t)(mask | GB_KEY_A_MASK);
    if(pressed & (1U << 7)) mask = (GB_key_mask_t)(mask | GB_KEY_START_MASK);
    GB_set_key_mask(core, mask);
}

static uint32_t gb_rgb_encode(GB_gameboy_t *core, uint8_t r, uint8_t g,
                              uint8_t b)
{
    (void)core;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void gb_boot_rom_load(GB_gameboy_t *core, GB_boot_rom_t type)
{
    if(type == GB_BOOT_ROM_DMG || type == GB_BOOT_ROM_DMG_0 ||
       type == GB_BOOT_ROM_MGB) {
        GB_load_boot_rom_from_buffer(core, sameboy_dmg_boot,
                                     sameboy_dmg_boot_len);
    } else {
        GB_load_boot_rom_from_buffer(core, sameboy_cgb_boot,
                                     sameboy_cgb_boot_len);
    }
}

static void gb_make_save_path(const char *rom_path, char *out, size_t out_len)
{
    char dir[GBUI_PATH_MAX];
    char stem[GBUI_TITLE_MAX];

    gb_split_dir_stem(rom_path, dir, sizeof(dir), stem, sizeof(stem));
    snprintf(out, out_len, "%s/%s.sav", dir, stem);
}

static void gb_save_battery(GB_gameboy_t *core, const char *save_path)
{
    char temp_path[GBUI_PATH_MAX + 8];

    if(GB_save_battery_size(core) <= 0) {
        return;
    }
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", save_path);
    if(GB_save_battery(core, temp_path) == 0 &&
       rename(temp_path, save_path) == 0) {
        gb_log("Saved battery and RTC data to %s", save_path);
    } else {
        unlink(temp_path);
        gb_log("Failed to save battery data to %s", save_path);
    }
}

static int gb_run_emulator(const char *rom_path)
{
    FILE *fp;
    uint8_t header[0x150];
    char save_path[GBUI_PATH_MAX];
    GB_gameboy_t *core = NULL;
    GB_model_t model;
    unsigned frame_count = 0;
    int result = -1;

    fp = fopen(rom_path, "rb");
    if(!fp || fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        if(fp) fclose(fp);
        gb_log("Failed to read ROM header from %s", rom_path);
        return -1;
    }
    fclose(fp);
    model = (header[0x143] & 0x80U) ? GB_MODEL_CGB_E : GB_MODEL_DMG_B;
    core = GB_init(GB_alloc(), model);
    if(!core) {
        gb_log("SameBoy core allocation failed");
        return -1;
    }
    GB_set_boot_rom_load_callback(core, gb_boot_rom_load);
    GB_set_rgb_encode_callback(core, gb_rgb_encode);
    GB_set_pixels_output(core, gb_native_frame);
    GB_set_sample_rate(core, GBUI_AUDIO_RATE);
    GB_apu_set_sample_callback(core, gb_audio_sample_cb);
    GB_set_rtc_mode(core, GB_RTC_MODE_SYNC_TO_HOST);
    if(GB_load_rom(core, rom_path) != 0) {
        gb_log("SameBoy rejected ROM %s", rom_path);
        goto cleanup;
    }
    GB_reset(core);
    gb_make_save_path(rom_path, save_path, sizeof(save_path));
    if(access(save_path, R_OK) == 0) {
        int load_result = GB_load_battery(core, save_path);
        gb_log("Loaded battery data from %s result=%d", save_path,
               load_result);
    }
    memset(gb_native_frame, 0, sizeof(gb_native_frame));
    if(gb_audio_open() != 0) {
        gb_log("Continuing without audio output");
    }
    gb_audio_frames = 0;
    result = 0;
    gb_log("Started %s model=%s save=%s", rom_path,
           model == GB_MODEL_CGB_E ? "CGB" : "DMG", save_path);

    while(!gb_stop_requested) {
        uint64_t frame_start = ui_monotonic_us();

        gb_sync_input(core);
        GB_run_frame(core);
        gb_publish_frame();
        gb_submit_audio();
        frame_count++;
        if(frame_count % 1800U == 0) {
            gb_save_battery(core, save_path);
        }

        uint64_t elapsed = ui_monotonic_us() - frame_start;
        uint64_t frame_us = 16743ULL;
        if(elapsed < frame_us) {
            usleep((useconds_t)(frame_us - elapsed));
        }
    }

cleanup:
    if(core) {
        if(result == 0) {
            gb_save_battery(core, save_path);
        }
        GB_free(core);
        GB_dealloc(core);
    }
    gb_audio_close();
    return result;
}

static void *gb_emulator_thread_main(void *arg)
{
    char rom_path[GBUI_PATH_MAX];

    (void)arg;
    snprintf(rom_path, sizeof(rom_path), "%s", gb_active_rom_path);

    gb_emulator_result = gb_run_emulator(rom_path);
    gb_emulator_running = 0;
    gb_emulator_finished = 1;
    return NULL;
}

static int gb_start_emulator(const char *path)
{
    char reason[128];

    if(!path || !path[0] || gb_emulator_running) {
        return 0;
    }
    if(!gb_validate_rom_for_start(path, reason, sizeof(reason))) {
        gb_set_status(reason, 0xEF4D5A);
        if(gb_game_status_label && lv_obj_is_valid(gb_game_status_label)) {
            lv_label_set_text(gb_game_status_label, reason);
        }
        gb_log("Rejected ROM %s: %s", path ? path : "(null)", reason);
        return 0;
    }
    ui_audio_stop_for_exclusive_app("Stopped for Game Boy");
    gb_log("Requested exclusive audio before ROM start");
    if(!gb_prepare_frame_buffers()) {
        gb_set_status("No RAM for Game Boy frame buffers", 0xEF4D5A);
        return 0;
    }

    snprintf(gb_active_rom_path, sizeof(gb_active_rom_path), "%s", path);
    gb_controller_state = 0xFFFFFFFFU;
    gb_input_old_state = 0xFFFFFFFFU;
    gb_lvgl_pressed_mask = 0;
    gb_touch_pressed_mask = 0;
    gb_keyboard_pressed_mask = 0;
    gb_stop_requested = 0;
    gb_emulator_finished = 0;
    gb_emulator_result = 0;

    if(gb_frame_image && lv_obj_is_valid(gb_frame_image)) {
        lv_image_set_src(gb_frame_image, &gb_frame_dsc);
        lv_obj_clear_flag(gb_frame_image, LV_OBJ_FLAG_HIDDEN);
    }
    if(gb_frame_placeholder && lv_obj_is_valid(gb_frame_placeholder)) {
        lv_obj_add_flag(gb_frame_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    if(gb_game_status_label && lv_obj_is_valid(gb_game_status_label)) {
        lv_label_set_text_fmt(gb_game_status_label, "Running %s",
                              gb_base_name(path));
    }

    gb_show_game_view(1);
    gb_emulator_running = 1;
    if(pthread_create(&gb_emulator_thread, NULL, gb_emulator_thread_main,
                      NULL) != 0) {
        gb_emulator_running = 0;
        gb_show_game_view(0);
        gb_set_status("Failed to start Game Boy thread", 0xEF4D5A);
        return 0;
    }
    gb_emulator_thread_valid = 1;
    gb_log("Started ROM %s", path);
    return 1;
}

static void gb_start_rom_event(lv_event_t *event)
{
    gb_rom_item_t *item = (gb_rom_item_t *)lv_event_get_user_data(event);
    if(!item) {
        return;
    }
    gb_update_selected_info((int)(item - gb_roms));
    gb_start_emulator(item->path);
}

static void gb_clear_list(void)
{
    if(gb_list_panel && lv_obj_is_valid(gb_list_panel)) {
        lv_obj_clean(gb_list_panel);
    }
}

static void gb_create_empty_row(const char *text)
{
    lv_obj_t *label;
    int panel_w;

    if(!gb_list_panel) {
        return;
    }
    panel_w = gb_list_cached_w > 0 ? gb_list_cached_w :
        lv_obj_get_width(gb_list_panel);
    panel_w -= 32;
    if(panel_w < 160) {
        panel_w = 160;
    }
    label = ui_label(gb_list_panel, text, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(label, panel_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(label, 16, 16);
}

static void gb_create_rom_row(int index)
{
    gb_rom_item_t *item = &gb_roms[index];
    lv_obj_t *row;
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *meta;
    lv_obj_t *size_label;
    char size_buf[32];
    char meta_text[GBUI_META_MAX];
    int panel_w = gb_list_cached_w > 0 ? gb_list_cached_w :
        (gb_list_panel ? lv_obj_get_width(gb_list_panel) : 520);
    int row_w = panel_w - 16;
    int title_w;

    if(row_w < 160) {
        row_w = 160;
    }
    title_w = row_w - 178;
    if(title_w < 160) {
        title_w = row_w - 68;
    }
    if(title_w < 96) {
        title_w = 96;
    }

    row = ui_panel(gb_list_panel, 8, 8 + index * 88, row_w, 78);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2028), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, gb_start_rom_event, LV_EVENT_CLICKED, item);

    icon = ui_label(row, item->cover_path[0] ? LV_SYMBOL_IMAGE : LV_SYMBOL_PLAY,
                    &lv_font_montserrat_22, item->valid_header ? 0x3DA5FF : 0xEF4D5A);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);

    title = ui_label(row, item->title, &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(title, title_w);
    lv_obj_set_height(title, 26);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(title, 52, 10);

    if(item->valid_header) {
        gb_format_rom_meta(item, ui_is_landscape(), meta_text,
                            sizeof(meta_text));
    } else {
        snprintf(meta_text, sizeof(meta_text), "%s", item->meta);
    }
    meta = ui_label(row, meta_text,
                    &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(meta, row_w - 72);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(meta, 52, 42);

    gb_format_size(size_buf, sizeof(size_buf), item->size);
    size_label = ui_label(row, size_buf, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_align(size_label, LV_ALIGN_RIGHT_MID, -14, 0);
}

static void gb_build_rom_list(void)
{
    char text[96];

    mkdir(GBUI_ROM_DIR_PRIMARY, 0755);
    gb_clear_list();
    memset(gb_roms, 0, sizeof(gb_roms));
    gb_rom_count = 0;
    gb_selected_index = -1;

    for(size_t i = 0; i < sizeof(gb_scan_dirs) / sizeof(gb_scan_dirs[0]); i++) {
        gb_scan_dir(gb_scan_dirs[i]);
    }

    for(int i = 0; i < gb_rom_count; i++) {
        gb_create_rom_row(i);
    }
    if(gb_count_label && lv_obj_is_valid(gb_count_label)) {
        lv_label_set_text_fmt(gb_count_label, "%d", gb_rom_count);
    }
    if(gb_rom_count == 0) {
        gb_create_empty_row("No ROM files found.");
        gb_set_status("No Game Boy ROM files found", 0x9AA4AF);
        gb_update_selected_info(-1);
    } else {
        snprintf(text, sizeof(text), "%d ROM%s found", gb_rom_count,
                 gb_rom_count == 1 ? "" : "s");
        gb_set_status(text, 0x25C281);
        gb_update_selected_info(0);
    }
    gb_log("Scan complete count=%d", gb_rom_count);
}

static void gb_scan_event(lv_event_t *event)
{
    (void)event;
    gb_build_rom_list();
}

static int gb_parent_body_height(lv_obj_t *parent)
{
    int h = parent ? lv_obj_get_height(parent) : 0;

    return h > 180 ? h : ui_body_height(144);
}

static int gb_clamp_int(int value, int min_value, int max_value)
{
    if(value < min_value) {
        return min_value;
    }
    if(value > max_value) {
        return max_value;
    }
    return value;
}

static void gb_ui_timer_cb(lv_timer_t *timer)
{
    uint32_t seq;

    (void)timer;
    gb_update_multitouch_controls();
    pthread_mutex_lock(&gb_frame_mutex);
    seq = gb_frame_seq;
    if(gb_frame_front && gb_frame_display && seq != gb_frame_seen_seq) {
        memcpy(gb_frame_display, gb_frame_front, GBUI_FRAME_BYTES);
        gb_frame_seen_seq = seq;
        pthread_mutex_unlock(&gb_frame_mutex);
        if(gb_frame_image && lv_obj_is_valid(gb_frame_image)) {
            lv_obj_invalidate(gb_frame_image);
        }
    } else {
        pthread_mutex_unlock(&gb_frame_mutex);
    }

    if(gb_emulator_finished) {
        char text[96];
        if(gb_emulator_thread_valid) {
            pthread_join(gb_emulator_thread, NULL);
            gb_emulator_thread_valid = 0;
        }
        gb_emulator_finished = 0;
        snprintf(text, sizeof(text), "Game Boy exited (%d)",
                 gb_emulator_result);
        gb_set_status(text, gb_emulator_result == 0 ? 0x25C281 : 0xEF4D5A);
        if(gb_game_status_label && lv_obj_is_valid(gb_game_status_label)) {
            lv_label_set_text(gb_game_status_label, text);
        }
        gb_show_game_view(0);
    }
}

static void gb_create_library(lv_obj_t *parent)
{
    lv_obj_t *cover;
    lv_obj_t *info;
    lv_obj_t *scan_btn;
    lv_obj_t *cover_icon;
    lv_obj_t *cover_text;
    lv_obj_t *folder_label;
    int landscape = ui_is_landscape();
    int body_h = gb_parent_body_height(parent);
    int screen_w = ui_screen_width();
    int panel_x = landscape ? 0 : ui_page_panel_x();
    int panel_w = landscape ? screen_w : ui_page_panel_width();
    int margin = landscape ? 24 : 0;
    int gap = landscape ? 24 : 16;
    int left_w = landscape ? gb_clamp_int(screen_w * 34 / 100, 320, 460) : panel_w;
    int min_list_w = landscape ? 360 : 420;
    int cover_x = margin;
    int cover_y = landscape ? 20 : 18;
    int cover_w = landscape ? left_w : panel_w;
    int cover_h = landscape ? gb_clamp_int(body_h * 42 / 100, 170, 260) :
        panel_w;
    int info_x = landscape ? margin : margin;
    int info_y = landscape ? 20 + cover_h + 16 : cover_y + cover_h + 12;
    int info_w = landscape ? left_w : panel_w;
    int info_h = landscape ? body_h - info_y - 20 :
        gb_clamp_int(body_h * 25 / 100, 204, 232);
    int list_x = landscape ? margin + left_w + gap : margin;
    int list_y = landscape ? 20 : info_y + info_h + 16;
    int list_w = landscape ? screen_w - list_x - margin : panel_w;
    int list_h = landscape ? body_h - 40 : body_h - list_y - 24;
    int cover_img_w = landscape ? cover_w - 32 : cover_w - 28;
    int cover_img_h = landscape ? cover_h - 32 : cover_img_w;
    int info_text_w = info_w - 32;
    int scan_btn_w = landscape ? 82 : 132;
    int scan_btn_h = landscape ? 46 : 46;
    int scan_btn_x;
    int scan_btn_y;
    int title_w;

    if(landscape && list_w < min_list_w) {
        left_w -= min_list_w - list_w;
        if(left_w < 280) {
            left_w = 280;
        }
        list_x = margin + left_w + gap;
        list_w = screen_w - list_x - margin;
        cover_w = left_w;
        cover_img_w = cover_w - 32;
        info_w = left_w;
        info_text_w = info_w - 32;
    }
    if(landscape && list_w < min_list_w) {
        list_w = min_list_w;
    }
    if(list_h < (landscape ? 260 : 220)) {
        list_h = landscape ? 260 : 220;
    }
    if(landscape && info_h < 136) {
        cover_h -= 136 - info_h;
        if(cover_h < 150) {
            cover_h = 150;
        }
        cover_img_h = cover_h - 32;
        info_y = 20 + cover_h + 16;
        info_h = body_h - info_y - 20;
    }
    if(info_h < 112) {
        info_h = 112;
    }
    scan_btn_x = landscape ? 8 : info_w - scan_btn_w - 24;
    scan_btn_y = landscape ? info_h - scan_btn_h - 28 :
        info_h - scan_btn_h - 38;
    title_w = info_text_w;
    if(title_w < 96) {
        title_w = 96;
    }

    gb_library_panel = ui_scroll_panel(parent, panel_x, 0, panel_w, body_h);
    lv_obj_set_style_bg_color(gb_library_panel, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(gb_library_panel, 0, 0);
    lv_obj_set_style_border_width(gb_library_panel, 0, 0);
    lv_obj_set_style_pad_all(gb_library_panel, 0, 0);
    lv_obj_set_style_pad_bottom(gb_library_panel, 48, 0);
    lv_obj_set_scrollbar_mode(gb_library_panel, LV_SCROLLBAR_MODE_OFF);
    if(landscape) {
        lv_obj_clear_flag(gb_library_panel, LV_OBJ_FLAG_SCROLLABLE);
    }

    cover = ui_panel(gb_library_panel, cover_x, cover_y, cover_w, cover_h);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x182331), 0);
    lv_obj_set_style_border_color(cover, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(cover, 0, 0);

    gb_cover_image = lv_image_create(cover);
    lv_obj_set_size(gb_cover_image, cover_img_w, cover_img_h);
    lv_obj_center(gb_cover_image);
    lv_image_set_inner_align(gb_cover_image, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(gb_cover_image, LV_OBJ_FLAG_HIDDEN);

    gb_cover_placeholder = lv_obj_create(cover);
    lv_obj_set_size(gb_cover_placeholder, cover_img_w, cover_img_h);
    lv_obj_center(gb_cover_placeholder);
    lv_obj_set_style_bg_color(gb_cover_placeholder, lv_color_hex(0x17202B), 0);
    lv_obj_set_style_radius(gb_cover_placeholder, 8, 0);
    lv_obj_set_style_border_width(gb_cover_placeholder, 0, 0);
    lv_obj_clear_flag(gb_cover_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    cover_icon = ui_label(gb_cover_placeholder, "GB", &lv_font_montserrat_32,
                          0x3DA5FF);
    lv_obj_align(cover_icon, LV_ALIGN_CENTER, 0, landscape ? -32 : -24);
    cover_text = ui_label(gb_cover_placeholder, "No Cover",
                          &lv_font_montserrat_18, 0xD3DAE3);
    lv_obj_align(cover_text, LV_ALIGN_CENTER, 0, landscape ? 34 : 24);

    info = ui_panel(gb_library_panel, info_x, info_y, info_w, info_h);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x182331), 0);
    lv_obj_set_style_border_color(info, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(info, 16, 0);
    gb_title_label = ui_label(info, "Game Boy", &lv_font_montserrat_26,
                              0xF2F5F8);
    lv_obj_set_width(gb_title_label, title_w);
    lv_obj_set_height(gb_title_label, 34);
    lv_label_set_long_mode(gb_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(gb_title_label, 0, 0);
    gb_meta_label = ui_label(info, "Scanning ROM library", &lv_font_montserrat_16,
                              0xD3DAE3);
    lv_obj_set_width(gb_meta_label, info_text_w);
    lv_label_set_long_mode(gb_meta_label,
                           landscape ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(gb_meta_label, 0, landscape ? 40 : 42);
    gb_path_label = ui_label(info, GBUI_ROM_DIR_PRIMARY, &lv_font_montserrat_14,
                              0x9AA4AF);
    lv_obj_set_width(gb_path_label, info_text_w);
    lv_label_set_long_mode(gb_path_label,
                           LV_LABEL_LONG_DOT);
    lv_obj_set_pos(gb_path_label, 0, landscape ? 62 : 78);

    scan_btn = gb_button(info, scan_btn_x,
                          scan_btn_y, scan_btn_w, scan_btn_h,
                          LV_SYMBOL_REFRESH, 0x3DA5FF);
    lv_obj_add_event_cb(scan_btn, gb_scan_event, LV_EVENT_CLICKED, NULL);
    folder_label = ui_label(info, "ROMs", &lv_font_montserrat_16, 0x9AA4AF);
    if(landscape) {
        lv_obj_align(folder_label, LV_ALIGN_BOTTOM_RIGHT, -52, -15);
    } else {
        lv_obj_set_pos(folder_label, 0, scan_btn_y + (scan_btn_h - 22) / 2);
    }
    gb_count_label = ui_label(info, "0", &lv_font_montserrat_20, 0x25C281);
    if(landscape) {
        lv_obj_align(gb_count_label, LV_ALIGN_BOTTOM_RIGHT, 0, -12);
    } else {
        lv_obj_align_to(gb_count_label, folder_label, LV_ALIGN_OUT_RIGHT_MID,
                        8, 1);
    }

    gb_status_label = ui_label(gb_library_panel, "",
                                &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(gb_status_label, landscape ? left_w - 32 : panel_w);
    lv_label_set_long_mode(gb_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(gb_status_label, landscape ? margin + 16 : 0,
                   landscape ? body_h - 48 : list_y - 34);
    lv_obj_add_flag(gb_status_label, LV_OBJ_FLAG_HIDDEN);

    gb_list_panel = ui_panel(gb_library_panel, list_x, list_y, list_w, list_h);
    gb_list_cached_w = list_w;
    lv_obj_set_style_bg_color(gb_list_panel, lv_color_hex(0x121820), 0);
    lv_obj_set_style_border_color(gb_list_panel, lv_color_hex(0x243244), 0);
    lv_obj_set_style_pad_all(gb_list_panel, 0, 0);
    lv_obj_add_flag(gb_list_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(gb_list_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(gb_list_panel, LV_SCROLLBAR_MODE_OFF);
}

static void gb_create_game(lv_obj_t *parent)
{
    lv_obj_t *frame_panel;
    int landscape = ui_is_landscape();
    int body_h = gb_parent_body_height(parent);
    int screen_w = ui_screen_width();
    int side_w = landscape ? gb_clamp_int(screen_w * 22 / 100, 230, 278) : 0;
    int frame_y = landscape ? 12 : 18;
    int frame_h = landscape ? body_h - 48 : 488;
    int frame_w = landscape ? (frame_h * GBUI_VIEW_W) / GBUI_VIEW_H : 520;
    int frame_x = landscape ? (screen_w - frame_w) / 2 : 24;
    uint32_t frame_scale = landscape ?
        (uint32_t)((frame_w * LV_SCALE_NONE) / GBUI_VIEW_W) : LV_SCALE_NONE;
    uint32_t frame_scale_h = landscape ?
        (uint32_t)((frame_h * LV_SCALE_NONE) / GBUI_VIEW_H) : LV_SCALE_NONE;

    if(landscape) {
        int max_frame_w = screen_w - side_w * 2 - 72;

        if(frame_h > body_h - 44) {
            frame_h = body_h - 44;
        }
        if(frame_h < 300) {
            frame_h = body_h > 320 ? body_h - 32 : 300;
        }
        frame_w = (frame_h * GBUI_VIEW_W) / GBUI_VIEW_H;
        if(frame_w > max_frame_w) {
            frame_w = max_frame_w;
            frame_h = (frame_w * GBUI_VIEW_H) / GBUI_VIEW_W;
        }
        if(frame_w < 320) {
            frame_w = 320;
            frame_h = (frame_w * GBUI_VIEW_H) / GBUI_VIEW_W;
        }
        frame_x = (screen_w - frame_w) / 2;
        if(frame_x < side_w + 24) {
            frame_x = side_w + 24;
        }
        if(frame_x + frame_w > screen_w - side_w - 24) {
            frame_x = screen_w - side_w - 24 - frame_w;
        }
        frame_y = (body_h - frame_h) / 2 - 10;
        if(frame_y < 10) {
            frame_y = 10;
        }
        frame_scale = (uint32_t)((frame_w * LV_SCALE_NONE) / GBUI_VIEW_W);
        frame_scale_h = (uint32_t)((frame_h * LV_SCALE_NONE) / GBUI_VIEW_H);
    }
    if(landscape && frame_scale_h < frame_scale) {
        frame_scale = frame_scale_h;
    }
    if(frame_scale == 0) {
        frame_scale = LV_SCALE_NONE;
    }

    gb_game_panel = ui_scroll_panel(parent, 0, 0, ui_screen_width(),
                                     body_h);
    lv_obj_set_style_bg_color(gb_game_panel, lv_color_hex(0x080B0F), 0);
    lv_obj_set_style_radius(gb_game_panel, 0, 0);
    lv_obj_set_style_border_width(gb_game_panel, 0, 0);
    lv_obj_add_flag(gb_game_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_scrollbar_mode(gb_game_panel, LV_SCROLLBAR_MODE_OFF);
    if(landscape) {
        lv_obj_set_style_pad_all(gb_game_panel, 0, 0);
        lv_obj_clear_flag(gb_game_panel, LV_OBJ_FLAG_SCROLLABLE);
    }

    frame_panel = ui_panel(gb_game_panel, frame_x, frame_y, frame_w, frame_h);
    lv_obj_set_style_bg_color(frame_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(frame_panel, lv_color_hex(0x253043), 0);
    lv_obj_set_style_pad_all(frame_panel, 0, 0);

    gb_frame_image = lv_image_create(frame_panel);
    lv_obj_set_size(gb_frame_image,
                    landscape ? (int)((GBUI_VIEW_W * frame_scale) / LV_SCALE_NONE) :
                    GBUI_VIEW_W,
                    landscape ? (int)((GBUI_VIEW_H * frame_scale) / LV_SCALE_NONE) :
                    GBUI_VIEW_H);
    lv_image_set_scale(gb_frame_image, frame_scale);
    lv_obj_center(gb_frame_image);
    lv_obj_add_flag(gb_frame_image, LV_OBJ_FLAG_HIDDEN);

    gb_frame_placeholder = ui_label(frame_panel, "GB", &lv_font_montserrat_32,
                                     0x3DA5FF);
    lv_obj_center(gb_frame_placeholder);

    gb_game_status_label = ui_label(gb_game_panel, "Ready",
                                     &lv_font_montserrat_16, 0xD3DAE3);
    lv_obj_set_width(gb_game_status_label, landscape ? frame_w : 380);
    lv_label_set_long_mode(gb_game_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(gb_game_status_label, landscape ? frame_x : 28,
                   landscape ? body_h - 34 : 520);

    if(landscape) {
        int margin = 18;
        int pad = body_h < 420 ? 68 : 76;
        int ab = body_h < 420 ? 88 : 98;
        int dpad_x = margin + (side_w - pad * 3) / 2;
        int dpad_y = frame_y + frame_h / 2 - pad / 2;
        int action_x = screen_w - margin - side_w +
                       (side_w - ab * 2 - 18) / 2;
        int action_y = frame_y + frame_h / 2 - ab / 2;
        int sys_w = (side_w - 14) / 2;
        int sys_h = 46;
        int sys_bottom_inset = body_h < 420 ? 24 : 44;
        int sys_y = body_h - sys_bottom_inset - sys_h;

        if(dpad_x < margin) {
            dpad_x = margin;
        }
        if(action_x < frame_x + frame_w + 16) {
            action_x = frame_x + frame_w + 16;
        }
        if(sys_w < 88) {
            sys_w = 88;
        }
        if(sys_y < dpad_y + pad * 2 + 12) {
            sys_y = dpad_y + pad * 2 + 12;
        }
        if(sys_y + sys_h > body_h - 8) {
            sys_y = body_h - sys_h - 8;
        }

        gb_add_control(gb_game_panel, dpad_x + pad, dpad_y - pad, pad, pad,
                        "Up", 0x1F2937, 0);
        gb_add_control(gb_game_panel, dpad_x + pad, dpad_y + pad, pad, pad,
                        "Down", 0x1F2937, 1);
        gb_add_control(gb_game_panel, dpad_x, dpad_y, pad, pad,
                        "Left", 0x1F2937, 2);
        gb_add_control(gb_game_panel, dpad_x + pad * 2, dpad_y, pad, pad,
                        "Right", 0x1F2937, 3);
        gb_add_control(gb_game_panel, action_x, action_y + ab / 3, ab, ab,
                        "B", 0x2563EB, 4);
        gb_add_control(gb_game_panel, action_x + ab + 18, action_y - ab / 4,
                        ab, ab, "A", 0xEF4D5A, 6);
        gb_add_control(gb_game_panel, margin, sys_y, sys_w, sys_h,
                        "Select", 0x14B8A6, 5);
        gb_add_control(gb_game_panel, margin + sys_w + 14, sys_y,
                        sys_w, sys_h, "Start", 0x374151, 7);
    } else {
        gb_add_control(gb_game_panel, 54, 690, 82, 82, "Up", 0x1F2937, 0);
        gb_add_control(gb_game_panel, 54, 874, 82, 82, "Down", 0x1F2937, 1);
        gb_add_control(gb_game_panel, 0, 782, 82, 82, "Left", 0x1F2937, 2);
        gb_add_control(gb_game_panel, 108, 782, 82, 82, "Right", 0x1F2937, 3);
        gb_add_control(gb_game_panel, 205, 875, 84, 58, "Select", 0x14B8A6, 5);
        gb_add_control(gb_game_panel, 300, 875, 84, 58, "Start", 0x374151, 7);
        gb_add_control(gb_game_panel, 355, 740, 94, 94, "B", 0x2563EB, 4);
        gb_add_control(gb_game_panel, 454, 685, 94, 94, "A", 0xEF4D5A, 6);
    }
}

void ui_gameboy_create(lv_obj_t *scr)
{
    ui_create_header(scr, "Game Boy");
    gb_body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(gb_body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(gb_body, 0, 0);
    lv_obj_set_style_border_width(gb_body, 0, 0);
    lv_obj_set_style_pad_all(gb_body, 0, 0);

    gb_create_library(gb_body);
    gb_create_game(gb_body);
    gb_build_rom_list();
    ui_extension_keyboard_set_key_cb(gb_hardware_key_cb, NULL);
    gb_log("Hardware keyboard mapped: arrows/WASD dpad, U/I/O A/B/Select, Enter Start, Esc stop/back");

    if(!gb_ui_timer) {
        gb_ui_timer = lv_timer_create(gb_ui_timer_cb, GBUI_REFRESH_TIMER_MS, NULL);
    }
}

void ui_gameboy_cleanup(void)
{
    gb_request_stop();
    if(gb_emulator_thread_valid) {
        pthread_join(gb_emulator_thread, NULL);
        gb_emulator_thread_valid = 0;
    }
    if(gb_ui_timer) {
        lv_timer_delete(gb_ui_timer);
        gb_ui_timer = NULL;
    }
    gb_audio_close();
    gb_free_frame_buffers();
    gb_lvgl_pressed_mask = 0;
    gb_touch_pressed_mask = 0;
    gb_keyboard_pressed_mask = 0;
    gb_controller_state = 0xFFFFFFFFU;
    ui_extension_keyboard_set_key_cb(NULL, NULL);
    gb_body = NULL;
    gb_library_panel = NULL;
    gb_game_panel = NULL;
    gb_list_panel = NULL;
    gb_status_label = NULL;
    gb_count_label = NULL;
    gb_title_label = NULL;
    gb_meta_label = NULL;
    gb_path_label = NULL;
    gb_cover_image = NULL;
    gb_cover_placeholder = NULL;
    gb_frame_image = NULL;
    gb_frame_placeholder = NULL;
    gb_game_status_label = NULL;
    memset(gb_control_btn, 0, sizeof(gb_control_btn));
}

int ui_gameboy_handle_back(void)
{
    if(!gb_game_panel || !lv_obj_is_valid(gb_game_panel) ||
       lv_obj_has_flag(gb_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }

    if(gb_emulator_running) {
        gb_request_stop();
        if(gb_game_status_label && lv_obj_is_valid(gb_game_status_label)) {
            lv_label_set_text(gb_game_status_label, "Stopping...");
        }
    } else {
        gb_show_game_view(0);
    }
    app_request_fast_refresh();
    return 1;
}
