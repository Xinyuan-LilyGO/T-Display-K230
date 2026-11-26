#include "ui_snes.h"

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
#include "third_party/snes9x/snes9x.h"
}

#define SNESUI_ROM_MAX 48
#define SNESUI_PATH_MAX 256
#define SNESUI_TITLE_MAX 96
#define SNESUI_META_MAX 160
#define SNESUI_ROM_DIR_PRIMARY "/root/snes"
#define SNESUI_LOG_PATH "/tmp/k230_phone_snes.log"
#define SNESUI_VIEW_W 512
#define SNESUI_VIEW_H 448
#define SNESUI_FRAME_BYTES (SNESUI_VIEW_W * SNESUI_VIEW_H * 2)
#define SNESUI_NATIVE_W (SNES_WIDTH * 2)
#define SNESUI_NATIVE_H (SNES_HEIGHT_EXTENDED * 2)
#define SNESUI_REFRESH_TIMER_MS 16
#define SNESUI_AUDIO_RATE 48000
#define SNESUI_AUDIO_MAX_FRAMES 1024
#define SNESUI_AUDIO_LOG_PATH "/tmp/k230_phone_snes_audio.log"
#define SNESUI_PCM_VOLUME_BIN "/root/app/k230_phone_ui/k230_pcm_volume"
#define SNESUI_VOLUME_CONFIG_PATH "/root/.k230_phone_audio_volume"

typedef struct {
    char path[SNESUI_PATH_MAX];
    char title[SNESUI_TITLE_MAX];
    char cover_path[SNESUI_PATH_MAX];
    uint64_t size;
    uint32_t crc32;
    uint8_t valid_header;
    char format[12];
    char region[20];
    char product[24];
    char meta[SNESUI_META_MAX];
} snes_rom_item_t;

static const char *const snes_scan_dirs[] = {
    SNESUI_ROM_DIR_PRIMARY,
    "/root/roms/snes",
    "/mnt/sdcard/snes",
    "/mnt/snes",
    "/media/snes",
};

static snes_rom_item_t snes_roms[SNESUI_ROM_MAX];
static int snes_rom_count;
static int snes_selected_index = -1;

static lv_timer_t *snes_ui_timer;
static lv_obj_t *snes_body;
static lv_obj_t *snes_library_panel;
static lv_obj_t *snes_game_panel;
static lv_obj_t *snes_list_panel;
static lv_obj_t *snes_status_label;
static lv_obj_t *snes_count_label;
static lv_obj_t *snes_title_label;
static lv_obj_t *snes_meta_label;
static lv_obj_t *snes_path_label;
static lv_obj_t *snes_cover_image;
static lv_obj_t *snes_cover_placeholder;
static lv_obj_t *snes_frame_image;
static lv_obj_t *snes_frame_placeholder;
static lv_obj_t *snes_game_status_label;
static lv_obj_t *snes_control_btn[12];
static int snes_list_cached_w;

static pthread_t snes_emulator_thread;
static int snes_emulator_thread_valid;
static volatile int snes_emulator_running;
static volatile int snes_emulator_finished;
static volatile int snes_emulator_result;
static volatile int snes_stop_requested;
static uint32_t snes_controller_state;
static uint32_t snes_lvgl_pressed_mask;
static uint32_t snes_touch_pressed_mask;
static uint32_t snes_keyboard_pressed_mask;
static char snes_active_rom_path[SNESUI_PATH_MAX];

static pthread_mutex_t snes_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *snes_frame_a;
static uint16_t *snes_frame_b;
static uint16_t *snes_frame_front;
static uint16_t *snes_frame_back;
static uint16_t *snes_frame_display;
static uint32_t snes_frame_seq;
static uint32_t snes_frame_seen_seq;
static lv_image_dsc_t snes_frame_dsc;
static uint16_t *snes_native_frame;
static FILE *snes_audio_pipe;
static int16_t *snes_audio_buffer;
static uint8_t *snes_video_storage;

extern "C" bool S9xInitDisplay(void)
{
    size_t screen_bytes = (size_t)SNESUI_NATIVE_W * SNESUI_NATIVE_H * 2U;
    size_t depth_bytes = (size_t)SNESUI_NATIVE_W * SNESUI_NATIVE_H;

    /* Snes9x stores buffer offsets in int32_t, so they must share one allocation. */
    snes_video_storage = (uint8_t *)calloc(1, screen_bytes * 2U +
                                             depth_bytes * 2U);
    if(!snes_video_storage) {
        return false;
    }
    snes_native_frame = (uint16_t *)snes_video_storage;
    GFX.Pitch = SNESUI_NATIVE_W * 2;
    GFX.ZPitch = SNESUI_NATIVE_W;
    GFX.Screen = snes_video_storage;
    GFX.SubScreen = snes_video_storage + screen_bytes;
    GFX.ZBuffer = snes_video_storage + screen_bytes * 2U;
    GFX.SubZBuffer = GFX.ZBuffer + depth_bytes;
    return true;
}

extern "C" void S9xDeinitDisplay(void)
{
    free(snes_video_storage);
    snes_video_storage = NULL;
    snes_native_frame = NULL;
    GFX.Screen = NULL;
    GFX.SubScreen = NULL;
    GFX.ZBuffer = NULL;
    GFX.SubZBuffer = NULL;
}

extern "C" uint32_t S9xReadJoypad(int32_t port)
{
    uint32_t pressed;
    uint32_t joypad = 0;

    if(port != 0) {
        return 0;
    }
    pressed = __atomic_load_n(&snes_controller_state, __ATOMIC_RELAXED);
    if(pressed & (1U << 0)) joypad |= SNES_UP_MASK;
    if(pressed & (1U << 1)) joypad |= SNES_DOWN_MASK;
    if(pressed & (1U << 2)) joypad |= SNES_LEFT_MASK;
    if(pressed & (1U << 3)) joypad |= SNES_RIGHT_MASK;
    if(pressed & (1U << 4)) joypad |= SNES_B_MASK;
    if(pressed & (1U << 5)) joypad |= SNES_Y_MASK;
    if(pressed & (1U << 6)) joypad |= SNES_A_MASK;
    if(pressed & (1U << 7)) joypad |= SNES_X_MASK;
    if(pressed & (1U << 8)) joypad |= SNES_TL_MASK;
    if(pressed & (1U << 9)) joypad |= SNES_TR_MASK;
    if(pressed & (1U << 10)) joypad |= SNES_SELECT_MASK;
    if(pressed & (1U << 11)) joypad |= SNES_START_MASK;
    return joypad;
}

extern "C" bool S9xReadMousePosition(int32_t, int32_t *, int32_t *, uint32_t *)
{
    return false;
}

extern "C" bool S9xReadSuperScopePosition(int32_t *, int32_t *, uint32_t *)
{
    return false;
}

extern "C" bool JustifierOffscreen(void)
{
    return true;
}

extern "C" void JustifierButtons(uint32_t *buttons)
{
    if(buttons) {
        *buttons = 0;
    }
}

static void snes_log(const char *fmt, ...)
{
    FILE *fp = fopen(SNESUI_LOG_PATH, "a");
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

static int snes_path_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

static uint64_t snes_file_size(const char *path)
{
    struct stat st;
    if(!path || stat(path, &st) != 0) {
        return 0;
    }
    return (uint64_t)st.st_size;
}

static void snes_format_size(char *out, size_t out_len, uint64_t bytes)
{
    if(bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if(bytes >= 1024ULL) {
        snprintf(out, out_len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    }
}

static const char *snes_base_name(const char *path)
{
    const char *slash;

    if(!path) {
        return "";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int snes_has_ext(const char *path, const char *ext)
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

static void snes_split_dir_stem(const char *path, char *dir, size_t dir_len,
                               char *stem, size_t stem_len)
{
    const char *base = snes_base_name(path);
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

static void snes_make_title(const char *path, char *out, size_t out_len)
{
    char stem[SNESUI_TITLE_MAX];

    snes_split_dir_stem(path, NULL, 0, stem, sizeof(stem));
    for(size_t i = 0; stem[i]; i++) {
        if(stem[i] == '_' || stem[i] == '-') {
            stem[i] = ' ';
        }
    }
    snprintf(out, out_len, "%s", stem[0] ? stem : snes_base_name(path));
}

static uint32_t snes_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
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

static uint32_t snes_crc32_file(const char *path)
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
            crc = snes_crc32_update(crc, buf, got);
        }
        if(got < sizeof(buf)) {
            break;
        }
    }
    fclose(fp);
    return crc;
}

static void snes_find_cover(snes_rom_item_t *item)
{
    char dir[SNESUI_PATH_MAX];
    char stem[SNESUI_TITLE_MAX];
    const char *exts[] = {".png", ".jpg", ".jpeg", ".bmp"};

    item->cover_path[0] = '\0';
    snes_split_dir_stem(item->path, dir, sizeof(dir), stem, sizeof(stem));
    for(size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        char path[SNESUI_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s%s", dir, stem, exts[i]);
        if(snes_path_exists(path)) {
            snprintf(item->cover_path, sizeof(item->cover_path), "%s", path);
            return;
        }
        snprintf(path, sizeof(path), "%s/covers/%s%s", dir, stem, exts[i]);
        if(snes_path_exists(path)) {
            snprintf(item->cover_path, sizeof(item->cover_path), "%s", path);
            return;
        }
    }
}

static void snes_format_rom_meta(const snes_rom_item_t *item, int compact,
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

static void snes_copy_header_text(char *out, size_t out_len,
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

static int snes_parse_header(snes_rom_item_t *item)
{
    FILE *fp;
    uint8_t h[32];
    uint64_t base;
    uint64_t offsets[2];
    int best_score = -1;
    uint8_t best[32];
    int best_hirom = 0;

    if(!item) {
        return 0;
    }
    if(item->size == 0) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "Empty ROM file");
        return 0;
    }
    if(item->size < 32U * 1024U || item->size > 16U * 1024U * 1024U) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta),
                 item->size < 32U * 1024U ? "ROM file is too small" :
                 "ROM exceeds the 16 MB limit");
        return 0;
    }
    fp = fopen(item->path, "rb");
    if(!fp) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "Cannot open ROM file");
        return 0;
    }
    base = (item->size & 0x7FFFU) == 512U ? 512U : 0U;
    offsets[0] = base + 0x7FC0U;
    offsets[1] = base + 0xFFC0U;
    for(int candidate = 0; candidate < 2; candidate++) {
        int score = 0;
        unsigned printable = 0;
        uint16_t complement;
        uint16_t checksum;

        if(offsets[candidate] + sizeof(h) > item->size ||
           fseek(fp, (long)offsets[candidate], SEEK_SET) != 0 ||
           fread(h, 1, sizeof(h), fp) != sizeof(h)) {
            continue;
        }
        for(unsigned i = 0; i < 21; i++) {
            if(h[i] == ' ' || (h[i] >= 0x21 && h[i] <= 0x7E)) {
                printable++;
            }
        }
        score += (int)printable;
        complement = (uint16_t)h[28] | ((uint16_t)h[29] << 8);
        checksum = (uint16_t)h[30] | ((uint16_t)h[31] << 8);
        if((uint16_t)(complement ^ checksum) == 0xFFFFU) {
            score += 16;
        }
        if(candidate == 0 && (h[21] & 1U) == 0) {
            score += 4;
        }
        if(candidate == 1 && (h[21] & 1U) != 0) {
            score += 4;
        }
        if(score > best_score) {
            best_score = score;
            memcpy(best, h, sizeof(best));
            best_hirom = candidate;
        }
    }
    fclose(fp);
    if(best_score < 12) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "No valid SNES header found");
        return 0;
    }

    item->valid_header = 1;
    snprintf(item->format, sizeof(item->format), "%s",
             best_hirom ? "HiROM" : "LoROM");
    snprintf(item->region, sizeof(item->region), "%s",
             best[25] >= 2 && best[25] <= 12 ? "PAL 50 Hz" : "NTSC 60 Hz");
    snes_copy_header_text(item->product, sizeof(item->product), best, 21);
    snes_format_rom_meta(item, 0, item->meta, sizeof(item->meta));
    return 1;
}

static int snes_validate_rom_for_start(const char *path, char *reason,
                                      size_t reason_len)
{
    struct stat st;
    snes_rom_item_t item;

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
    if(!snes_parse_header(&item)) {
        snprintf(reason, reason_len, "%s", item.meta);
        return 0;
    }
    return 1;
}

static int snes_rom_exists(const char *path)
{
    for(int i = 0; i < snes_rom_count; i++) {
        if(strcmp(snes_roms[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void snes_add_rom_path(const char *path)
{
    snes_rom_item_t *item;

    if(!path || snes_rom_count >= SNESUI_ROM_MAX || snes_rom_exists(path)) {
        return;
    }
    item = &snes_roms[snes_rom_count];
    memset(item, 0, sizeof(*item));
    snprintf(item->path, sizeof(item->path), "%s", path);
    item->size = snes_file_size(path);
    snes_make_title(path, item->title, sizeof(item->title));
    snes_parse_header(item);
    item->crc32 = snes_crc32_file(path);
    snes_find_cover(item);
    snes_rom_count++;
}

static void snes_scan_dir(const char *dir)
{
    DIR *dp;
    struct dirent *ent;

    dp = opendir(dir);
    if(!dp) {
        return;
    }
    while((ent = readdir(dp)) != NULL && snes_rom_count < SNESUI_ROM_MAX) {
        char path[SNESUI_PATH_MAX];
        struct stat st;

        if(ent->d_name[0] == '.') {
            continue;
        }
        if(!snes_has_ext(ent->d_name, ".sfc") &&
           !snes_has_ext(ent->d_name, ".smc")) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if(stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            snes_add_rom_path(path);
        }
    }
    closedir(dp);
}

static lv_obj_t *snes_button(lv_obj_t *parent, int x, int y, int w, int h,
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

static void snes_set_status(const char *text, uint32_t color)
{
    if(snes_status_label && lv_obj_is_valid(snes_status_label)) {
        lv_label_set_text(snes_status_label, text ? text : "");
        lv_obj_set_style_text_color(snes_status_label, lv_color_hex(color), 0);
    }
}

static void snes_update_cover(const snes_rom_item_t *item)
{
    if(!snes_cover_image || !snes_cover_placeholder) {
        return;
    }
    if(item && item->cover_path[0]) {
        lv_image_set_src(snes_cover_image, item->cover_path);
        lv_obj_clear_flag(snes_cover_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(snes_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(snes_cover_image, NULL);
        lv_obj_add_flag(snes_cover_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(snes_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void snes_update_selected_info(int index)
{
    char text[192];
    char size_buf[32];
    const snes_rom_item_t *item = NULL;

    if(index >= 0 && index < snes_rom_count) {
        item = &snes_roms[index];
        snes_selected_index = index;
    }

    if(snes_title_label && lv_obj_is_valid(snes_title_label)) {
        lv_label_set_text(snes_title_label, item ? item->title : "No ROM selected");
    }
    if(snes_meta_label && lv_obj_is_valid(snes_meta_label)) {
        if(item && item->valid_header) {
            snes_format_rom_meta(item, ui_is_landscape(), text, sizeof(text));
            lv_label_set_text(snes_meta_label, text);
        } else {
            lv_label_set_text(snes_meta_label,
                              item ? item->meta :
                              "Copy .sfc or .smc files to /root/snes");
        }
    }
    if(snes_path_label && lv_obj_is_valid(snes_path_label)) {
        if(item) {
            snes_format_size(size_buf, sizeof(size_buf), item->size);
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
            snprintf(text, sizeof(text), "%s", SNESUI_ROM_DIR_PRIMARY);
        }
        lv_label_set_text(snes_path_label, text);
    }
    snes_update_cover(item);
}

static int snes_prepare_frame_buffers(void)
{
    if(!snes_frame_a) {
        snes_frame_a = (uint16_t *)malloc(SNESUI_FRAME_BYTES);
    }
    if(!snes_frame_b) {
        snes_frame_b = (uint16_t *)malloc(SNESUI_FRAME_BYTES);
    }
    if(!snes_frame_display) {
        snes_frame_display = (uint16_t *)malloc(SNESUI_FRAME_BYTES);
    }
    if(!snes_frame_a || !snes_frame_b || !snes_frame_display) {
        return 0;
    }
    memset(snes_frame_a, 0, SNESUI_FRAME_BYTES);
    memset(snes_frame_b, 0, SNESUI_FRAME_BYTES);
    memset(snes_frame_display, 0, SNESUI_FRAME_BYTES);
    snes_frame_front = snes_frame_a;
    snes_frame_back = snes_frame_b;
    snes_frame_seq = 0;
    snes_frame_seen_seq = 0;

    memset(&snes_frame_dsc, 0, sizeof(snes_frame_dsc));
    snes_frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    snes_frame_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    snes_frame_dsc.header.w = SNESUI_VIEW_W;
    snes_frame_dsc.header.h = SNESUI_VIEW_H;
    snes_frame_dsc.header.stride = SNESUI_VIEW_W * 2;
    snes_frame_dsc.data_size = SNESUI_FRAME_BYTES;
    snes_frame_dsc.data = (const uint8_t *)snes_frame_display;
    return 1;
}

static void snes_free_frame_buffers(void)
{
    pthread_mutex_lock(&snes_frame_mutex);
    free(snes_frame_a);
    free(snes_frame_b);
    free(snes_frame_display);
    snes_frame_a = NULL;
    snes_frame_b = NULL;
    snes_frame_display = NULL;
    snes_frame_front = NULL;
    snes_frame_back = NULL;
    snes_frame_seq = 0;
    snes_frame_seen_seq = 0;
    pthread_mutex_unlock(&snes_frame_mutex);
}

static void snes_show_game_view(int show)
{
    if(snes_library_panel && lv_obj_is_valid(snes_library_panel)) {
        if(show) {
            lv_obj_add_flag(snes_library_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(snes_library_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(snes_game_panel && lv_obj_is_valid(snes_game_panel)) {
        if(show) {
            lv_obj_clear_flag(snes_game_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(snes_game_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void snes_update_controller_state(void)
{
    uint32_t pressed_mask = (snes_lvgl_pressed_mask |
                             snes_touch_pressed_mask |
                             snes_keyboard_pressed_mask) & 0xFFFU;

    __atomic_store_n(&snes_controller_state, pressed_mask, __ATOMIC_RELAXED);
}

static void snes_set_controller_bit(unsigned bit, int pressed)
{
    uint32_t mask = 1U << bit;

    if(pressed) {
        __sync_fetch_and_or(&snes_lvgl_pressed_mask, mask);
    } else {
        __sync_fetch_and_and(&snes_lvgl_pressed_mask, ~mask);
    }
    snes_update_controller_state();
}

static void snes_set_touch_pressed_mask(uint32_t mask)
{
    mask &= 0xFFFU;
    if(mask == snes_touch_pressed_mask) {
        return;
    }
    snes_touch_pressed_mask = mask;
    snes_update_controller_state();
}

static int snes_point_in_obj(const ui_touch_point_t *point, lv_obj_t *obj)
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

static void snes_update_multitouch_controls(void)
{
    ui_touch_point_t points[UI_MULTITOUCH_MAX_POINTS];
    uint32_t count;
    uint32_t mask = 0;

    if(!snes_game_panel || !lv_obj_is_valid(snes_game_panel) ||
       lv_obj_has_flag(snes_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        snes_set_touch_pressed_mask(0);
        return;
    }

    count = ui_multitouch_get_points(points, UI_MULTITOUCH_MAX_POINTS);
    for(uint32_t i = 0; i < count && i < UI_MULTITOUCH_MAX_POINTS; i++) {
        for(unsigned bit = 0; bit < 12; bit++) {
            if(snes_point_in_obj(&points[i], snes_control_btn[bit])) {
                mask |= 1U << bit;
            }
        }
    }
    snes_set_touch_pressed_mask(mask);
}

static void snes_control_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    uintptr_t raw = (uintptr_t)lv_event_get_user_data(event);
    unsigned bit;

    if(raw == 0) {
        return;
    }
    bit = (unsigned)(raw - 1U);
    if(code == LV_EVENT_PRESSED) {
        snes_set_controller_bit(bit, 1);
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        snes_set_controller_bit(bit, 0);
    }
}

static void snes_add_control(lv_obj_t *parent, int x, int y, int w, int h,
                            const char *text, uint32_t color, unsigned bit)
{
    lv_obj_t *btn = snes_button(parent, x, y, w, h, text, color);

    if(bit < 12) {
        snes_control_btn[bit] = btn;
    }
    lv_obj_add_event_cb(btn, snes_control_event, LV_EVENT_PRESSED,
                        (void *)(uintptr_t)(bit + 1U));
    lv_obj_add_event_cb(btn, snes_control_event, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)(bit + 1U));
    lv_obj_add_event_cb(btn, snes_control_event, LV_EVENT_PRESS_LOST,
                        (void *)(uintptr_t)(bit + 1U));
}

static void snes_request_stop(void)
{
    snes_stop_requested = 1;
}

static void snes_set_keyboard_bit(unsigned bit, int pressed)
{
    uint32_t mask;

    if(bit >= 12) {
        return;
    }
    mask = 1U << bit;
    if(pressed) {
        __sync_fetch_and_or(&snes_keyboard_pressed_mask, mask);
    } else {
        __sync_fetch_and_and(&snes_keyboard_pressed_mask, ~mask);
    }
    snes_update_controller_state();
}

static int snes_keyboard_bit_for_key(int code, uint32_t key)
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
        return 11;
    case LV_KEY_ENTER:
        return 11;
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
        return 4;
    }
    if(key == 'i' || key == 'I') {
        return 6;
    }
    if(key == 'j' || key == 'J') {
        return 5;
    }
    if(key == 'k' || key == 'K') return 7;
    if(key == 'q' || key == 'Q') return 8;
    if(key == 'e' || key == 'E') return 9;
    if(key == 'o' || key == 'O') return 10;
    return -1;
}

static void snes_hardware_key_cb(int code, uint32_t key, int pressed,
                                void *user_data)
{
    int bit;

    (void)user_data;
    if(code == 40 && pressed &&
       snes_game_panel && lv_obj_is_valid(snes_game_panel) &&
       !lv_obj_has_flag(snes_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        if(snes_emulator_running) {
            snes_request_stop();
            if(snes_game_status_label && lv_obj_is_valid(snes_game_status_label)) {
                lv_label_set_text(snes_game_status_label, "Stopping...");
            }
        } else {
            snes_show_game_view(0);
        }
        return;
    }

    bit = snes_keyboard_bit_for_key(code, key);
    if(bit < 0) {
        return;
    }
    snes_set_keyboard_bit((unsigned)bit, pressed);
}

static void snes_audio_close(void)
{
    if(snes_audio_pipe) {
        pclose(snes_audio_pipe);
        snes_audio_pipe = NULL;
    }
    free(snes_audio_buffer);
    snes_audio_buffer = NULL;
}

static int snes_audio_open(void)
{
    char cmd[512];

    if(snes_audio_pipe) {
        return 0;
    }
    if(!snes_audio_buffer) {
        snes_audio_buffer = (int16_t *)malloc(SNESUI_AUDIO_MAX_FRAMES * 2U *
                                             sizeof(int16_t));
    }
    if(!snes_audio_buffer) {
        snes_log("SNES audio buffer allocation failed");
        return -1;
    }

    if(access(SNESUI_PCM_VOLUME_BIN, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
                 SNESUI_PCM_VOLUME_BIN " " SNESUI_VOLUME_CONFIG_PATH
                 " 2>>" SNESUI_AUDIO_LOG_PATH " | "
                 "aplay -q -D default -t raw -f S16_LE -c 2 -r %d "
                 "-B 50000 -F 10000 - >>" SNESUI_AUDIO_LOG_PATH " 2>&1",
                 SNESUI_AUDIO_RATE);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "aplay -q -D default -t raw -f S16_LE -c 2 -r %d "
                 "-B 50000 -F 10000 - >>" SNESUI_AUDIO_LOG_PATH " 2>&1",
                 SNESUI_AUDIO_RATE);
    }

    snes_audio_pipe = popen(cmd, "w");
    if(!snes_audio_pipe) {
        snes_log("SNES audio pipe open failed");
        return -1;
    }
    setvbuf(snes_audio_pipe, NULL, _IONBF, 0);
    snes_log("SNES audio pipe opened rate=%d", SNESUI_AUDIO_RATE);
    return 0;
}

static void snes_stop_event(lv_event_t *event)
{
    (void)event;
    snes_request_stop();
    if(snes_game_status_label && lv_obj_is_valid(snes_game_status_label)) {
        lv_label_set_text(snes_game_status_label, "Stopping...");
    }
}

static void snes_publish_frame(void)
{
    int source_w = IPPU.RenderedScreenWidth;
    int source_h = IPPU.RenderedScreenHeight;

    if(!snes_native_frame) return;
    if(source_w < 1 || source_w > SNESUI_NATIVE_W) source_w = SNES_WIDTH;
    if(source_h < 1 || source_h > SNESUI_NATIVE_H) source_h = SNES_HEIGHT;
    pthread_mutex_lock(&snes_frame_mutex);
    if(snes_frame_back) {
        for(int y = 0; y < SNESUI_VIEW_H; y++) {
            const uint16_t *src = snes_native_frame +
                ((y * source_h) / SNESUI_VIEW_H) * SNESUI_NATIVE_W;
            uint16_t *dst = snes_frame_back + y * SNESUI_VIEW_W;
            for(int x = 0; x < SNESUI_VIEW_W; x++) {
                dst[x] = src[(x * source_w) / SNESUI_VIEW_W];
            }
        }
        uint16_t *tmp = snes_frame_front;
        snes_frame_front = snes_frame_back;
        snes_frame_back = tmp;
        snes_frame_seq++;
    }
    pthread_mutex_unlock(&snes_frame_mutex);
}

static void snes_submit_audio(size_t frames)
{
    size_t bytes;

    if(frames == 0 || !snes_audio_pipe || !snes_audio_buffer) {
        return;
    }
    S9xMixSamples(snes_audio_buffer, (int32_t)(frames * 2U));
    bytes = frames * 2U * sizeof(snes_audio_buffer[0]);
    if(fwrite(snes_audio_buffer, 1, bytes, snes_audio_pipe) != bytes) {
        snes_log("SNES audio write failed");
        snes_audio_close();
    }
}

static void snes_setup_settings(void)
{
    memset(&Settings, 0, sizeof(Settings));
    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = SNESUI_AUDIO_RATE;
    Settings.SoundInputRate = SNESUI_AUDIO_RATE;
    Settings.InterpolatedSound = true;
}

static void snes_make_save_path(const char *rom_path, char *out, size_t out_len)
{
    char dir[SNESUI_PATH_MAX];
    char stem[SNESUI_TITLE_MAX];

    snes_split_dir_stem(rom_path, dir, sizeof(dir), stem, sizeof(stem));
    snprintf(out, out_len, "%s/%s.srm", dir, stem);
}

static size_t snes_sram_size(void)
{
    size_t size = Memory.SRAMSize ?
        ((size_t)1U << (Memory.SRAMSize + 3U)) * 128U : 0U;
    return size > SRAM_SIZE ? SRAM_SIZE : size;
}

static void snes_load_sram(const char *save_path)
{
    FILE *fp;
    size_t size = snes_sram_size();

    if(size == 0) return;
    memset(Memory.SRAM, 0, SRAM_SIZE);
    fp = fopen(save_path, "rb");
    if(!fp) return;
    size_t got = fread(Memory.SRAM, 1, size, fp);
    fclose(fp);
    snes_log("Loaded %zu/%zu SRAM bytes from %s", got, size, save_path);
}

static void snes_save_sram(const char *save_path)
{
    char temp_path[SNESUI_PATH_MAX + 8];
    FILE *fp;
    size_t size = snes_sram_size();

    if(size == 0) return;
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", save_path);
    fp = fopen(temp_path, "wb");
    if(!fp || fwrite(Memory.SRAM, 1, size, fp) != size ||
       fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        if(fp) fclose(fp);
        unlink(temp_path);
        snes_log("Failed to write SRAM to %s", save_path);
        return;
    }
    fclose(fp);
    if(rename(temp_path, save_path) == 0) {
        snes_log("Saved %zu SRAM bytes to %s", size, save_path);
    } else {
        unlink(temp_path);
        snes_log("Failed to replace SRAM file %s", save_path);
    }
}

static int snes_run_emulator(const char *rom_path)
{
    char save_path[SNESUI_PATH_MAX];
    unsigned frame_count = 0;
    int result = -1;
    int display_ready = 0;
    int memory_ready = 0;
    int apu_ready = 0;
    int gfx_ready = 0;

    snes_setup_settings();
    if(!(display_ready = S9xInitDisplay()) ||
       !(memory_ready = S9xInitMemory()) ||
       !(apu_ready = S9xInitAPU()) || !S9xInitSound(0, 0) ||
       !(gfx_ready = S9xInitGFX())) {
        snes_log("Snes9x initialization failed");
        goto cleanup;
    }
    if(!LoadROM(rom_path)) {
        snes_log("Snes9x rejected ROM %s", rom_path);
        goto cleanup;
    }
    S9xSetPlaybackRate(SNESUI_AUDIO_RATE);
    snes_make_save_path(rom_path, save_path, sizeof(save_path));
    snes_load_sram(save_path);
    memset(snes_native_frame, 0,
           (size_t)SNESUI_NATIVE_W * SNESUI_NATIVE_H * sizeof(*snes_native_frame));
    if(snes_audio_open() != 0) {
        snes_log("Continuing without audio output");
    }
    result = 0;
    snes_log("Started %s title=%.21s fps=%d save=%s", rom_path,
             Memory.ROMName, Memory.ROMFramesPerSecond, save_path);

    while(!snes_stop_requested) {
        uint64_t frame_start = ui_monotonic_us();

        size_t audio_frames = Memory.ROMFramesPerSecond == 50 ? 960U : 800U;
        IPPU.RenderThisFrame = true;
        S9xMainLoop();
        snes_publish_frame();
        snes_submit_audio(audio_frames);
        frame_count++;
        if(frame_count % 1800U == 0) {
            snes_save_sram(save_path);
        }

        uint64_t elapsed = ui_monotonic_us() - frame_start;
        uint64_t frame_us = Memory.ROMFramesPerSecond == 50 ? 20000ULL : 16667ULL;
        if(elapsed < frame_us) {
            usleep((useconds_t)(frame_us - elapsed));
        }
    }

cleanup:
    if(result == 0) snes_save_sram(save_path);
    if(gfx_ready) S9xDeinitGFX();
    if(apu_ready) S9xDeinitAPU();
    if(memory_ready) S9xDeinitMemory();
    if(display_ready) S9xDeinitDisplay();
    snes_audio_close();
    return result;
}

static void *snes_emulator_thread_main(void *arg)
{
    char rom_path[SNESUI_PATH_MAX];

    (void)arg;
    snprintf(rom_path, sizeof(rom_path), "%s", snes_active_rom_path);

    snes_emulator_result = snes_run_emulator(rom_path);
    snes_emulator_running = 0;
    snes_emulator_finished = 1;
    return NULL;
}

static int snes_start_emulator(const char *path)
{
    char reason[128];

    if(!path || !path[0] || snes_emulator_running) {
        return 0;
    }
    if(!snes_validate_rom_for_start(path, reason, sizeof(reason))) {
        snes_set_status(reason, 0xEF4D5A);
        if(snes_game_status_label && lv_obj_is_valid(snes_game_status_label)) {
            lv_label_set_text(snes_game_status_label, reason);
        }
        snes_log("Rejected ROM %s: %s", path ? path : "(null)", reason);
        return 0;
    }
    ui_audio_stop_for_exclusive_app("Stopped for SNES");
    snes_log("Requested exclusive audio before ROM start");
    if(!snes_prepare_frame_buffers()) {
        snes_set_status("No RAM for SNES frame buffers", 0xEF4D5A);
        return 0;
    }

    snprintf(snes_active_rom_path, sizeof(snes_active_rom_path), "%s", path);
    snes_controller_state = 0;
    snes_lvgl_pressed_mask = 0;
    snes_touch_pressed_mask = 0;
    snes_keyboard_pressed_mask = 0;
    snes_stop_requested = 0;
    snes_emulator_finished = 0;
    snes_emulator_result = 0;

    if(snes_frame_image && lv_obj_is_valid(snes_frame_image)) {
        lv_image_set_src(snes_frame_image, &snes_frame_dsc);
        lv_obj_clear_flag(snes_frame_image, LV_OBJ_FLAG_HIDDEN);
    }
    if(snes_frame_placeholder && lv_obj_is_valid(snes_frame_placeholder)) {
        lv_obj_add_flag(snes_frame_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    if(snes_game_status_label && lv_obj_is_valid(snes_game_status_label)) {
        lv_label_set_text_fmt(snes_game_status_label, "Running %s",
                              snes_base_name(path));
    }

    snes_show_game_view(1);
    snes_emulator_running = 1;
    if(pthread_create(&snes_emulator_thread, NULL, snes_emulator_thread_main,
                      NULL) != 0) {
        snes_emulator_running = 0;
        snes_show_game_view(0);
        snes_set_status("Failed to start SNES thread", 0xEF4D5A);
        return 0;
    }
    snes_emulator_thread_valid = 1;
    snes_log("Started ROM %s", path);
    return 1;
}

static void snes_start_rom_event(lv_event_t *event)
{
    snes_rom_item_t *item = (snes_rom_item_t *)lv_event_get_user_data(event);
    if(!item) {
        return;
    }
    snes_update_selected_info((int)(item - snes_roms));
    snes_start_emulator(item->path);
}

static void snes_clear_list(void)
{
    if(snes_list_panel && lv_obj_is_valid(snes_list_panel)) {
        lv_obj_clean(snes_list_panel);
    }
}

static void snes_create_empty_row(const char *text)
{
    lv_obj_t *label;
    int panel_w;

    if(!snes_list_panel) {
        return;
    }
    panel_w = snes_list_cached_w > 0 ? snes_list_cached_w :
        lv_obj_get_width(snes_list_panel);
    panel_w -= 32;
    if(panel_w < 160) {
        panel_w = 160;
    }
    label = ui_label(snes_list_panel, text, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(label, panel_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(label, 16, 16);
}

static void snes_create_rom_row(int index)
{
    snes_rom_item_t *item = &snes_roms[index];
    lv_obj_t *row;
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *meta;
    lv_obj_t *size_label;
    char size_buf[32];
    char meta_text[SNESUI_META_MAX];
    int panel_w = snes_list_cached_w > 0 ? snes_list_cached_w :
        (snes_list_panel ? lv_obj_get_width(snes_list_panel) : 520);
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

    row = ui_panel(snes_list_panel, 8, 8 + index * 88, row_w, 78);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2028), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, snes_start_rom_event, LV_EVENT_CLICKED, item);

    icon = ui_label(row, item->cover_path[0] ? LV_SYMBOL_IMAGE : LV_SYMBOL_PLAY,
                    &lv_font_montserrat_22, item->valid_header ? 0x3DA5FF : 0xEF4D5A);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);

    title = ui_label(row, item->title, &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(title, title_w);
    lv_obj_set_height(title, 26);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(title, 52, 10);

    if(item->valid_header) {
        snes_format_rom_meta(item, ui_is_landscape(), meta_text,
                            sizeof(meta_text));
    } else {
        snprintf(meta_text, sizeof(meta_text), "%s", item->meta);
    }
    meta = ui_label(row, meta_text,
                    &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(meta, row_w - 72);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(meta, 52, 42);

    snes_format_size(size_buf, sizeof(size_buf), item->size);
    size_label = ui_label(row, size_buf, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_align(size_label, LV_ALIGN_RIGHT_MID, -14, 0);
}

static void snes_build_rom_list(void)
{
    char text[96];

    mkdir(SNESUI_ROM_DIR_PRIMARY, 0755);
    snes_clear_list();
    memset(snes_roms, 0, sizeof(snes_roms));
    snes_rom_count = 0;
    snes_selected_index = -1;

    for(size_t i = 0; i < sizeof(snes_scan_dirs) / sizeof(snes_scan_dirs[0]); i++) {
        snes_scan_dir(snes_scan_dirs[i]);
    }

    for(int i = 0; i < snes_rom_count; i++) {
        snes_create_rom_row(i);
    }
    if(snes_count_label && lv_obj_is_valid(snes_count_label)) {
        lv_label_set_text_fmt(snes_count_label, "%d", snes_rom_count);
    }
    if(snes_rom_count == 0) {
        snes_create_empty_row("No ROM files found.");
        snes_set_status("No SNES ROM files found", 0x9AA4AF);
        snes_update_selected_info(-1);
    } else {
        snprintf(text, sizeof(text), "%d ROM%s found", snes_rom_count,
                 snes_rom_count == 1 ? "" : "s");
        snes_set_status(text, 0x25C281);
        snes_update_selected_info(0);
    }
    snes_log("Scan complete count=%d", snes_rom_count);
}

static void snes_scan_event(lv_event_t *event)
{
    (void)event;
    snes_build_rom_list();
}

static int snes_parent_body_height(lv_obj_t *parent)
{
    int h = parent ? lv_obj_get_height(parent) : 0;

    return h > 180 ? h : ui_body_height(144);
}

static int snes_clamp_int(int value, int min_value, int max_value)
{
    if(value < min_value) {
        return min_value;
    }
    if(value > max_value) {
        return max_value;
    }
    return value;
}

static void snes_ui_timer_cb(lv_timer_t *timer)
{
    uint32_t seq;

    (void)timer;
    snes_update_multitouch_controls();
    pthread_mutex_lock(&snes_frame_mutex);
    seq = snes_frame_seq;
    if(snes_frame_front && snes_frame_display && seq != snes_frame_seen_seq) {
        memcpy(snes_frame_display, snes_frame_front, SNESUI_FRAME_BYTES);
        snes_frame_seen_seq = seq;
        pthread_mutex_unlock(&snes_frame_mutex);
        if(snes_frame_image && lv_obj_is_valid(snes_frame_image)) {
            lv_obj_invalidate(snes_frame_image);
        }
    } else {
        pthread_mutex_unlock(&snes_frame_mutex);
    }

    if(snes_emulator_finished) {
        char text[96];
        if(snes_emulator_thread_valid) {
            pthread_join(snes_emulator_thread, NULL);
            snes_emulator_thread_valid = 0;
        }
        snes_emulator_finished = 0;
        snprintf(text, sizeof(text), "SNES exited (%d)",
                 snes_emulator_result);
        snes_set_status(text, snes_emulator_result == 0 ? 0x25C281 : 0xEF4D5A);
        if(snes_game_status_label && lv_obj_is_valid(snes_game_status_label)) {
            lv_label_set_text(snes_game_status_label, text);
        }
        snes_show_game_view(0);
    }
}

static void snes_create_library(lv_obj_t *parent)
{
    lv_obj_t *cover;
    lv_obj_t *info;
    lv_obj_t *scan_btn;
    lv_obj_t *cover_icon;
    lv_obj_t *cover_text;
    lv_obj_t *folder_label;
    int landscape = ui_is_landscape();
    int body_h = snes_parent_body_height(parent);
    int screen_w = ui_screen_width();
    int panel_x = landscape ? 0 : ui_page_panel_x();
    int panel_w = landscape ? screen_w : ui_page_panel_width();
    int margin = landscape ? 24 : 0;
    int gap = landscape ? 24 : 16;
    int left_w = landscape ? snes_clamp_int(screen_w * 34 / 100, 320, 460) : panel_w;
    int min_list_w = landscape ? 360 : 420;
    int cover_x = margin;
    int cover_y = landscape ? 20 : 18;
    int cover_w = landscape ? left_w : panel_w;
    int cover_h = landscape ? snes_clamp_int(body_h * 42 / 100, 170, 260) :
        panel_w;
    int info_x = landscape ? margin : margin;
    int info_y = landscape ? 20 + cover_h + 16 : cover_y + cover_h + 12;
    int info_w = landscape ? left_w : panel_w;
    int info_h = landscape ? body_h - info_y - 20 :
        snes_clamp_int(body_h * 25 / 100, 204, 232);
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

    snes_library_panel = ui_scroll_panel(parent, panel_x, 0, panel_w, body_h);
    lv_obj_set_style_bg_color(snes_library_panel, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(snes_library_panel, 0, 0);
    lv_obj_set_style_border_width(snes_library_panel, 0, 0);
    lv_obj_set_style_pad_all(snes_library_panel, 0, 0);
    lv_obj_set_style_pad_bottom(snes_library_panel, 48, 0);
    lv_obj_set_scrollbar_mode(snes_library_panel, LV_SCROLLBAR_MODE_OFF);
    if(landscape) {
        lv_obj_clear_flag(snes_library_panel, LV_OBJ_FLAG_SCROLLABLE);
    }

    cover = ui_panel(snes_library_panel, cover_x, cover_y, cover_w, cover_h);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x182331), 0);
    lv_obj_set_style_border_color(cover, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(cover, 0, 0);

    snes_cover_image = lv_image_create(cover);
    lv_obj_set_size(snes_cover_image, cover_img_w, cover_img_h);
    lv_obj_center(snes_cover_image);
    lv_image_set_inner_align(snes_cover_image, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(snes_cover_image, LV_OBJ_FLAG_HIDDEN);

    snes_cover_placeholder = lv_obj_create(cover);
    lv_obj_set_size(snes_cover_placeholder, cover_img_w, cover_img_h);
    lv_obj_center(snes_cover_placeholder);
    lv_obj_set_style_bg_color(snes_cover_placeholder, lv_color_hex(0x17202B), 0);
    lv_obj_set_style_radius(snes_cover_placeholder, 8, 0);
    lv_obj_set_style_border_width(snes_cover_placeholder, 0, 0);
    lv_obj_clear_flag(snes_cover_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    cover_icon = ui_label(snes_cover_placeholder, "SNES", &lv_font_montserrat_32,
                          0x3DA5FF);
    lv_obj_align(cover_icon, LV_ALIGN_CENTER, 0, landscape ? -32 : -24);
    cover_text = ui_label(snes_cover_placeholder, "No Cover",
                          &lv_font_montserrat_18, 0xD3DAE3);
    lv_obj_align(cover_text, LV_ALIGN_CENTER, 0, landscape ? 34 : 24);

    info = ui_panel(snes_library_panel, info_x, info_y, info_w, info_h);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x182331), 0);
    lv_obj_set_style_border_color(info, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(info, 16, 0);
    snes_title_label = ui_label(info, "SNES", &lv_font_montserrat_26,
                              0xF2F5F8);
    lv_obj_set_width(snes_title_label, title_w);
    lv_obj_set_height(snes_title_label, 34);
    lv_label_set_long_mode(snes_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(snes_title_label, 0, 0);
    snes_meta_label = ui_label(info, "Scanning ROM library", &lv_font_montserrat_16,
                              0xD3DAE3);
    lv_obj_set_width(snes_meta_label, info_text_w);
    lv_label_set_long_mode(snes_meta_label,
                           landscape ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(snes_meta_label, 0, landscape ? 40 : 42);
    snes_path_label = ui_label(info, SNESUI_ROM_DIR_PRIMARY, &lv_font_montserrat_14,
                              0x9AA4AF);
    lv_obj_set_width(snes_path_label, info_text_w);
    lv_label_set_long_mode(snes_path_label,
                           LV_LABEL_LONG_DOT);
    lv_obj_set_pos(snes_path_label, 0, landscape ? 62 : 78);

    scan_btn = snes_button(info, scan_btn_x,
                          scan_btn_y, scan_btn_w, scan_btn_h,
                          LV_SYMBOL_REFRESH, 0x3DA5FF);
    lv_obj_add_event_cb(scan_btn, snes_scan_event, LV_EVENT_CLICKED, NULL);
    folder_label = ui_label(info, "ROMs", &lv_font_montserrat_16, 0x9AA4AF);
    if(landscape) {
        lv_obj_align(folder_label, LV_ALIGN_BOTTOM_RIGHT, -52, -15);
    } else {
        lv_obj_set_pos(folder_label, 0, scan_btn_y + (scan_btn_h - 22) / 2);
    }
    snes_count_label = ui_label(info, "0", &lv_font_montserrat_20, 0x25C281);
    if(landscape) {
        lv_obj_align(snes_count_label, LV_ALIGN_BOTTOM_RIGHT, 0, -12);
    } else {
        lv_obj_align_to(snes_count_label, folder_label, LV_ALIGN_OUT_RIGHT_MID,
                        8, 1);
    }

    snes_status_label = ui_label(snes_library_panel, "",
                                &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(snes_status_label, landscape ? left_w - 32 : panel_w);
    lv_label_set_long_mode(snes_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(snes_status_label, landscape ? margin + 16 : 0,
                   landscape ? body_h - 48 : list_y - 34);
    lv_obj_add_flag(snes_status_label, LV_OBJ_FLAG_HIDDEN);

    snes_list_panel = ui_panel(snes_library_panel, list_x, list_y, list_w, list_h);
    snes_list_cached_w = list_w;
    lv_obj_set_style_bg_color(snes_list_panel, lv_color_hex(0x121820), 0);
    lv_obj_set_style_border_color(snes_list_panel, lv_color_hex(0x243244), 0);
    lv_obj_set_style_pad_all(snes_list_panel, 0, 0);
    lv_obj_add_flag(snes_list_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(snes_list_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(snes_list_panel, LV_SCROLLBAR_MODE_OFF);
}

static void snes_create_game(lv_obj_t *parent)
{
    lv_obj_t *frame_panel;
    int landscape = ui_is_landscape();
    int body_h = snes_parent_body_height(parent);
    int screen_w = ui_screen_width();
    int side_w = landscape ? snes_clamp_int(screen_w * 22 / 100, 230, 278) : 0;
    int frame_y = landscape ? 12 : 18;
    int frame_h = landscape ? body_h - 48 : 488;
    int frame_w = landscape ? (frame_h * SNESUI_VIEW_W) / SNESUI_VIEW_H : 520;
    int frame_x = landscape ? (screen_w - frame_w) / 2 : 24;
    uint32_t frame_scale = landscape ?
        (uint32_t)((frame_w * LV_SCALE_NONE) / SNESUI_VIEW_W) : LV_SCALE_NONE;
    uint32_t frame_scale_h = landscape ?
        (uint32_t)((frame_h * LV_SCALE_NONE) / SNESUI_VIEW_H) : LV_SCALE_NONE;

    if(landscape) {
        int max_frame_w = screen_w - side_w * 2 - 72;

        if(frame_h > body_h - 44) {
            frame_h = body_h - 44;
        }
        if(frame_h < 300) {
            frame_h = body_h > 320 ? body_h - 32 : 300;
        }
        frame_w = (frame_h * SNESUI_VIEW_W) / SNESUI_VIEW_H;
        if(frame_w > max_frame_w) {
            frame_w = max_frame_w;
            frame_h = (frame_w * SNESUI_VIEW_H) / SNESUI_VIEW_W;
        }
        if(frame_w < 320) {
            frame_w = 320;
            frame_h = (frame_w * SNESUI_VIEW_H) / SNESUI_VIEW_W;
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
        frame_scale = (uint32_t)((frame_w * LV_SCALE_NONE) / SNESUI_VIEW_W);
        frame_scale_h = (uint32_t)((frame_h * LV_SCALE_NONE) / SNESUI_VIEW_H);
    }
    if(landscape && frame_scale_h < frame_scale) {
        frame_scale = frame_scale_h;
    }
    if(frame_scale == 0) {
        frame_scale = LV_SCALE_NONE;
    }

    snes_game_panel = ui_scroll_panel(parent, 0, 0, ui_screen_width(),
                                     body_h);
    lv_obj_set_style_bg_color(snes_game_panel, lv_color_hex(0x080B0F), 0);
    lv_obj_set_style_radius(snes_game_panel, 0, 0);
    lv_obj_set_style_border_width(snes_game_panel, 0, 0);
    lv_obj_add_flag(snes_game_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_scrollbar_mode(snes_game_panel, LV_SCROLLBAR_MODE_OFF);
    if(landscape) {
        lv_obj_set_style_pad_all(snes_game_panel, 0, 0);
        lv_obj_clear_flag(snes_game_panel, LV_OBJ_FLAG_SCROLLABLE);
    }

    frame_panel = ui_panel(snes_game_panel, frame_x, frame_y, frame_w, frame_h);
    lv_obj_set_style_bg_color(frame_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(frame_panel, lv_color_hex(0x253043), 0);
    lv_obj_set_style_pad_all(frame_panel, 0, 0);

    snes_frame_image = lv_image_create(frame_panel);
    lv_obj_set_size(snes_frame_image,
                    landscape ? (int)((SNESUI_VIEW_W * frame_scale) / LV_SCALE_NONE) :
                    SNESUI_VIEW_W,
                    landscape ? (int)((SNESUI_VIEW_H * frame_scale) / LV_SCALE_NONE) :
                    SNESUI_VIEW_H);
    lv_image_set_scale(snes_frame_image, frame_scale);
    lv_obj_center(snes_frame_image);
    lv_obj_add_flag(snes_frame_image, LV_OBJ_FLAG_HIDDEN);

    snes_frame_placeholder = ui_label(frame_panel, "SNES", &lv_font_montserrat_32,
                                     0x3DA5FF);
    lv_obj_center(snes_frame_placeholder);

    snes_game_status_label = ui_label(snes_game_panel, "Ready",
                                     &lv_font_montserrat_16, 0xD3DAE3);
    lv_obj_set_width(snes_game_status_label, landscape ? frame_w : 380);
    lv_label_set_long_mode(snes_game_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(snes_game_status_label, landscape ? frame_x : 28,
                   landscape ? body_h - 34 : 520);

    if(landscape) {
        int margin = 18;
        int pad = body_h < 420 ? 60 : 66;
        int ab = body_h < 420 ? 54 : 60;
        int dpad_x = margin + (side_w - pad * 3) / 2;
        int dpad_y = frame_y + frame_h / 2 - pad / 2;
        int action_cx = screen_w - margin - side_w / 2;
        int action_cy = frame_y + frame_h / 2;
        int shoulder_w = side_w - 48;
        int shoulder_h = 38;
        int sys_w = side_w - 60;
        int sys_h = 42;
        int sys_bottom_inset = body_h < 420 ? 24 : 44;
        int sys_y = body_h - sys_bottom_inset - sys_h;

        if(dpad_x < margin) {
            dpad_x = margin;
        }
        if(shoulder_w < 112) shoulder_w = 112;
        if(sys_w < 112) sys_w = 112;

        snes_add_control(snes_game_panel, dpad_x + pad, dpad_y - pad, pad, pad,
                        "Up", 0x1F2937, 0);
        snes_add_control(snes_game_panel, dpad_x + pad, dpad_y + pad, pad, pad,
                        "Down", 0x1F2937, 1);
        snes_add_control(snes_game_panel, dpad_x, dpad_y, pad, pad,
                        "Left", 0x1F2937, 2);
        snes_add_control(snes_game_panel, dpad_x + pad * 2, dpad_y, pad, pad,
                        "Right", 0x1F2937, 3);
        snes_add_control(snes_game_panel, action_cx - ab / 2,
                        action_cy + ab / 2, ab, ab,
                        "B", 0x2563EB, 4);
        snes_add_control(snes_game_panel, action_cx - ab * 3 / 2,
                        action_cy - ab / 2, ab, ab, "Y", 0x22A06B, 5);
        snes_add_control(snes_game_panel, action_cx + ab / 2,
                        action_cy - ab / 2, ab, ab, "A", 0xEF4D5A, 6);
        snes_add_control(snes_game_panel, action_cx - ab / 2,
                        action_cy - ab * 3 / 2, ab, ab, "X", 0x8B5CF6, 7);
        snes_add_control(snes_game_panel, margin + 24, 10,
                        shoulder_w, shoulder_h, "L", 0x4B5563, 8);
        snes_add_control(snes_game_panel,
                        screen_w - margin - 24 - shoulder_w, 10,
                        shoulder_w, shoulder_h, "R", 0x4B5563, 9);
        snes_add_control(snes_game_panel, margin + 30, sys_y,
                        sys_w, sys_h, "Select", 0x14B8A6, 10);
        snes_add_control(snes_game_panel, screen_w - margin - 30 - sys_w,
                        sys_y, sys_w, sys_h, "Start", 0x374151, 11);
    } else {
        snes_add_control(snes_game_panel, 54, 690, 82, 82, "Up", 0x1F2937, 0);
        snes_add_control(snes_game_panel, 54, 874, 82, 82, "Down", 0x1F2937, 1);
        snes_add_control(snes_game_panel, 0, 782, 82, 82, "Left", 0x1F2937, 2);
        snes_add_control(snes_game_panel, 108, 782, 82, 82, "Right", 0x1F2937, 3);
        snes_add_control(snes_game_panel, 205, 930, 84, 58, "Select", 0x14B8A6, 10);
        snes_add_control(snes_game_panel, 300, 930, 84, 58, "Start", 0x374151, 11);
        snes_add_control(snes_game_panel, 390, 815, 82, 82, "B", 0x2563EB, 4);
        snes_add_control(snes_game_panel, 310, 735, 82, 82, "Y", 0x22A06B, 5);
        snes_add_control(snes_game_panel, 470, 735, 82, 82, "A", 0xEF4D5A, 6);
        snes_add_control(snes_game_panel, 390, 655, 82, 82, "X", 0x8B5CF6, 7);
        snes_add_control(snes_game_panel, 16, 620, 120, 44, "L", 0x4B5563, 8);
        snes_add_control(snes_game_panel, 436, 620, 120, 44, "R", 0x4B5563, 9);
    }
}

void ui_snes_create(lv_obj_t *scr)
{
    ui_create_header(scr, "SNES");
    snes_body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(snes_body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(snes_body, 0, 0);
    lv_obj_set_style_border_width(snes_body, 0, 0);
    lv_obj_set_style_pad_all(snes_body, 0, 0);

    snes_create_library(snes_body);
    snes_create_game(snes_body);
    snes_build_rom_list();
    ui_extension_keyboard_set_key_cb(snes_hardware_key_cb, NULL);
    snes_log("Keyboard: arrows/WASD dpad, U/I B/A, J/K Y/X, Q/E L/R, O Select, Enter Start, Esc stop/back");

    if(!snes_ui_timer) {
        snes_ui_timer = lv_timer_create(snes_ui_timer_cb, SNESUI_REFRESH_TIMER_MS, NULL);
    }
}

void ui_snes_cleanup(void)
{
    snes_request_stop();
    if(snes_emulator_thread_valid) {
        pthread_join(snes_emulator_thread, NULL);
        snes_emulator_thread_valid = 0;
    }
    if(snes_ui_timer) {
        lv_timer_delete(snes_ui_timer);
        snes_ui_timer = NULL;
    }
    snes_audio_close();
    snes_free_frame_buffers();
    snes_lvgl_pressed_mask = 0;
    snes_touch_pressed_mask = 0;
    snes_keyboard_pressed_mask = 0;
    snes_controller_state = 0;
    ui_extension_keyboard_set_key_cb(NULL, NULL);
    snes_body = NULL;
    snes_library_panel = NULL;
    snes_game_panel = NULL;
    snes_list_panel = NULL;
    snes_status_label = NULL;
    snes_count_label = NULL;
    snes_title_label = NULL;
    snes_meta_label = NULL;
    snes_path_label = NULL;
    snes_cover_image = NULL;
    snes_cover_placeholder = NULL;
    snes_frame_image = NULL;
    snes_frame_placeholder = NULL;
    snes_game_status_label = NULL;
    memset(snes_control_btn, 0, sizeof(snes_control_btn));
}

int ui_snes_handle_back(void)
{
    if(!snes_game_panel || !lv_obj_is_valid(snes_game_panel) ||
       lv_obj_has_flag(snes_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }

    if(snes_emulator_running) {
        snes_request_stop();
        if(snes_game_status_label && lv_obj_is_valid(snes_game_status_label)) {
            lv_label_set_text(snes_game_status_label, "Stopping...");
        }
    } else {
        snes_show_game_view(0);
    }
    app_request_fast_refresh();
    return 1;
}
