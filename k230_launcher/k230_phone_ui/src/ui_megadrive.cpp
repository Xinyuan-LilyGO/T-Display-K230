#include "ui_megadrive.h"

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
#include "gwenesis.h"

extern unsigned char *ROM_DATA;
extern unsigned char *VRAM;
extern unsigned char gwenesis_vdp_regs[REG_SIZE];
extern unsigned short gwenesis_vdp_status;
extern int hint_pending;
extern int screen_height;
extern int screen_width;

int frame_counter;
int scan_line;
int system_clock;
int16_t gwenesis_sn76489_buffer[GWENESIS_AUDIO_BUFFER_LENGTH_PAL + 1];
int sn76489_clock;
int sn76489_index;
int16_t gwenesis_ym2612_buffer[GWENESIS_AUDIO_BUFFER_LENGTH_PAL + 1];
int ym2612_clock;
int ym2612_index;

void gwenesis_io_get_buttons(void)
{
}
}

#define MD_ROM_MAX 48
#define MD_PATH_MAX 256
#define MD_TITLE_MAX 96
#define MD_META_MAX 160
#define MD_ROM_DIR_PRIMARY "/root/megadrive"
#define MD_LOG_PATH "/tmp/k230_phone_megadrive.log"
#define MD_VIEW_W 512
#define MD_VIEW_H 384
#define MD_FRAME_BYTES (MD_VIEW_W * MD_VIEW_H * 2)
#define MD_NATIVE_W 320
#define MD_NATIVE_H 240
#define MD_REFRESH_TIMER_MS 16
#define MD_AUDIO_RATE 26633
#define MD_AUDIO_MAX_SAMPLES 600
#define MD_AUDIO_LOG_PATH "/tmp/k230_phone_megadrive_audio.log"
#define MD_PCM_VOLUME_BIN "/root/app/k230_phone_ui/k230_pcm_volume"
#define MD_VOLUME_CONFIG_PATH "/root/.k230_phone_audio_volume"

typedef struct {
    char path[MD_PATH_MAX];
    char title[MD_TITLE_MAX];
    char cover_path[MD_PATH_MAX];
    uint64_t size;
    uint32_t crc32;
    uint8_t valid_header;
    uint8_t smd;
    char format[12];
    char region[20];
    char product[24];
    char meta[MD_META_MAX];
} md_rom_item_t;

static const char *const md_scan_dirs[] = {
    MD_ROM_DIR_PRIMARY,
    "/root/roms/megadrive",
    "/mnt/sdcard/megadrive",
    "/mnt/megadrive",
    "/media/megadrive",
};

static md_rom_item_t md_roms[MD_ROM_MAX];
static int md_rom_count;
static int md_selected_index = -1;

static lv_timer_t *md_ui_timer;
static lv_obj_t *md_body;
static lv_obj_t *md_library_panel;
static lv_obj_t *md_game_panel;
static lv_obj_t *md_list_panel;
static lv_obj_t *md_status_label;
static lv_obj_t *md_count_label;
static lv_obj_t *md_title_label;
static lv_obj_t *md_meta_label;
static lv_obj_t *md_path_label;
static lv_obj_t *md_cover_image;
static lv_obj_t *md_cover_placeholder;
static lv_obj_t *md_frame_image;
static lv_obj_t *md_frame_placeholder;
static lv_obj_t *md_game_status_label;
static lv_obj_t *md_control_btn[8];
static int md_list_cached_w;

static pthread_t md_emulator_thread;
static int md_emulator_thread_valid;
static volatile int md_emulator_running;
static volatile int md_emulator_finished;
static volatile int md_emulator_result;
static volatile int md_stop_requested;
static uint32_t md_controller_state = 0xFFFFFFFFU;
static uint32_t md_input_old_state = 0xFFFFFFFFU;
static uint32_t md_lvgl_pressed_mask;
static uint32_t md_touch_pressed_mask;
static uint32_t md_keyboard_pressed_mask;
static char md_active_rom_path[MD_PATH_MAX];

static pthread_mutex_t md_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *md_frame_a;
static uint16_t *md_frame_b;
static uint16_t *md_frame_front;
static uint16_t *md_frame_back;
static uint16_t *md_frame_display;
static uint32_t md_frame_seq;
static uint32_t md_frame_seen_seq;
static lv_image_dsc_t md_frame_dsc;
static uint16_t md_native_frame[MD_NATIVE_W * MD_NATIVE_H];
static FILE *md_audio_pipe;
static int16_t *md_audio_buffer;

static void md_log(const char *fmt, ...)
{
    FILE *fp = fopen(MD_LOG_PATH, "a");
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

static int md_path_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

static uint64_t md_file_size(const char *path)
{
    struct stat st;
    if(!path || stat(path, &st) != 0) {
        return 0;
    }
    return (uint64_t)st.st_size;
}

static void md_format_size(char *out, size_t out_len, uint64_t bytes)
{
    if(bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if(bytes >= 1024ULL) {
        snprintf(out, out_len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    }
}

static const char *md_base_name(const char *path)
{
    const char *slash;

    if(!path) {
        return "";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int md_has_ext(const char *path, const char *ext)
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

static void md_split_dir_stem(const char *path, char *dir, size_t dir_len,
                               char *stem, size_t stem_len)
{
    const char *base = md_base_name(path);
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

static void md_make_title(const char *path, char *out, size_t out_len)
{
    char stem[MD_TITLE_MAX];

    md_split_dir_stem(path, NULL, 0, stem, sizeof(stem));
    for(size_t i = 0; stem[i]; i++) {
        if(stem[i] == '_' || stem[i] == '-') {
            stem[i] = ' ';
        }
    }
    snprintf(out, out_len, "%s", stem[0] ? stem : md_base_name(path));
}

static uint32_t md_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
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

static uint32_t md_crc32_file(const char *path)
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
            crc = md_crc32_update(crc, buf, got);
        }
        if(got < sizeof(buf)) {
            break;
        }
    }
    fclose(fp);
    return crc;
}

static void md_find_cover(md_rom_item_t *item)
{
    char dir[MD_PATH_MAX];
    char stem[MD_TITLE_MAX];
    const char *exts[] = {".png", ".jpg", ".jpeg", ".bmp"};

    item->cover_path[0] = '\0';
    md_split_dir_stem(item->path, dir, sizeof(dir), stem, sizeof(stem));
    for(size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        char path[MD_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s%s", dir, stem, exts[i]);
        if(md_path_exists(path)) {
            snprintf(item->cover_path, sizeof(item->cover_path), "%s", path);
            return;
        }
        snprintf(path, sizeof(path), "%s/covers/%s%s", dir, stem, exts[i]);
        if(md_path_exists(path)) {
            snprintf(item->cover_path, sizeof(item->cover_path), "%s", path);
            return;
        }
    }
}

static void md_format_rom_meta(const md_rom_item_t *item, int compact,
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

static void md_copy_header_text(char *out, size_t out_len,
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

static int md_parse_header(md_rom_item_t *item)
{
    FILE *fp;
    uint8_t h[0x200];
    size_t got;

    if(!item) {
        return 0;
    }
    if(item->size == 0) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "Empty ROM file");
        return 0;
    }
    if(item->size < 0x200 || item->size > MAX_ROM_SIZE + 0x200U) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta),
                 item->size < 0x200 ? "ROM file is too small" :
                 "ROM exceeds the 8 MB core limit");
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

    if(h[1] == 0x03 && h[8] == 0xAA && h[9] == 0xBB &&
       item->size > 0x200 && ((item->size - 0x200) % 0x4000) == 0) {
        item->valid_header = 1;
        item->smd = 1;
        snprintf(item->format, sizeof(item->format), "SMD");
        snprintf(item->region, sizeof(item->region), "Interleaved");
        md_format_rom_meta(item, 0, item->meta, sizeof(item->meta));
        return 1;
    }

    if(item->size > MAX_ROM_SIZE) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta),
                 "ROM exceeds the 8 MB core limit");
        return 0;
    }

    if(memcmp(h + 0x100, "SEGA", 4) != 0) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta),
                 "Missing SEGA cartridge header");
        return 0;
    }
    item->valid_header = 1;
    snprintf(item->format, sizeof(item->format), "BIN/MD");
    md_copy_header_text(item->product, sizeof(item->product), h + 0x180, 14);
    md_copy_header_text(item->region, sizeof(item->region), h + 0x1F0, 16);
    md_format_rom_meta(item, 0, item->meta, sizeof(item->meta));
    return 1;
}

static int md_validate_rom_for_start(const char *path, char *reason,
                                      size_t reason_len)
{
    struct stat st;
    md_rom_item_t item;

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
    if(!md_parse_header(&item)) {
        snprintf(reason, reason_len, "%s", item.meta);
        return 0;
    }
    return 1;
}

static int md_rom_exists(const char *path)
{
    for(int i = 0; i < md_rom_count; i++) {
        if(strcmp(md_roms[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void md_add_rom_path(const char *path)
{
    md_rom_item_t *item;

    if(!path || md_rom_count >= MD_ROM_MAX || md_rom_exists(path)) {
        return;
    }
    item = &md_roms[md_rom_count];
    memset(item, 0, sizeof(*item));
    snprintf(item->path, sizeof(item->path), "%s", path);
    item->size = md_file_size(path);
    md_make_title(path, item->title, sizeof(item->title));
    md_parse_header(item);
    item->crc32 = md_crc32_file(path);
    md_find_cover(item);
    md_rom_count++;
}

static void md_scan_dir(const char *dir)
{
    DIR *dp;
    struct dirent *ent;

    dp = opendir(dir);
    if(!dp) {
        return;
    }
    while((ent = readdir(dp)) != NULL && md_rom_count < MD_ROM_MAX) {
        char path[MD_PATH_MAX];
        struct stat st;

        if(ent->d_name[0] == '.') {
            continue;
        }
        if(!md_has_ext(ent->d_name, ".bin") &&
           !md_has_ext(ent->d_name, ".md") &&
           !md_has_ext(ent->d_name, ".gen") &&
           !md_has_ext(ent->d_name, ".smd")) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if(stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            md_add_rom_path(path);
        }
    }
    closedir(dp);
}

static lv_obj_t *md_button(lv_obj_t *parent, int x, int y, int w, int h,
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

static void md_set_status(const char *text, uint32_t color)
{
    if(md_status_label && lv_obj_is_valid(md_status_label)) {
        lv_label_set_text(md_status_label, text ? text : "");
        lv_obj_set_style_text_color(md_status_label, lv_color_hex(color), 0);
    }
}

static void md_update_cover(const md_rom_item_t *item)
{
    if(!md_cover_image || !md_cover_placeholder) {
        return;
    }
    if(item && item->cover_path[0]) {
        lv_image_set_src(md_cover_image, item->cover_path);
        lv_obj_clear_flag(md_cover_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(md_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(md_cover_image, NULL);
        lv_obj_add_flag(md_cover_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(md_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void md_update_selected_info(int index)
{
    char text[192];
    char size_buf[32];
    const md_rom_item_t *item = NULL;

    if(index >= 0 && index < md_rom_count) {
        item = &md_roms[index];
        md_selected_index = index;
    }

    if(md_title_label && lv_obj_is_valid(md_title_label)) {
        lv_label_set_text(md_title_label, item ? item->title : "No ROM selected");
    }
    if(md_meta_label && lv_obj_is_valid(md_meta_label)) {
        if(item && item->valid_header) {
            md_format_rom_meta(item, ui_is_landscape(), text, sizeof(text));
            lv_label_set_text(md_meta_label, text);
        } else {
            lv_label_set_text(md_meta_label,
                              item ? item->meta :
                              "Copy .bin, .md, .gen, or .smd files to /root/megadrive");
        }
    }
    if(md_path_label && lv_obj_is_valid(md_path_label)) {
        if(item) {
            md_format_size(size_buf, sizeof(size_buf), item->size);
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
            snprintf(text, sizeof(text), "%s", MD_ROM_DIR_PRIMARY);
        }
        lv_label_set_text(md_path_label, text);
    }
    md_update_cover(item);
}

static int md_prepare_frame_buffers(void)
{
    if(!md_frame_a) {
        md_frame_a = (uint16_t *)malloc(MD_FRAME_BYTES);
    }
    if(!md_frame_b) {
        md_frame_b = (uint16_t *)malloc(MD_FRAME_BYTES);
    }
    if(!md_frame_display) {
        md_frame_display = (uint16_t *)malloc(MD_FRAME_BYTES);
    }
    if(!md_frame_a || !md_frame_b || !md_frame_display) {
        return 0;
    }
    memset(md_frame_a, 0, MD_FRAME_BYTES);
    memset(md_frame_b, 0, MD_FRAME_BYTES);
    memset(md_frame_display, 0, MD_FRAME_BYTES);
    md_frame_front = md_frame_a;
    md_frame_back = md_frame_b;
    md_frame_seq = 0;
    md_frame_seen_seq = 0;

    memset(&md_frame_dsc, 0, sizeof(md_frame_dsc));
    md_frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    md_frame_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    md_frame_dsc.header.w = MD_VIEW_W;
    md_frame_dsc.header.h = MD_VIEW_H;
    md_frame_dsc.header.stride = MD_VIEW_W * 2;
    md_frame_dsc.data_size = MD_FRAME_BYTES;
    md_frame_dsc.data = (const uint8_t *)md_frame_display;
    return 1;
}

static void md_free_frame_buffers(void)
{
    pthread_mutex_lock(&md_frame_mutex);
    free(md_frame_a);
    free(md_frame_b);
    free(md_frame_display);
    md_frame_a = NULL;
    md_frame_b = NULL;
    md_frame_display = NULL;
    md_frame_front = NULL;
    md_frame_back = NULL;
    md_frame_seq = 0;
    md_frame_seen_seq = 0;
    pthread_mutex_unlock(&md_frame_mutex);
}

static void md_show_game_view(int show)
{
    if(md_library_panel && lv_obj_is_valid(md_library_panel)) {
        if(show) {
            lv_obj_add_flag(md_library_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(md_library_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(md_game_panel && lv_obj_is_valid(md_game_panel)) {
        if(show) {
            lv_obj_clear_flag(md_game_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(md_game_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void md_update_controller_state(void)
{
    uint32_t pressed_mask = (md_lvgl_pressed_mask |
                             md_touch_pressed_mask |
                             md_keyboard_pressed_mask) & 0xFFU;

    __atomic_store_n(&md_controller_state, ~pressed_mask, __ATOMIC_RELAXED);
}

static void md_set_controller_bit(unsigned bit, int pressed)
{
    uint32_t mask = 1U << bit;

    if(pressed) {
        __sync_fetch_and_or(&md_lvgl_pressed_mask, mask);
    } else {
        __sync_fetch_and_and(&md_lvgl_pressed_mask, ~mask);
    }
    md_update_controller_state();
}

static void md_set_touch_pressed_mask(uint32_t mask)
{
    mask &= 0xFFU;
    if(mask == md_touch_pressed_mask) {
        return;
    }
    md_touch_pressed_mask = mask;
    md_update_controller_state();
}

static int md_point_in_obj(const ui_touch_point_t *point, lv_obj_t *obj)
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

static void md_update_multitouch_controls(void)
{
    ui_touch_point_t points[UI_MULTITOUCH_MAX_POINTS];
    uint32_t count;
    uint32_t mask = 0;

    if(!md_game_panel || !lv_obj_is_valid(md_game_panel) ||
       lv_obj_has_flag(md_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        md_set_touch_pressed_mask(0);
        return;
    }

    count = ui_multitouch_get_points(points, UI_MULTITOUCH_MAX_POINTS);
    for(uint32_t i = 0; i < count && i < UI_MULTITOUCH_MAX_POINTS; i++) {
        for(unsigned bit = 0; bit < 8; bit++) {
            if(md_point_in_obj(&points[i], md_control_btn[bit])) {
                mask |= 1U << bit;
            }
        }
    }
    md_set_touch_pressed_mask(mask);
}

static void md_control_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    uintptr_t raw = (uintptr_t)lv_event_get_user_data(event);
    unsigned bit;

    if(raw == 0) {
        return;
    }
    bit = (unsigned)(raw - 1U);
    if(code == LV_EVENT_PRESSED) {
        md_set_controller_bit(bit, 1);
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        md_set_controller_bit(bit, 0);
    }
}

static void md_add_control(lv_obj_t *parent, int x, int y, int w, int h,
                            const char *text, uint32_t color, unsigned bit)
{
    lv_obj_t *btn = md_button(parent, x, y, w, h, text, color);

    if(bit < 8) {
        md_control_btn[bit] = btn;
    }
    lv_obj_add_event_cb(btn, md_control_event, LV_EVENT_PRESSED,
                        (void *)(uintptr_t)(bit + 1U));
    lv_obj_add_event_cb(btn, md_control_event, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)(bit + 1U));
    lv_obj_add_event_cb(btn, md_control_event, LV_EVENT_PRESS_LOST,
                        (void *)(uintptr_t)(bit + 1U));
}

static void md_request_stop(void)
{
    md_stop_requested = 1;
}

static void md_set_keyboard_bit(unsigned bit, int pressed)
{
    uint32_t mask;

    if(bit >= 8) {
        return;
    }
    mask = 1U << bit;
    if(pressed) {
        __sync_fetch_and_or(&md_keyboard_pressed_mask, mask);
    } else {
        __sync_fetch_and_and(&md_keyboard_pressed_mask, ~mask);
    }
    md_update_controller_state();
}

static int md_keyboard_bit_for_key(int code, uint32_t key)
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

static void md_hardware_key_cb(int code, uint32_t key, int pressed,
                                void *user_data)
{
    int bit;

    (void)user_data;
    if(code == 40 && pressed &&
       md_game_panel && lv_obj_is_valid(md_game_panel) &&
       !lv_obj_has_flag(md_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        if(md_emulator_running) {
            md_request_stop();
            if(md_game_status_label && lv_obj_is_valid(md_game_status_label)) {
                lv_label_set_text(md_game_status_label, "Stopping...");
            }
        } else {
            md_show_game_view(0);
        }
        return;
    }

    bit = md_keyboard_bit_for_key(code, key);
    if(bit < 0) {
        return;
    }
    md_set_keyboard_bit((unsigned)bit, pressed);
}

static void md_audio_close(void)
{
    if(md_audio_pipe) {
        pclose(md_audio_pipe);
        md_audio_pipe = NULL;
    }
    free(md_audio_buffer);
    md_audio_buffer = NULL;
}

static int md_audio_open(void)
{
    char cmd[512];

    if(md_audio_pipe) {
        return 0;
    }
    if(!md_audio_buffer) {
        md_audio_buffer = (int16_t *)malloc(MD_AUDIO_MAX_SAMPLES *
                                             sizeof(int16_t));
    }
    if(!md_audio_buffer) {
        md_log("Mega Drive audio buffer allocation failed");
        return -1;
    }

    if(access(MD_PCM_VOLUME_BIN, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
                 MD_PCM_VOLUME_BIN " " MD_VOLUME_CONFIG_PATH
                 " 2>>" MD_AUDIO_LOG_PATH " | "
                 "aplay -q -D default -t raw -f S16_LE -c 1 -r %d "
                 "-B 50000 -F 10000 - >>" MD_AUDIO_LOG_PATH " 2>&1",
                 MD_AUDIO_RATE);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "aplay -q -D default -t raw -f S16_LE -c 1 -r %d "
                 "-B 50000 -F 10000 - >>" MD_AUDIO_LOG_PATH " 2>&1",
                 MD_AUDIO_RATE);
    }

    md_audio_pipe = popen(cmd, "w");
    if(!md_audio_pipe) {
        md_log("Mega Drive audio pipe open failed");
        return -1;
    }
    setvbuf(md_audio_pipe, NULL, _IONBF, 0);
    md_log("Mega Drive audio pipe opened rate=%d", MD_AUDIO_RATE);
    return 0;
}

static void md_stop_event(lv_event_t *event)
{
    (void)event;
    md_request_stop();
    if(md_game_status_label && lv_obj_is_valid(md_game_status_label)) {
        lv_label_set_text(md_game_status_label, "Stopping...");
    }
}

static void md_publish_frame(void)
{
    pthread_mutex_lock(&md_frame_mutex);
    if(md_frame_back) {
        for(int y = 0; y < MD_VIEW_H; y++) {
            const uint16_t *src = md_native_frame +
                ((y * MD_NATIVE_H) / MD_VIEW_H) * MD_NATIVE_W;
            uint16_t *dst = md_frame_back + y * MD_VIEW_W;
            for(int x = 0; x < MD_VIEW_W; x++) {
                dst[x] = src[(x * MD_NATIVE_W) / MD_VIEW_W];
            }
        }
        uint16_t *tmp = md_frame_front;
        md_frame_front = md_frame_back;
        md_frame_back = tmp;
        md_frame_seq++;
    }
    pthread_mutex_unlock(&md_frame_mutex);
}

static void md_submit_audio(void)
{
    int available = ym2612_index;
    int samples;

    if(sn76489_index < available) {
        available = sn76489_index;
    }
    samples = available / 2;
    if(samples > MD_AUDIO_MAX_SAMPLES) {
        samples = MD_AUDIO_MAX_SAMPLES;
    }
    if(samples <= 0 || md_audio_open() != 0 || !md_audio_pipe) {
        return;
    }

    for(int i = 0; i < samples; i++) {
        int mixed = (int)gwenesis_ym2612_buffer[i] +
                    (int)gwenesis_sn76489_buffer[i];
        if(mixed > INT16_MAX) {
            mixed = INT16_MAX;
        } else if(mixed < INT16_MIN) {
            mixed = INT16_MIN;
        }
        md_audio_buffer[i] = (int16_t)mixed;
    }
    size_t bytes = (size_t)samples * sizeof(md_audio_buffer[0]);
    if(fwrite(md_audio_buffer, 1, bytes, md_audio_pipe) != bytes) {
        md_log("Mega Drive audio write failed");
        md_audio_close();
    }
}

static void md_sync_input(void)
{
    uint32_t state = __atomic_load_n(&md_controller_state, __ATOMIC_RELAXED);
    uint32_t changed = state ^ md_input_old_state;

    if(!changed) {
        return;
    }
    md_input_old_state = state;
    for(unsigned bit = 0; bit < 8; bit++) {
        if(changed & (1U << bit)) {
            if(state & (1U << bit)) {
                gwenesis_io_pad_release_button(0, (int)bit);
            } else {
                gwenesis_io_pad_press_button(0, (int)bit);
            }
        }
    }
}

static int md_run_emulator(const char *rom_path)
{
    FILE *fp = NULL;
    uint8_t *rom_data = NULL;
    struct stat st;
    int result = -1;

    if(stat(rom_path, &st) != 0 || st.st_size <= 0 ||
       st.st_size > (off_t)(MAX_ROM_SIZE + 0x200U)) {
        md_log("Invalid ROM size for %s", rom_path);
        return -1;
    }
    fp = fopen(rom_path, "rb");
    rom_data = (uint8_t *)malloc((size_t)st.st_size);
    VRAM = (unsigned char *)malloc(VRAM_MAX_SIZE);
    if(!fp || !rom_data || !VRAM ||
       fread(rom_data, 1, (size_t)st.st_size, fp) != (size_t)st.st_size) {
        md_log("Failed to load ROM %s", rom_path);
        goto cleanup;
    }
    fclose(fp);
    fp = NULL;

    memset(VRAM, 0, VRAM_MAX_SIZE);
    memset(md_native_frame, 0, sizeof(md_native_frame));
    load_cartridge(rom_data, (size_t)st.st_size);
    rom_data = NULL;
    power_on();
    reset_emulation();
    md_audio_open();
    result = 0;

    while(!md_stop_requested) {
        uint64_t frame_start = ui_monotonic_us();
        int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL :
            LINES_PER_FRAME_NTSC;
        int refresh_rate = REG1_PAL ? GWENESIS_REFRESH_RATE_PAL :
            GWENESIS_REFRESH_RATE_NTSC;
        int hint_counter = gwenesis_vdp_regs[10];

        md_sync_input();
        screen_width = REG12_MODE_H40 ? 320 : 256;
        screen_height = REG1_PAL ? 240 : 224;
        memset(md_native_frame, 0, sizeof(md_native_frame));
        gwenesis_vdp_set_buffer(md_native_frame);
        gwenesis_vdp_render_config();

        system_clock = 0;
        zclk = 0;
        ym2612_clock = 0;
        ym2612_index = 0;
        sn76489_clock = 0;
        sn76489_index = 0;
        scan_line = 0;

        while(scan_line < lines_per_frame && !md_stop_requested) {
            int target = system_clock + VDP_CYCLES_PER_LINE;
            m68k_run((unsigned int)target);
            z80_run(target);
            if(GWENESIS_AUDIO_ACCURATE == 0) {
                gwenesis_SN76489_run(target);
                ym2612_run(target);
            }

            if(scan_line < screen_height) {
                gwenesis_vdp_render_line(scan_line);
            }
            if(scan_line == 0 || scan_line > screen_height) {
                hint_counter = REG10_LINE_COUNTER;
            }
            if(--hint_counter < 0) {
                if(REG0_LINE_INTERRUPT && scan_line <= screen_height) {
                    hint_pending = 1;
                    if((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0) {
                        m68k_update_irq(4);
                    }
                }
                hint_counter = REG10_LINE_COUNTER;
            }

            scan_line++;
            if(scan_line == screen_height) {
                if(REG1_VBLANK_INTERRUPT) {
                    gwenesis_vdp_status |= STATUS_VIRQPENDING;
                    gwenesis_m68k_set_irq(6);
                }
                z80_irq_line(1);
            } else if(scan_line == screen_height + 1) {
                z80_irq_line(0);
            }
            system_clock = target;
        }

        if(GWENESIS_AUDIO_ACCURATE == 1) {
            gwenesis_SN76489_run(system_clock);
            ym2612_run(system_clock);
        }
        gwenesis_m68k_end_frame((unsigned int)system_clock);
        md_publish_frame();
        md_submit_audio();
        frame_counter++;

        uint64_t elapsed = ui_monotonic_us() - frame_start;
        uint64_t frame_us = 1000000ULL / (uint64_t)refresh_rate;
        if(elapsed < frame_us) {
            usleep((useconds_t)(frame_us - elapsed));
        }
    }

cleanup:
    if(fp) {
        fclose(fp);
    }
    free(rom_data);
    free(ROM_DATA);
    ROM_DATA = NULL;
    free(VRAM);
    VRAM = NULL;
    md_audio_close();
    return result;
}

static void *md_emulator_thread_main(void *arg)
{
    char rom_path[MD_PATH_MAX];

    (void)arg;
    snprintf(rom_path, sizeof(rom_path), "%s", md_active_rom_path);

    md_emulator_result = md_run_emulator(rom_path);
    md_emulator_running = 0;
    md_emulator_finished = 1;
    return NULL;
}

static int md_start_emulator(const char *path)
{
    char reason[128];

    if(!path || !path[0] || md_emulator_running) {
        return 0;
    }
    if(!md_validate_rom_for_start(path, reason, sizeof(reason))) {
        md_set_status(reason, 0xEF4D5A);
        if(md_game_status_label && lv_obj_is_valid(md_game_status_label)) {
            lv_label_set_text(md_game_status_label, reason);
        }
        md_log("Rejected ROM %s: %s", path ? path : "(null)", reason);
        return 0;
    }
    ui_audio_stop_for_exclusive_app("Stopped for Mega Drive");
    md_log("Requested exclusive audio before ROM start");
    if(!md_prepare_frame_buffers()) {
        md_set_status("No RAM for Mega Drive frame buffers", 0xEF4D5A);
        return 0;
    }

    snprintf(md_active_rom_path, sizeof(md_active_rom_path), "%s", path);
    md_controller_state = 0xFFFFFFFFU;
    md_input_old_state = 0xFFFFFFFFU;
    md_lvgl_pressed_mask = 0;
    md_touch_pressed_mask = 0;
    md_keyboard_pressed_mask = 0;
    md_stop_requested = 0;
    md_emulator_finished = 0;
    md_emulator_result = 0;

    if(md_frame_image && lv_obj_is_valid(md_frame_image)) {
        lv_image_set_src(md_frame_image, &md_frame_dsc);
        lv_obj_clear_flag(md_frame_image, LV_OBJ_FLAG_HIDDEN);
    }
    if(md_frame_placeholder && lv_obj_is_valid(md_frame_placeholder)) {
        lv_obj_add_flag(md_frame_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    if(md_game_status_label && lv_obj_is_valid(md_game_status_label)) {
        lv_label_set_text_fmt(md_game_status_label, "Running %s",
                              md_base_name(path));
    }

    md_show_game_view(1);
    md_emulator_running = 1;
    if(pthread_create(&md_emulator_thread, NULL, md_emulator_thread_main,
                      NULL) != 0) {
        md_emulator_running = 0;
        md_show_game_view(0);
        md_set_status("Failed to start Mega Drive thread", 0xEF4D5A);
        return 0;
    }
    md_emulator_thread_valid = 1;
    md_log("Started ROM %s", path);
    return 1;
}

static void md_start_rom_event(lv_event_t *event)
{
    md_rom_item_t *item = (md_rom_item_t *)lv_event_get_user_data(event);
    if(!item) {
        return;
    }
    md_update_selected_info((int)(item - md_roms));
    md_start_emulator(item->path);
}

static void md_clear_list(void)
{
    if(md_list_panel && lv_obj_is_valid(md_list_panel)) {
        lv_obj_clean(md_list_panel);
    }
}

static void md_create_empty_row(const char *text)
{
    lv_obj_t *label;
    int panel_w;

    if(!md_list_panel) {
        return;
    }
    panel_w = md_list_cached_w > 0 ? md_list_cached_w :
        lv_obj_get_width(md_list_panel);
    panel_w -= 32;
    if(panel_w < 160) {
        panel_w = 160;
    }
    label = ui_label(md_list_panel, text, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(label, panel_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(label, 16, 16);
}

static void md_create_rom_row(int index)
{
    md_rom_item_t *item = &md_roms[index];
    lv_obj_t *row;
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *meta;
    lv_obj_t *size_label;
    char size_buf[32];
    char meta_text[MD_META_MAX];
    int panel_w = md_list_cached_w > 0 ? md_list_cached_w :
        (md_list_panel ? lv_obj_get_width(md_list_panel) : 520);
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

    row = ui_panel(md_list_panel, 8, 8 + index * 88, row_w, 78);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2028), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, md_start_rom_event, LV_EVENT_CLICKED, item);

    icon = ui_label(row, item->cover_path[0] ? LV_SYMBOL_IMAGE : LV_SYMBOL_PLAY,
                    &lv_font_montserrat_22, item->valid_header ? 0x3DA5FF : 0xEF4D5A);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);

    title = ui_label(row, item->title, &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(title, title_w);
    lv_obj_set_height(title, 26);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(title, 52, 10);

    if(item->valid_header) {
        md_format_rom_meta(item, ui_is_landscape(), meta_text,
                            sizeof(meta_text));
    } else {
        snprintf(meta_text, sizeof(meta_text), "%s", item->meta);
    }
    meta = ui_label(row, meta_text,
                    &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(meta, row_w - 72);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(meta, 52, 42);

    md_format_size(size_buf, sizeof(size_buf), item->size);
    size_label = ui_label(row, size_buf, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_align(size_label, LV_ALIGN_RIGHT_MID, -14, 0);
}

static void md_build_rom_list(void)
{
    char text[96];

    mkdir(MD_ROM_DIR_PRIMARY, 0755);
    md_clear_list();
    memset(md_roms, 0, sizeof(md_roms));
    md_rom_count = 0;
    md_selected_index = -1;

    for(size_t i = 0; i < sizeof(md_scan_dirs) / sizeof(md_scan_dirs[0]); i++) {
        md_scan_dir(md_scan_dirs[i]);
    }

    for(int i = 0; i < md_rom_count; i++) {
        md_create_rom_row(i);
    }
    if(md_count_label && lv_obj_is_valid(md_count_label)) {
        lv_label_set_text_fmt(md_count_label, "%d", md_rom_count);
    }
    if(md_rom_count == 0) {
        md_create_empty_row("No ROM files found.");
        md_set_status("No Mega Drive ROM files found", 0x9AA4AF);
        md_update_selected_info(-1);
    } else {
        snprintf(text, sizeof(text), "%d ROM%s found", md_rom_count,
                 md_rom_count == 1 ? "" : "s");
        md_set_status(text, 0x25C281);
        md_update_selected_info(0);
    }
    md_log("Scan complete count=%d", md_rom_count);
}

static void md_scan_event(lv_event_t *event)
{
    (void)event;
    md_build_rom_list();
}

static int md_parent_body_height(lv_obj_t *parent)
{
    int h = parent ? lv_obj_get_height(parent) : 0;

    return h > 180 ? h : ui_body_height(144);
}

static int md_clamp_int(int value, int min_value, int max_value)
{
    if(value < min_value) {
        return min_value;
    }
    if(value > max_value) {
        return max_value;
    }
    return value;
}

static void md_ui_timer_cb(lv_timer_t *timer)
{
    uint32_t seq;

    (void)timer;
    md_update_multitouch_controls();
    pthread_mutex_lock(&md_frame_mutex);
    seq = md_frame_seq;
    if(md_frame_front && md_frame_display && seq != md_frame_seen_seq) {
        memcpy(md_frame_display, md_frame_front, MD_FRAME_BYTES);
        md_frame_seen_seq = seq;
        pthread_mutex_unlock(&md_frame_mutex);
        if(md_frame_image && lv_obj_is_valid(md_frame_image)) {
            lv_obj_invalidate(md_frame_image);
        }
    } else {
        pthread_mutex_unlock(&md_frame_mutex);
    }

    if(md_emulator_finished) {
        char text[96];
        if(md_emulator_thread_valid) {
            pthread_join(md_emulator_thread, NULL);
            md_emulator_thread_valid = 0;
        }
        md_emulator_finished = 0;
        snprintf(text, sizeof(text), "Mega Drive exited (%d)",
                 md_emulator_result);
        md_set_status(text, md_emulator_result == 0 ? 0x25C281 : 0xEF4D5A);
        if(md_game_status_label && lv_obj_is_valid(md_game_status_label)) {
            lv_label_set_text(md_game_status_label, text);
        }
        md_show_game_view(0);
    }
}

static void md_create_library(lv_obj_t *parent)
{
    lv_obj_t *cover;
    lv_obj_t *info;
    lv_obj_t *scan_btn;
    lv_obj_t *cover_icon;
    lv_obj_t *cover_text;
    lv_obj_t *folder_label;
    int landscape = ui_is_landscape();
    int body_h = md_parent_body_height(parent);
    int screen_w = ui_screen_width();
    int panel_x = landscape ? 0 : ui_page_panel_x();
    int panel_w = landscape ? screen_w : ui_page_panel_width();
    int margin = landscape ? 24 : 0;
    int gap = landscape ? 24 : 16;
    int left_w = landscape ? md_clamp_int(screen_w * 34 / 100, 320, 460) : panel_w;
    int min_list_w = landscape ? 360 : 420;
    int cover_x = margin;
    int cover_y = landscape ? 20 : 18;
    int cover_w = landscape ? left_w : panel_w;
    int cover_h = landscape ? md_clamp_int(body_h * 42 / 100, 170, 260) :
        panel_w;
    int info_x = landscape ? margin : margin;
    int info_y = landscape ? 20 + cover_h + 16 : cover_y + cover_h + 12;
    int info_w = landscape ? left_w : panel_w;
    int info_h = landscape ? body_h - info_y - 20 :
        md_clamp_int(body_h * 25 / 100, 204, 232);
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

    md_library_panel = ui_scroll_panel(parent, panel_x, 0, panel_w, body_h);
    lv_obj_set_style_bg_color(md_library_panel, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(md_library_panel, 0, 0);
    lv_obj_set_style_border_width(md_library_panel, 0, 0);
    lv_obj_set_style_pad_all(md_library_panel, 0, 0);
    lv_obj_set_style_pad_bottom(md_library_panel, 48, 0);
    lv_obj_set_scrollbar_mode(md_library_panel, LV_SCROLLBAR_MODE_OFF);
    if(landscape) {
        lv_obj_clear_flag(md_library_panel, LV_OBJ_FLAG_SCROLLABLE);
    }

    cover = ui_panel(md_library_panel, cover_x, cover_y, cover_w, cover_h);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x182331), 0);
    lv_obj_set_style_border_color(cover, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(cover, 0, 0);

    md_cover_image = lv_image_create(cover);
    lv_obj_set_size(md_cover_image, cover_img_w, cover_img_h);
    lv_obj_center(md_cover_image);
    lv_image_set_inner_align(md_cover_image, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(md_cover_image, LV_OBJ_FLAG_HIDDEN);

    md_cover_placeholder = lv_obj_create(cover);
    lv_obj_set_size(md_cover_placeholder, cover_img_w, cover_img_h);
    lv_obj_center(md_cover_placeholder);
    lv_obj_set_style_bg_color(md_cover_placeholder, lv_color_hex(0x17202B), 0);
    lv_obj_set_style_radius(md_cover_placeholder, 8, 0);
    lv_obj_set_style_border_width(md_cover_placeholder, 0, 0);
    lv_obj_clear_flag(md_cover_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    cover_icon = ui_label(md_cover_placeholder, "MD", &lv_font_montserrat_32,
                          0x3DA5FF);
    lv_obj_align(cover_icon, LV_ALIGN_CENTER, 0, landscape ? -32 : -24);
    cover_text = ui_label(md_cover_placeholder, "No Cover",
                          &lv_font_montserrat_18, 0xD3DAE3);
    lv_obj_align(cover_text, LV_ALIGN_CENTER, 0, landscape ? 34 : 24);

    info = ui_panel(md_library_panel, info_x, info_y, info_w, info_h);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x182331), 0);
    lv_obj_set_style_border_color(info, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(info, 16, 0);
    md_title_label = ui_label(info, "Mega Drive", &lv_font_montserrat_26,
                              0xF2F5F8);
    lv_obj_set_width(md_title_label, title_w);
    lv_obj_set_height(md_title_label, 34);
    lv_label_set_long_mode(md_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(md_title_label, 0, 0);
    md_meta_label = ui_label(info, "Scanning ROM library", &lv_font_montserrat_16,
                              0xD3DAE3);
    lv_obj_set_width(md_meta_label, info_text_w);
    lv_label_set_long_mode(md_meta_label,
                           landscape ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(md_meta_label, 0, landscape ? 40 : 42);
    md_path_label = ui_label(info, MD_ROM_DIR_PRIMARY, &lv_font_montserrat_14,
                              0x9AA4AF);
    lv_obj_set_width(md_path_label, info_text_w);
    lv_label_set_long_mode(md_path_label,
                           LV_LABEL_LONG_DOT);
    lv_obj_set_pos(md_path_label, 0, landscape ? 62 : 78);

    scan_btn = md_button(info, scan_btn_x,
                          scan_btn_y, scan_btn_w, scan_btn_h,
                          LV_SYMBOL_REFRESH, 0x3DA5FF);
    lv_obj_add_event_cb(scan_btn, md_scan_event, LV_EVENT_CLICKED, NULL);
    folder_label = ui_label(info, "ROMs", &lv_font_montserrat_16, 0x9AA4AF);
    if(landscape) {
        lv_obj_align(folder_label, LV_ALIGN_BOTTOM_RIGHT, -52, -15);
    } else {
        lv_obj_set_pos(folder_label, 0, scan_btn_y + (scan_btn_h - 22) / 2);
    }
    md_count_label = ui_label(info, "0", &lv_font_montserrat_20, 0x25C281);
    if(landscape) {
        lv_obj_align(md_count_label, LV_ALIGN_BOTTOM_RIGHT, 0, -12);
    } else {
        lv_obj_align_to(md_count_label, folder_label, LV_ALIGN_OUT_RIGHT_MID,
                        8, 1);
    }

    md_status_label = ui_label(md_library_panel, "",
                                &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(md_status_label, landscape ? left_w - 32 : panel_w);
    lv_label_set_long_mode(md_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(md_status_label, landscape ? margin + 16 : 0,
                   landscape ? body_h - 48 : list_y - 34);
    lv_obj_add_flag(md_status_label, LV_OBJ_FLAG_HIDDEN);

    md_list_panel = ui_panel(md_library_panel, list_x, list_y, list_w, list_h);
    md_list_cached_w = list_w;
    lv_obj_set_style_bg_color(md_list_panel, lv_color_hex(0x121820), 0);
    lv_obj_set_style_border_color(md_list_panel, lv_color_hex(0x243244), 0);
    lv_obj_set_style_pad_all(md_list_panel, 0, 0);
    lv_obj_add_flag(md_list_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(md_list_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(md_list_panel, LV_SCROLLBAR_MODE_OFF);
}

static void md_create_game(lv_obj_t *parent)
{
    lv_obj_t *frame_panel;
    int landscape = ui_is_landscape();
    int body_h = md_parent_body_height(parent);
    int screen_w = ui_screen_width();
    int side_w = landscape ? md_clamp_int(screen_w * 22 / 100, 230, 278) : 0;
    int frame_y = landscape ? 12 : 18;
    int frame_h = landscape ? body_h - 48 : 488;
    int frame_w = landscape ? (frame_h * MD_VIEW_W) / MD_VIEW_H : 520;
    int frame_x = landscape ? (screen_w - frame_w) / 2 : 24;
    uint32_t frame_scale = landscape ?
        (uint32_t)((frame_w * LV_SCALE_NONE) / MD_VIEW_W) : LV_SCALE_NONE;
    uint32_t frame_scale_h = landscape ?
        (uint32_t)((frame_h * LV_SCALE_NONE) / MD_VIEW_H) : LV_SCALE_NONE;

    if(landscape) {
        int max_frame_w = screen_w - side_w * 2 - 72;

        if(frame_h > body_h - 44) {
            frame_h = body_h - 44;
        }
        if(frame_h < 300) {
            frame_h = body_h > 320 ? body_h - 32 : 300;
        }
        frame_w = (frame_h * MD_VIEW_W) / MD_VIEW_H;
        if(frame_w > max_frame_w) {
            frame_w = max_frame_w;
            frame_h = (frame_w * MD_VIEW_H) / MD_VIEW_W;
        }
        if(frame_w < 320) {
            frame_w = 320;
            frame_h = (frame_w * MD_VIEW_H) / MD_VIEW_W;
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
        frame_scale = (uint32_t)((frame_w * LV_SCALE_NONE) / MD_VIEW_W);
        frame_scale_h = (uint32_t)((frame_h * LV_SCALE_NONE) / MD_VIEW_H);
    }
    if(landscape && frame_scale_h < frame_scale) {
        frame_scale = frame_scale_h;
    }
    if(frame_scale == 0) {
        frame_scale = LV_SCALE_NONE;
    }

    md_game_panel = ui_scroll_panel(parent, 0, 0, ui_screen_width(),
                                     body_h);
    lv_obj_set_style_bg_color(md_game_panel, lv_color_hex(0x080B0F), 0);
    lv_obj_set_style_radius(md_game_panel, 0, 0);
    lv_obj_set_style_border_width(md_game_panel, 0, 0);
    lv_obj_add_flag(md_game_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_scrollbar_mode(md_game_panel, LV_SCROLLBAR_MODE_OFF);
    if(landscape) {
        lv_obj_set_style_pad_all(md_game_panel, 0, 0);
        lv_obj_clear_flag(md_game_panel, LV_OBJ_FLAG_SCROLLABLE);
    }

    frame_panel = ui_panel(md_game_panel, frame_x, frame_y, frame_w, frame_h);
    lv_obj_set_style_bg_color(frame_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(frame_panel, lv_color_hex(0x253043), 0);
    lv_obj_set_style_pad_all(frame_panel, 0, 0);

    md_frame_image = lv_image_create(frame_panel);
    lv_obj_set_size(md_frame_image,
                    landscape ? (int)((MD_VIEW_W * frame_scale) / LV_SCALE_NONE) :
                    MD_VIEW_W,
                    landscape ? (int)((MD_VIEW_H * frame_scale) / LV_SCALE_NONE) :
                    MD_VIEW_H);
    lv_image_set_scale(md_frame_image, frame_scale);
    lv_obj_center(md_frame_image);
    lv_obj_add_flag(md_frame_image, LV_OBJ_FLAG_HIDDEN);

    md_frame_placeholder = ui_label(frame_panel, "MD", &lv_font_montserrat_32,
                                     0x3DA5FF);
    lv_obj_center(md_frame_placeholder);

    md_game_status_label = ui_label(md_game_panel, "Ready",
                                     &lv_font_montserrat_16, 0xD3DAE3);
    lv_obj_set_width(md_game_status_label, landscape ? frame_w : 380);
    lv_label_set_long_mode(md_game_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(md_game_status_label, landscape ? frame_x : 28,
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

        md_add_control(md_game_panel, dpad_x + pad, dpad_y - pad, pad, pad,
                        "Up", 0x1F2937, 0);
        md_add_control(md_game_panel, dpad_x + pad, dpad_y + pad, pad, pad,
                        "Down", 0x1F2937, 1);
        md_add_control(md_game_panel, dpad_x, dpad_y, pad, pad,
                        "Left", 0x1F2937, 2);
        md_add_control(md_game_panel, dpad_x + pad * 2, dpad_y, pad, pad,
                        "Right", 0x1F2937, 3);
        md_add_control(md_game_panel, action_x, action_y + ab / 3, ab, ab,
                        "B", 0x2563EB, 4);
        md_add_control(md_game_panel, action_x + ab + 18, action_y - ab / 4,
                        ab, ab, "A", 0xEF4D5A, 6);
        md_add_control(md_game_panel, margin, sys_y, sys_w, sys_h,
                        "C", 0x14B8A6, 5);
        md_add_control(md_game_panel, margin + sys_w + 14, sys_y,
                        sys_w, sys_h, "Start", 0x374151, 7);
    } else {
        md_add_control(md_game_panel, 54, 690, 82, 82, "Up", 0x1F2937, 0);
        md_add_control(md_game_panel, 54, 874, 82, 82, "Down", 0x1F2937, 1);
        md_add_control(md_game_panel, 0, 782, 82, 82, "Left", 0x1F2937, 2);
        md_add_control(md_game_panel, 108, 782, 82, 82, "Right", 0x1F2937, 3);
        md_add_control(md_game_panel, 205, 875, 84, 58, "C", 0x14B8A6, 5);
        md_add_control(md_game_panel, 300, 875, 84, 58, "Start", 0x374151, 7);
        md_add_control(md_game_panel, 355, 740, 94, 94, "B", 0x2563EB, 4);
        md_add_control(md_game_panel, 454, 685, 94, 94, "A", 0xEF4D5A, 6);
    }
}

void ui_megadrive_create(lv_obj_t *scr)
{
    ui_create_header(scr, "Mega Drive");
    md_body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(md_body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(md_body, 0, 0);
    lv_obj_set_style_border_width(md_body, 0, 0);
    lv_obj_set_style_pad_all(md_body, 0, 0);

    md_create_library(md_body);
    md_create_game(md_body);
    md_build_rom_list();
    ui_extension_keyboard_set_key_cb(md_hardware_key_cb, NULL);
    md_log("Hardware keyboard mapped: arrows/WASD dpad, U/I/O A/B/C, Enter Start, Esc stop/back");

    if(!md_ui_timer) {
        md_ui_timer = lv_timer_create(md_ui_timer_cb, MD_REFRESH_TIMER_MS, NULL);
    }
}

void ui_megadrive_cleanup(void)
{
    md_request_stop();
    if(md_emulator_thread_valid) {
        pthread_join(md_emulator_thread, NULL);
        md_emulator_thread_valid = 0;
    }
    if(md_ui_timer) {
        lv_timer_delete(md_ui_timer);
        md_ui_timer = NULL;
    }
    md_audio_close();
    md_free_frame_buffers();
    md_lvgl_pressed_mask = 0;
    md_touch_pressed_mask = 0;
    md_keyboard_pressed_mask = 0;
    md_controller_state = 0xFFFFFFFFU;
    ui_extension_keyboard_set_key_cb(NULL, NULL);
    md_body = NULL;
    md_library_panel = NULL;
    md_game_panel = NULL;
    md_list_panel = NULL;
    md_status_label = NULL;
    md_count_label = NULL;
    md_title_label = NULL;
    md_meta_label = NULL;
    md_path_label = NULL;
    md_cover_image = NULL;
    md_cover_placeholder = NULL;
    md_frame_image = NULL;
    md_frame_placeholder = NULL;
    md_game_status_label = NULL;
    memset(md_control_btn, 0, sizeof(md_control_btn));
}

int ui_megadrive_handle_back(void)
{
    if(!md_game_panel || !lv_obj_is_valid(md_game_panel) ||
       lv_obj_has_flag(md_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }

    if(md_emulator_running) {
        md_request_stop();
        if(md_game_status_label && lv_obj_is_valid(md_game_status_label)) {
            lv_label_set_text(md_game_status_label, "Stopping...");
        }
    } else {
        md_show_game_view(0);
    }
    app_request_fast_refresh();
    return 1;
}

extern "C" bool initLoadGwenesisState(unsigned char *src_buffer)
{
    (void)src_buffer;
    return false;
}

extern "C" int saveGwenesisState(unsigned char *dest_buffer, int save_size)
{
    (void)dest_buffer;
    (void)save_size;
    return -1;
}

extern "C" int loadGwenesisState(unsigned char *src_buffer)
{
    (void)src_buffer;
    return -1;
}

extern "C" SaveState *saveGwenesisStateOpenForRead(const char *file_name)
{
    (void)file_name;
    return NULL;
}

extern "C" SaveState *saveGwenesisStateOpenForWrite(const char *file_name)
{
    (void)file_name;
    return NULL;
}

extern "C" int saveGwenesisStateGet(SaveState *state, const char *tag_name)
{
    (void)state;
    (void)tag_name;
    return 0;
}

extern "C" void saveGwenesisStateSet(SaveState *state, const char *tag_name,
                                      int value)
{
    (void)state;
    (void)tag_name;
    (void)value;
}

extern "C" void saveGwenesisStateGetBuffer(SaveState *state,
                                            const char *tag_name,
                                            void *buffer, int length)
{
    (void)state;
    (void)tag_name;
    if(buffer && length > 0) {
        memset(buffer, 0, (size_t)length);
    }
}

extern "C" void saveGwenesisStateSetBuffer(SaveState *state,
                                            const char *tag_name,
                                            void *buffer, int length)
{
    (void)state;
    (void)tag_name;
    (void)buffer;
    (void)length;
}
