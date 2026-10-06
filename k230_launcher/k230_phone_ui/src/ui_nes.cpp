#include "ui_nes.h"

#include "ui_audio.h"
#include "ui_hardware.h"
#include "ui_multitouch.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
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
#include "bitmap.h"
#include "event.h"
#include "gui.h"
#include "log.h"
#include "nes/nes.h"
#include "nes/nes_mmc.h"
#include "nes/nes_pal.h"
#include "nes/nesinput.h"
#include "nofconfig.h"
#include "nofrendo.h"
#include "osd.h"
#include "vid_drv.h"
}

#define NES_ROM_MAX 48
#define NES_PATH_MAX 256
#define NES_TITLE_MAX 96
#define NES_META_MAX 160
#define NES_ROM_DIR_PRIMARY "/root/nes"
#define NES_CONFIG_PATH "/root/nes/nofrendo.cfg"
#define NES_LOG_PATH "/tmp/k230_phone_nes.log"
#define NES_VIEW_W 512
#define NES_VIEW_H 480
#define NES_FRAME_BYTES (NES_VIEW_W * NES_VIEW_H * 2)
#define NES_REFRESH_TIMER_MS 16
#define NES_AUDIO_RATE 22050
#define NES_AUDIO_REFRESH 60
#define NES_AUDIO_MAX_SAMPLES 512
#define NES_AUDIO_LOG_PATH "/tmp/k230_phone_nes_audio.log"
#define NES_PCM_VOLUME_BIN "/root/app/k230_phone_ui/k230_pcm_volume"
#define NES_VOLUME_CONFIG_PATH "/root/.k230_phone_audio_volume"

typedef struct {
    char path[NES_PATH_MAX];
    char title[NES_TITLE_MAX];
    char cover_path[NES_PATH_MAX];
    uint64_t size;
    uint32_t crc32;
    uint8_t valid_header;
    uint8_t prg_banks;
    uint8_t chr_banks;
    uint16_t mapper;
    uint8_t battery;
    uint8_t trainer;
    uint8_t four_screen;
    char mirror[16];
    char system[16];
    char meta[NES_META_MAX];
} nes_rom_item_t;

static const char *const nes_scan_dirs[] = {
    NES_ROM_DIR_PRIMARY,
    "/root/roms/nes",
    "/mnt/sdcard/nes",
    "/mnt/nes",
    "/media/nes",
};

static nes_rom_item_t nes_roms[NES_ROM_MAX];
static int nes_rom_count;
static int nes_selected_index = -1;

static lv_timer_t *nes_ui_timer;
static lv_obj_t *nes_body;
static lv_obj_t *nes_library_panel;
static lv_obj_t *nes_game_panel;
static lv_obj_t *nes_list_panel;
static lv_obj_t *nes_status_label;
static lv_obj_t *nes_count_label;
static lv_obj_t *nes_title_label;
static lv_obj_t *nes_meta_label;
static lv_obj_t *nes_path_label;
static lv_obj_t *nes_cover_image;
static lv_obj_t *nes_cover_placeholder;
static lv_obj_t *nes_frame_image;
static lv_obj_t *nes_frame_placeholder;
static lv_obj_t *nes_game_status_label;
static lv_obj_t *nes_control_btn[8];
static int nes_list_cached_w;

static pthread_t nes_emulator_thread;
static int nes_emulator_thread_valid;
static volatile int nes_emulator_running;
static volatile int nes_emulator_finished;
static volatile int nes_emulator_result;
static volatile int nes_stop_requested;
static uint32_t nes_controller_state = 0xFFFFFFFFU;
static uint32_t nes_input_old_state = 0xFFFFFFFFU;
static uint32_t nes_lvgl_pressed_mask;
static uint32_t nes_touch_pressed_mask;
static uint32_t nes_keyboard_pressed_mask;
static char nes_active_rom_path[NES_PATH_MAX];

static pthread_mutex_t nes_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *nes_frame_a;
static uint16_t *nes_frame_b;
static uint16_t *nes_frame_front;
static uint16_t *nes_frame_back;
static uint16_t *nes_frame_display;
static uint32_t nes_frame_seq;
static uint32_t nes_frame_seen_seq;
static lv_image_dsc_t nes_frame_dsc;
static uint16_t nes_palette565[256];
static uint8_t nes_screen_probe[NES_VIEW_W * NES_VIEW_H];
static bitmap_t *nes_lock_bitmap;

static pthread_t nes_tick_thread;
static int nes_tick_thread_valid;
static volatile int nes_tick_thread_stop;
static int nes_tick_frequency;
static void (*nes_tick_func)(void);
static pthread_mutex_t nes_tick_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *nes_audio_pipe;
static void (*nes_audio_playfunc)(void *buffer, int size);
static int16_t *nes_audio_buffer;
static int nes_audio_accum;

static void nes_log(const char *fmt, ...)
{
    FILE *fp = fopen(NES_LOG_PATH, "a");
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

static int nes_path_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

static uint64_t nes_file_size(const char *path)
{
    struct stat st;
    if(!path || stat(path, &st) != 0) {
        return 0;
    }
    return (uint64_t)st.st_size;
}

static void nes_format_size(char *out, size_t out_len, uint64_t bytes)
{
    if(bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if(bytes >= 1024ULL) {
        snprintf(out, out_len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    }
}

static const char *nes_base_name(const char *path)
{
    const char *slash;

    if(!path) {
        return "";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int nes_has_ext(const char *path, const char *ext)
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

static void nes_split_dir_stem(const char *path, char *dir, size_t dir_len,
                               char *stem, size_t stem_len)
{
    const char *base = nes_base_name(path);
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

static void nes_make_title(const char *path, char *out, size_t out_len)
{
    char stem[NES_TITLE_MAX];

    nes_split_dir_stem(path, NULL, 0, stem, sizeof(stem));
    for(size_t i = 0; stem[i]; i++) {
        if(stem[i] == '_' || stem[i] == '-') {
            stem[i] = ' ';
        }
    }
    snprintf(out, out_len, "%s", stem[0] ? stem : nes_base_name(path));
}

static uint32_t nes_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
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

static uint32_t nes_crc32_file(const char *path)
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
            crc = nes_crc32_update(crc, buf, got);
        }
        if(got < sizeof(buf)) {
            break;
        }
    }
    fclose(fp);
    return crc;
}

static void nes_find_cover(nes_rom_item_t *item)
{
    char dir[NES_PATH_MAX];
    char stem[NES_TITLE_MAX];
    const char *exts[] = {".png", ".jpg", ".jpeg", ".bmp"};

    item->cover_path[0] = '\0';
    nes_split_dir_stem(item->path, dir, sizeof(dir), stem, sizeof(stem));
    for(size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        char path[NES_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s%s", dir, stem, exts[i]);
        if(nes_path_exists(path)) {
            snprintf(item->cover_path, sizeof(item->cover_path), "%s", path);
            return;
        }
        snprintf(path, sizeof(path), "%s/covers/%s%s", dir, stem, exts[i]);
        if(nes_path_exists(path)) {
            snprintf(item->cover_path, sizeof(item->cover_path), "%s", path);
            return;
        }
    }
}

static int nes_is_nes2_header(const uint8_t h[16])
{
    return (h[7] & 0x0C) == 0x08;
}

static uint16_t nes_mapper_from_header(const uint8_t h[16])
{
    uint16_t mapper = (uint16_t)((h[6] >> 4) | (h[7] & 0xF0));
    if(nes_is_nes2_header(h)) {
        mapper |= (uint16_t)(h[8] & 0x0F) << 8;
    }
    return mapper;
}

static int nes_mapper_supported(uint16_t mapper)
{
    return mmc_peek((int)mapper) ? 1 : 0;
}

static int nes_parse_header(nes_rom_item_t *item)
{
    FILE *fp;
    uint8_t h[16];
    size_t got;
    uint64_t expected_size;

    if(!item) {
        return 0;
    }
    if(item->size == 0) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "Empty ROM file");
        return 0;
    }
    if(item->size < sizeof(h)) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "ROM file is too small");
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
    if(got != sizeof(h) || h[0] != 'N' || h[1] != 'E' || h[2] != 'S' ||
       h[3] != 0x1A) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "Invalid iNES header");
        return 0;
    }

    item->valid_header = 1;
    item->prg_banks = h[4];
    item->chr_banks = h[5];
    item->mapper = nes_mapper_from_header(h);
    item->battery = (h[6] & 0x02) ? 1 : 0;
    item->trainer = (h[6] & 0x04) ? 1 : 0;
    item->four_screen = (h[6] & 0x08) ? 1 : 0;
    if(nes_is_nes2_header(h)) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta),
                 "Unsupported NES 2.0 ROM: mapper %u",
                 (unsigned)item->mapper);
        return 0;
    }
    if(!nes_mapper_supported(item->mapper)) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta),
                 "Unsupported mapper %u",
                 (unsigned)item->mapper);
        return 0;
    }
    if(item->prg_banks == 0) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta), "Invalid ROM: no PRG data");
        return 0;
    }
    expected_size = 16ULL + (item->trainer ? 512ULL : 0ULL) +
        (uint64_t)item->prg_banks * 16ULL * 1024ULL +
        (uint64_t)item->chr_banks * 8ULL * 1024ULL;
    if(item->size < expected_size) {
        item->valid_header = 0;
        snprintf(item->meta, sizeof(item->meta),
                 "Invalid ROM: truncated file");
        return 0;
    }
    snprintf(item->mirror, sizeof(item->mirror), "%s",
             item->four_screen ? "4-screen" : ((h[6] & 0x01) ? "Vertical" : "Horizontal"));
    snprintf(item->system, sizeof(item->system), "%s",
             (h[9] & 0x01) ? "PAL" : "NTSC");
    snprintf(item->meta, sizeof(item->meta),
             "Mapper %u  PRG %uKB  CHR %uKB  %s  %s%s%s",
             (unsigned)item->mapper, (unsigned)item->prg_banks * 16U,
             (unsigned)item->chr_banks * 8U, item->mirror, item->system,
             item->battery ? "  Battery" : "",
             item->trainer ? "  Trainer" : "");
    return 1;
}

static int nes_validate_rom_for_start(const char *path, char *reason,
                                      size_t reason_len)
{
    FILE *fp;
    struct stat st;
    uint8_t h[16];
    size_t got;
    uint64_t expected_size;
    uint8_t prg_banks;
    uint8_t chr_banks;
    uint16_t mapper;
    int trainer;

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
    if(st.st_size <= 0) {
        snprintf(reason, reason_len, "ROM file is empty");
        return 0;
    }
    if(st.st_size < (off_t)sizeof(h)) {
        snprintf(reason, reason_len, "ROM file is too small");
        return 0;
    }
    fp = fopen(path, "rb");
    if(!fp) {
        snprintf(reason, reason_len, "Cannot open ROM file");
        return 0;
    }
    got = fread(h, 1, sizeof(h), fp);
    fclose(fp);
    if(got != sizeof(h) || h[0] != 'N' || h[1] != 'E' || h[2] != 'S' ||
       h[3] != 0x1A) {
        snprintf(reason, reason_len, "Invalid NES ROM header");
        return 0;
    }
    prg_banks = h[4];
    chr_banks = h[5];
    trainer = (h[6] & 0x04) ? 1 : 0;
    mapper = nes_mapper_from_header(h);
    if(nes_is_nes2_header(h)) {
        snprintf(reason, reason_len, "Unsupported NES 2.0 ROM: mapper %u",
                 (unsigned)mapper);
        return 0;
    }
    if(!nes_mapper_supported(mapper)) {
        snprintf(reason, reason_len, "Unsupported mapper %u",
                 (unsigned)mapper);
        return 0;
    }
    if(prg_banks == 0) {
        snprintf(reason, reason_len, "Invalid ROM: no PRG data");
        return 0;
    }
    expected_size = 16ULL + (trainer ? 512ULL : 0ULL) +
        (uint64_t)prg_banks * 16ULL * 1024ULL +
        (uint64_t)chr_banks * 8ULL * 1024ULL;
    if((uint64_t)st.st_size < expected_size) {
        snprintf(reason, reason_len, "ROM file is truncated");
        return 0;
    }
    return 1;
}

static int nes_rom_exists(const char *path)
{
    for(int i = 0; i < nes_rom_count; i++) {
        if(strcmp(nes_roms[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void nes_add_rom_path(const char *path)
{
    nes_rom_item_t *item;

    if(!path || nes_rom_count >= NES_ROM_MAX || nes_rom_exists(path)) {
        return;
    }
    item = &nes_roms[nes_rom_count];
    memset(item, 0, sizeof(*item));
    snprintf(item->path, sizeof(item->path), "%s", path);
    item->size = nes_file_size(path);
    nes_make_title(path, item->title, sizeof(item->title));
    nes_parse_header(item);
    item->crc32 = nes_crc32_file(path);
    nes_find_cover(item);
    nes_rom_count++;
}

static void nes_scan_dir(const char *dir)
{
    DIR *dp;
    struct dirent *ent;

    dp = opendir(dir);
    if(!dp) {
        return;
    }
    while((ent = readdir(dp)) != NULL && nes_rom_count < NES_ROM_MAX) {
        char path[NES_PATH_MAX];
        struct stat st;

        if(ent->d_name[0] == '.') {
            continue;
        }
        if(!nes_has_ext(ent->d_name, ".nes")) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if(stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            nes_add_rom_path(path);
        }
    }
    closedir(dp);
}

static lv_obj_t *nes_button(lv_obj_t *parent, int x, int y, int w, int h,
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

static void nes_set_status(const char *text, uint32_t color)
{
    if(nes_status_label && lv_obj_is_valid(nes_status_label)) {
        lv_label_set_text(nes_status_label, text ? text : "");
        lv_obj_set_style_text_color(nes_status_label, lv_color_hex(color), 0);
    }
}

static void nes_update_cover(const nes_rom_item_t *item)
{
    if(!nes_cover_image || !nes_cover_placeholder) {
        return;
    }
    if(item && item->cover_path[0]) {
        lv_image_set_src(nes_cover_image, item->cover_path);
        lv_obj_clear_flag(nes_cover_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(nes_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(nes_cover_image, NULL);
        lv_obj_add_flag(nes_cover_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(nes_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void nes_update_selected_info(int index)
{
    char text[192];
    char size_buf[32];
    const nes_rom_item_t *item = NULL;

    if(index >= 0 && index < nes_rom_count) {
        item = &nes_roms[index];
        nes_selected_index = index;
    }

    if(nes_title_label && lv_obj_is_valid(nes_title_label)) {
        lv_label_set_text(nes_title_label, item ? item->title : "No ROM selected");
    }
    if(nes_meta_label && lv_obj_is_valid(nes_meta_label)) {
        lv_label_set_text(nes_meta_label, item ? item->meta : "Copy .nes files to /root/nes");
    }
    if(nes_path_label && lv_obj_is_valid(nes_path_label)) {
        if(item) {
            nes_format_size(size_buf, sizeof(size_buf), item->size);
            snprintf(text, sizeof(text), "%s\n%s  CRC32 %08X%s",
                     item->path, size_buf, item->crc32,
                     item->cover_path[0] ? "\nSidecar cover found" : "\nNo embedded cover in NES ROM");
        } else {
            snprintf(text, sizeof(text), "%s", NES_ROM_DIR_PRIMARY);
        }
        lv_label_set_text(nes_path_label, text);
    }
    nes_update_cover(item);
}

static int nes_prepare_frame_buffers(void)
{
    if(!nes_frame_a) {
        nes_frame_a = (uint16_t *)malloc(NES_FRAME_BYTES);
    }
    if(!nes_frame_b) {
        nes_frame_b = (uint16_t *)malloc(NES_FRAME_BYTES);
    }
    if(!nes_frame_display) {
        nes_frame_display = (uint16_t *)malloc(NES_FRAME_BYTES);
    }
    if(!nes_frame_a || !nes_frame_b || !nes_frame_display) {
        return 0;
    }
    memset(nes_frame_a, 0, NES_FRAME_BYTES);
    memset(nes_frame_b, 0, NES_FRAME_BYTES);
    memset(nes_frame_display, 0, NES_FRAME_BYTES);
    nes_frame_front = nes_frame_a;
    nes_frame_back = nes_frame_b;
    nes_frame_seq = 0;
    nes_frame_seen_seq = 0;

    memset(&nes_frame_dsc, 0, sizeof(nes_frame_dsc));
    nes_frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    nes_frame_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    nes_frame_dsc.header.w = NES_VIEW_W;
    nes_frame_dsc.header.h = NES_VIEW_H;
    nes_frame_dsc.header.stride = NES_VIEW_W * 2;
    nes_frame_dsc.data_size = NES_FRAME_BYTES;
    nes_frame_dsc.data = (const uint8_t *)nes_frame_display;
    return 1;
}

static void nes_free_frame_buffers(void)
{
    pthread_mutex_lock(&nes_frame_mutex);
    free(nes_frame_a);
    free(nes_frame_b);
    free(nes_frame_display);
    nes_frame_a = NULL;
    nes_frame_b = NULL;
    nes_frame_display = NULL;
    nes_frame_front = NULL;
    nes_frame_back = NULL;
    nes_frame_seq = 0;
    nes_frame_seen_seq = 0;
    pthread_mutex_unlock(&nes_frame_mutex);
}

static void nes_show_game_view(int show)
{
    if(nes_library_panel && lv_obj_is_valid(nes_library_panel)) {
        if(show) {
            lv_obj_add_flag(nes_library_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(nes_library_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(nes_game_panel && lv_obj_is_valid(nes_game_panel)) {
        if(show) {
            lv_obj_clear_flag(nes_game_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(nes_game_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void nes_update_controller_state(void)
{
    uint32_t pressed_mask = (nes_lvgl_pressed_mask |
                             nes_touch_pressed_mask |
                             nes_keyboard_pressed_mask) & 0xFFU;

    __atomic_store_n(&nes_controller_state, ~pressed_mask, __ATOMIC_RELAXED);
}

static void nes_set_controller_bit(unsigned bit, int pressed)
{
    uint32_t mask = 1U << bit;

    if(pressed) {
        __sync_fetch_and_or(&nes_lvgl_pressed_mask, mask);
    } else {
        __sync_fetch_and_and(&nes_lvgl_pressed_mask, ~mask);
    }
    nes_update_controller_state();
}

static void nes_set_touch_pressed_mask(uint32_t mask)
{
    mask &= 0xFFU;
    if(mask == nes_touch_pressed_mask) {
        return;
    }
    nes_touch_pressed_mask = mask;
    nes_update_controller_state();
}

static int nes_point_in_obj(const ui_touch_point_t *point, lv_obj_t *obj)
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

static void nes_update_multitouch_controls(void)
{
    ui_touch_point_t points[UI_MULTITOUCH_MAX_POINTS];
    uint32_t count;
    uint32_t mask = 0;

    if(!nes_game_panel || !lv_obj_is_valid(nes_game_panel) ||
       lv_obj_has_flag(nes_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        nes_set_touch_pressed_mask(0);
        return;
    }

    count = ui_multitouch_get_points(points, UI_MULTITOUCH_MAX_POINTS);
    for(uint32_t i = 0; i < count && i < UI_MULTITOUCH_MAX_POINTS; i++) {
        for(unsigned bit = 0; bit < 8; bit++) {
            if(nes_point_in_obj(&points[i], nes_control_btn[bit])) {
                mask |= 1U << bit;
            }
        }
    }
    nes_set_touch_pressed_mask(mask);
}

static void nes_control_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    uintptr_t raw = (uintptr_t)lv_event_get_user_data(event);
    unsigned bit;

    if(raw == 0) {
        return;
    }
    bit = (unsigned)(raw - 1U);
    if(code == LV_EVENT_PRESSED) {
        nes_set_controller_bit(bit, 1);
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        nes_set_controller_bit(bit, 0);
    }
}

static void nes_add_control(lv_obj_t *parent, int x, int y, int w, int h,
                            const char *text, uint32_t color, unsigned bit)
{
    lv_obj_t *btn = nes_button(parent, x, y, w, h, text, color);

    if(bit < 8) {
        nes_control_btn[bit] = btn;
    }
    lv_obj_add_event_cb(btn, nes_control_event, LV_EVENT_PRESSED,
                        (void *)(uintptr_t)(bit + 1U));
    lv_obj_add_event_cb(btn, nes_control_event, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)(bit + 1U));
    lv_obj_add_event_cb(btn, nes_control_event, LV_EVENT_PRESS_LOST,
                        (void *)(uintptr_t)(bit + 1U));
}

static void nes_request_stop(void)
{
    nes_stop_requested = 1;
}

static void nes_set_keyboard_bit(unsigned bit, int pressed)
{
    uint32_t mask;

    if(bit >= 8) {
        return;
    }
    mask = 1U << bit;
    if(pressed) {
        __sync_fetch_and_or(&nes_keyboard_pressed_mask, mask);
    } else {
        __sync_fetch_and_and(&nes_keyboard_pressed_mask, ~mask);
    }
    nes_update_controller_state();
}

static int nes_keyboard_bit_for_key(int code, uint32_t key)
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
        return 4;
    case LV_KEY_ENTER:
        return 5;
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
    case 5:
    case 14:
    case 7:
        return 4;
    case 21:
        return 5;
    case 45:
    case 44:
    case 43:
        return 6;
    case 35:
    case 34:
    case 33:
        return 7;
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
    if(key == 'u' || key == 'U' || key == 'i' || key == 'I' ||
       key == 'o' || key == 'O') {
        return 6;
    }
    if(key == 'h' || key == 'H' || key == 'j' || key == 'J' ||
       key == 'k' || key == 'K') {
        return 7;
    }
    return -1;
}

static void nes_hardware_key_cb(int code, uint32_t key, int pressed,
                                void *user_data)
{
    int bit;

    (void)user_data;
    if(code == 40 && pressed &&
       nes_game_panel && lv_obj_is_valid(nes_game_panel) &&
       !lv_obj_has_flag(nes_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        if(nes_emulator_running) {
            nes_request_stop();
            if(nes_game_status_label && lv_obj_is_valid(nes_game_status_label)) {
                lv_label_set_text(nes_game_status_label, "Stopping...");
            }
        } else {
            nes_show_game_view(0);
        }
        return;
    }

    bit = nes_keyboard_bit_for_key(code, key);
    if(bit < 0) {
        return;
    }
    nes_set_keyboard_bit((unsigned)bit, pressed);
}

static void nes_audio_close(void)
{
    if(nes_audio_pipe) {
        pclose(nes_audio_pipe);
        nes_audio_pipe = NULL;
    }
    free(nes_audio_buffer);
    nes_audio_buffer = NULL;
    nes_audio_playfunc = NULL;
    nes_audio_accum = 0;
}

static int nes_audio_open(void)
{
    char cmd[512];

    if(nes_audio_pipe) {
        return 0;
    }
    if(!nes_audio_buffer) {
        nes_audio_buffer = (int16_t *)malloc(NES_AUDIO_MAX_SAMPLES *
                                             sizeof(int16_t));
    }
    if(!nes_audio_buffer) {
        nes_log("NES audio buffer allocation failed");
        return -1;
    }

    if(access(NES_PCM_VOLUME_BIN, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
                 NES_PCM_VOLUME_BIN " " NES_VOLUME_CONFIG_PATH
                 " 2>>" NES_AUDIO_LOG_PATH " | "
                 "aplay -q -D default -t raw -f S16_LE -c 1 -r %d "
                 "-B 50000 -F 10000 - >>" NES_AUDIO_LOG_PATH " 2>&1",
                 NES_AUDIO_RATE);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "aplay -q -D default -t raw -f S16_LE -c 1 -r %d "
                 "-B 50000 -F 10000 - >>" NES_AUDIO_LOG_PATH " 2>&1",
                 NES_AUDIO_RATE);
    }

    nes_audio_pipe = popen(cmd, "w");
    if(!nes_audio_pipe) {
        nes_log("NES audio pipe open failed");
        return -1;
    }
    setvbuf(nes_audio_pipe, NULL, _IONBF, 0);
    nes_audio_accum = 0;
    nes_log("NES audio pipe opened rate=%d", NES_AUDIO_RATE);
    return 0;
}

static void nes_stop_event(lv_event_t *event)
{
    (void)event;
    nes_request_stop();
    if(nes_game_status_label && lv_obj_is_valid(nes_game_status_label)) {
        lv_label_set_text(nes_game_status_label, "Stopping...");
    }
}

static int nes_log_print(const char *text)
{
    if(text) {
        nes_log("%s", text);
    }
    return 0;
}

static void *nes_tick_thread_main(void *arg)
{
    (void)arg;
    while(!nes_tick_thread_stop) {
        int freq = nes_tick_frequency > 0 ? nes_tick_frequency : 60;
        void (*fn)(void);
        usleep((useconds_t)(1000000 / freq));
        pthread_mutex_lock(&nes_tick_mutex);
        fn = nes_tick_func;
        pthread_mutex_unlock(&nes_tick_mutex);
        if(fn && !nes_tick_thread_stop) {
            fn();
        }
    }
    return NULL;
}

static void nes_stop_tick_thread(void)
{
    nes_tick_thread_stop = 1;
    if(nes_tick_thread_valid) {
        pthread_join(nes_tick_thread, NULL);
        nes_tick_thread_valid = 0;
    }
    pthread_mutex_lock(&nes_tick_mutex);
    nes_tick_func = NULL;
    nes_tick_frequency = 0;
    pthread_mutex_unlock(&nes_tick_mutex);
}

static void *nes_emulator_thread_main(void *arg)
{
    char rom_path[NES_PATH_MAX];
    char *argv[1];
    int result;

    (void)arg;
    snprintf(rom_path, sizeof(rom_path), "%s", nes_active_rom_path);
    argv[0] = rom_path;

    result = nofrendo_main(1, argv);
    main_eject();
    vid_shutdown();
    osd_shutdown();

    nes_emulator_result = result;
    nes_emulator_running = 0;
    nes_emulator_finished = 1;
    return NULL;
}

static int nes_start_emulator(const char *path)
{
    char reason[128];

    if(!path || !path[0] || nes_emulator_running) {
        return 0;
    }
    if(!nes_validate_rom_for_start(path, reason, sizeof(reason))) {
        nes_set_status(reason, 0xEF4D5A);
        if(nes_game_status_label && lv_obj_is_valid(nes_game_status_label)) {
            lv_label_set_text(nes_game_status_label, reason);
        }
        nes_log("Rejected ROM %s: %s", path ? path : "(null)", reason);
        return 0;
    }
    ui_audio_stop_for_exclusive_app("Stopped for NES");
    nes_log("Requested exclusive audio before ROM start");
    if(!nes_prepare_frame_buffers()) {
        nes_set_status("No RAM for NES frame buffers", 0xEF4D5A);
        return 0;
    }

    snprintf(nes_active_rom_path, sizeof(nes_active_rom_path), "%s", path);
    nes_controller_state = 0xFFFFFFFFU;
    nes_input_old_state = 0xFFFFFFFFU;
    nes_lvgl_pressed_mask = 0;
    nes_touch_pressed_mask = 0;
    nes_keyboard_pressed_mask = 0;
    nes_stop_requested = 0;
    nes_emulator_finished = 0;
    nes_emulator_result = 0;

    if(nes_frame_image && lv_obj_is_valid(nes_frame_image)) {
        lv_image_set_src(nes_frame_image, &nes_frame_dsc);
        lv_obj_clear_flag(nes_frame_image, LV_OBJ_FLAG_HIDDEN);
    }
    if(nes_frame_placeholder && lv_obj_is_valid(nes_frame_placeholder)) {
        lv_obj_add_flag(nes_frame_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    if(nes_game_status_label && lv_obj_is_valid(nes_game_status_label)) {
        lv_label_set_text_fmt(nes_game_status_label, "Running %s",
                              nes_base_name(path));
    }

    nes_show_game_view(1);
    nes_emulator_running = 1;
    if(pthread_create(&nes_emulator_thread, NULL, nes_emulator_thread_main,
                      NULL) != 0) {
        nes_emulator_running = 0;
        nes_show_game_view(0);
        nes_set_status("Failed to start NES thread", 0xEF4D5A);
        return 0;
    }
    nes_emulator_thread_valid = 1;
    nes_log("Started ROM %s", path);
    return 1;
}

static void nes_start_rom_event(lv_event_t *event)
{
    nes_rom_item_t *item = (nes_rom_item_t *)lv_event_get_user_data(event);
    if(!item) {
        return;
    }
    nes_update_selected_info((int)(item - nes_roms));
    nes_start_emulator(item->path);
}

static void nes_clear_list(void)
{
    if(nes_list_panel && lv_obj_is_valid(nes_list_panel)) {
        lv_obj_clean(nes_list_panel);
    }
}

static void nes_create_empty_row(const char *text)
{
    lv_obj_t *label;
    int panel_w;

    if(!nes_list_panel) {
        return;
    }
    panel_w = nes_list_cached_w > 0 ? nes_list_cached_w :
        lv_obj_get_width(nes_list_panel);
    panel_w -= 32;
    if(panel_w < 160) {
        panel_w = 160;
    }
    label = ui_label(nes_list_panel, text, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(label, panel_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(label, 16, 16);
}

static void nes_create_rom_row(int index)
{
    nes_rom_item_t *item = &nes_roms[index];
    lv_obj_t *row;
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *meta;
    lv_obj_t *size_label;
    char size_buf[32];
    int panel_w = nes_list_cached_w > 0 ? nes_list_cached_w :
        (nes_list_panel ? lv_obj_get_width(nes_list_panel) : 520);
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

    row = ui_panel(nes_list_panel, 8, 8 + index * 88, row_w, 78);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2028), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, nes_start_rom_event, LV_EVENT_CLICKED, item);

    icon = ui_label(row, item->cover_path[0] ? LV_SYMBOL_IMAGE : LV_SYMBOL_PLAY,
                    &lv_font_montserrat_22, item->valid_header ? 0x3DA5FF : 0xEF4D5A);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);

    title = ui_label(row, item->title, &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(title, title_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title, 52, 10);

    meta = ui_label(row, item->valid_header ? item->meta : item->meta,
                    &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(meta, row_w - 72);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(meta, 52, 42);

    nes_format_size(size_buf, sizeof(size_buf), item->size);
    size_label = ui_label(row, size_buf, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_align(size_label, LV_ALIGN_RIGHT_MID, -14, 0);
}

static void nes_build_rom_list(void)
{
    char text[96];

    mkdir(NES_ROM_DIR_PRIMARY, 0755);
    nes_clear_list();
    memset(nes_roms, 0, sizeof(nes_roms));
    nes_rom_count = 0;
    nes_selected_index = -1;

    for(size_t i = 0; i < sizeof(nes_scan_dirs) / sizeof(nes_scan_dirs[0]); i++) {
        nes_scan_dir(nes_scan_dirs[i]);
    }

    for(int i = 0; i < nes_rom_count; i++) {
        nes_create_rom_row(i);
    }
    if(nes_count_label && lv_obj_is_valid(nes_count_label)) {
        lv_label_set_text_fmt(nes_count_label, "%d", nes_rom_count);
    }
    if(nes_rom_count == 0) {
        nes_create_empty_row("No ROM files found.");
        nes_set_status("No NES ROM files found", 0x9AA4AF);
        nes_update_selected_info(-1);
    } else {
        snprintf(text, sizeof(text), "%d ROM%s found", nes_rom_count,
                 nes_rom_count == 1 ? "" : "s");
        nes_set_status(text, 0x25C281);
        nes_update_selected_info(0);
    }
    nes_log("Scan complete count=%d", nes_rom_count);
}

static void nes_scan_event(lv_event_t *event)
{
    (void)event;
    nes_build_rom_list();
}

static int nes_parent_body_height(lv_obj_t *parent)
{
    int h = parent ? lv_obj_get_height(parent) : 0;

    return h > 180 ? h : ui_body_height(144);
}

static int nes_clamp_int(int value, int min_value, int max_value)
{
    if(value < min_value) {
        return min_value;
    }
    if(value > max_value) {
        return max_value;
    }
    return value;
}

static void nes_ui_timer_cb(lv_timer_t *timer)
{
    uint32_t seq;

    (void)timer;
    nes_update_multitouch_controls();
    pthread_mutex_lock(&nes_frame_mutex);
    seq = nes_frame_seq;
    if(nes_frame_front && nes_frame_display && seq != nes_frame_seen_seq) {
        memcpy(nes_frame_display, nes_frame_front, NES_FRAME_BYTES);
        nes_frame_seen_seq = seq;
        pthread_mutex_unlock(&nes_frame_mutex);
        if(nes_frame_image && lv_obj_is_valid(nes_frame_image)) {
            lv_obj_invalidate(nes_frame_image);
        }
    } else {
        pthread_mutex_unlock(&nes_frame_mutex);
    }

    if(nes_emulator_finished) {
        char text[96];
        if(nes_emulator_thread_valid) {
            pthread_join(nes_emulator_thread, NULL);
            nes_emulator_thread_valid = 0;
        }
        nes_emulator_finished = 0;
        snprintf(text, sizeof(text), "NES exited (%d)", nes_emulator_result);
        nes_set_status(text, nes_emulator_result == 0 ? 0x25C281 : 0xEF4D5A);
        if(nes_game_status_label && lv_obj_is_valid(nes_game_status_label)) {
            lv_label_set_text(nes_game_status_label, text);
        }
        nes_show_game_view(0);
    }
}

static void nes_create_library(lv_obj_t *parent)
{
    lv_obj_t *cover;
    lv_obj_t *info;
    lv_obj_t *scan_btn;
    lv_obj_t *cover_icon;
    lv_obj_t *cover_text;
    lv_obj_t *folder_label;
    int landscape = ui_is_landscape();
    int body_h = nes_parent_body_height(parent);
    int screen_w = ui_screen_width();
    int panel_x = landscape ? 0 : ui_page_panel_x();
    int panel_w = landscape ? screen_w : ui_page_panel_width();
    int margin = landscape ? 24 : 0;
    int gap = landscape ? 24 : 16;
    int left_w = landscape ? nes_clamp_int(screen_w * 34 / 100, 320, 460) : panel_w;
    int min_list_w = landscape ? 360 : 420;
    int list_x = landscape ? margin + left_w + gap : margin;
    int list_y = landscape ? 20 : 360;
    int list_w = landscape ? screen_w - list_x - margin : panel_w;
    int list_h = landscape ? body_h - 40 : body_h - list_y - 24;
    int cover_w = landscape ? left_w : (panel_w - gap) / 2;
    int cover_h = landscape ? nes_clamp_int(body_h * 42 / 100, 170, 260) : 260;
    int cover_img_w = landscape ? cover_w - 32 : cover_w - 28;
    int cover_img_h = landscape ? cover_h - 32 : cover_h - 30;
    int info_x = landscape ? margin : margin + cover_w + gap;
    int info_y = landscape ? 20 + cover_h + 16 : 22;
    int info_w = landscape ? left_w : cover_w;
    int info_h = landscape ? body_h - info_y - 54 : cover_h;
    int info_text_w = info_w - 32;

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
    if(list_h < 260) {
        list_h = 260;
    }
    if(landscape && info_h < 136) {
        cover_h -= 136 - info_h;
        if(cover_h < 150) {
            cover_h = 150;
        }
        cover_img_h = cover_h - 32;
        info_y = 20 + cover_h + 16;
        info_h = body_h - info_y - 54;
    }
    if(info_h < 112) {
        info_h = 112;
    }

    nes_library_panel = ui_scroll_panel(parent, panel_x, 0, panel_w, body_h);
    lv_obj_set_style_bg_color(nes_library_panel, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(nes_library_panel, 0, 0);
    lv_obj_set_style_border_width(nes_library_panel, 0, 0);
    lv_obj_set_style_pad_all(nes_library_panel, 0, 0);
    lv_obj_set_style_pad_bottom(nes_library_panel, 48, 0);
    lv_obj_set_scrollbar_mode(nes_library_panel, LV_SCROLLBAR_MODE_OFF);
    if(landscape) {
        lv_obj_clear_flag(nes_library_panel, LV_OBJ_FLAG_SCROLLABLE);
    }

    cover = ui_panel(nes_library_panel, margin, landscape ? 20 : 22,
                     cover_w, cover_h);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x182331), 0);
    lv_obj_set_style_border_color(cover, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(cover, 0, 0);

    nes_cover_image = lv_image_create(cover);
    lv_obj_set_size(nes_cover_image, cover_img_w, cover_img_h);
    lv_obj_center(nes_cover_image);
    lv_image_set_inner_align(nes_cover_image, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(nes_cover_image, LV_OBJ_FLAG_HIDDEN);

    nes_cover_placeholder = lv_obj_create(cover);
    lv_obj_set_size(nes_cover_placeholder, cover_img_w, cover_img_h);
    lv_obj_center(nes_cover_placeholder);
    lv_obj_set_style_bg_color(nes_cover_placeholder, lv_color_hex(0x17202B), 0);
    lv_obj_set_style_radius(nes_cover_placeholder, 8, 0);
    lv_obj_set_style_border_width(nes_cover_placeholder, 0, 0);
    lv_obj_clear_flag(nes_cover_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    cover_icon = ui_label(nes_cover_placeholder, "NES", &lv_font_montserrat_32,
                          0x3DA5FF);
    lv_obj_align(cover_icon, LV_ALIGN_CENTER, 0, landscape ? -32 : -24);
    cover_text = ui_label(nes_cover_placeholder, "No Cover",
                          &lv_font_montserrat_18, 0xD3DAE3);
    lv_obj_align(cover_text, LV_ALIGN_CENTER, 0, landscape ? 34 : 24);

    info = ui_panel(nes_library_panel, info_x, info_y, info_w, info_h);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x182331), 0);
    lv_obj_set_style_border_color(info, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(info, 16, 0);
    nes_title_label = ui_label(info, "NES", &lv_font_montserrat_26, 0xF2F5F8);
    lv_obj_set_width(nes_title_label, info_text_w);
    lv_label_set_long_mode(nes_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(nes_title_label, 0, 0);
    nes_meta_label = ui_label(info, "Scanning ROM library", &lv_font_montserrat_16,
                              0xD3DAE3);
    lv_obj_set_width(nes_meta_label, info_text_w);
    lv_label_set_long_mode(nes_meta_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(nes_meta_label, 0, landscape ? 40 : 48);
    nes_path_label = ui_label(info, NES_ROM_DIR_PRIMARY, &lv_font_montserrat_14,
                              0x9AA4AF);
    lv_obj_set_width(nes_path_label, info_text_w);
    lv_label_set_long_mode(nes_path_label,
                           LV_LABEL_LONG_DOT);
    lv_obj_set_pos(nes_path_label, 0, landscape ? 78 : 108);

    scan_btn = nes_button(info, landscape ? info_w - 164 : 0,
                          landscape ? 0 : 162, 132, 46,
                          LV_SYMBOL_REFRESH, 0x3DA5FF);
    lv_obj_add_event_cb(scan_btn, nes_scan_event, LV_EVENT_CLICKED, NULL);
    folder_label = ui_label(info, "ROMs", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(folder_label, LV_ALIGN_BOTTOM_RIGHT, -52, -15);
    nes_count_label = ui_label(info, "0", &lv_font_montserrat_20, 0x25C281);
    lv_obj_align(nes_count_label, LV_ALIGN_BOTTOM_RIGHT, 0, -12);

    nes_status_label = ui_label(nes_library_panel, "Ready",
                                &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(nes_status_label, landscape ? left_w : panel_w);
    lv_label_set_long_mode(nes_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(nes_status_label, landscape ? 24 : 0,
                   landscape ? body_h - 30 : list_y - 34);

    nes_list_panel = ui_panel(nes_library_panel, list_x, list_y, list_w, list_h);
    nes_list_cached_w = list_w;
    lv_obj_set_style_bg_color(nes_list_panel, lv_color_hex(0x121820), 0);
    lv_obj_set_style_border_color(nes_list_panel, lv_color_hex(0x243244), 0);
    lv_obj_set_style_pad_all(nes_list_panel, 0, 0);
    lv_obj_add_flag(nes_list_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(nes_list_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nes_list_panel, LV_SCROLLBAR_MODE_OFF);
}

static void nes_create_game(lv_obj_t *parent)
{
    lv_obj_t *frame_panel;
    int landscape = ui_is_landscape();
    int body_h = nes_parent_body_height(parent);
    int screen_w = ui_screen_width();
    int side_w = landscape ? nes_clamp_int(screen_w * 22 / 100, 230, 278) : 0;
    int frame_y = landscape ? 12 : 18;
    int frame_h = landscape ? body_h - 48 : 488;
    int frame_w = landscape ? (frame_h * NES_VIEW_W) / NES_VIEW_H : 520;
    int frame_x = landscape ? (screen_w - frame_w) / 2 : 24;
    uint32_t frame_scale = landscape ?
        (uint32_t)((frame_w * LV_SCALE_NONE) / NES_VIEW_W) : LV_SCALE_NONE;
    uint32_t frame_scale_h = landscape ?
        (uint32_t)((frame_h * LV_SCALE_NONE) / NES_VIEW_H) : LV_SCALE_NONE;

    if(landscape) {
        int max_frame_w = screen_w - side_w * 2 - 72;

        if(frame_h > body_h - 44) {
            frame_h = body_h - 44;
        }
        if(frame_h < 300) {
            frame_h = body_h > 320 ? body_h - 32 : 300;
        }
        frame_w = (frame_h * NES_VIEW_W) / NES_VIEW_H;
        if(frame_w > max_frame_w) {
            frame_w = max_frame_w;
            frame_h = (frame_w * NES_VIEW_H) / NES_VIEW_W;
        }
        if(frame_w < 320) {
            frame_w = 320;
            frame_h = (frame_w * NES_VIEW_H) / NES_VIEW_W;
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
        frame_scale = (uint32_t)((frame_w * LV_SCALE_NONE) / NES_VIEW_W);
        frame_scale_h = (uint32_t)((frame_h * LV_SCALE_NONE) / NES_VIEW_H);
    }
    if(landscape && frame_scale_h < frame_scale) {
        frame_scale = frame_scale_h;
    }
    if(frame_scale == 0) {
        frame_scale = LV_SCALE_NONE;
    }

    nes_game_panel = ui_scroll_panel(parent, 0, 0, ui_screen_width(),
                                     body_h);
    lv_obj_set_style_bg_color(nes_game_panel, lv_color_hex(0x080B0F), 0);
    lv_obj_set_style_radius(nes_game_panel, 0, 0);
    lv_obj_set_style_border_width(nes_game_panel, 0, 0);
    lv_obj_add_flag(nes_game_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_scrollbar_mode(nes_game_panel, LV_SCROLLBAR_MODE_OFF);
    if(landscape) {
        lv_obj_clear_flag(nes_game_panel, LV_OBJ_FLAG_SCROLLABLE);
    }

    frame_panel = ui_panel(nes_game_panel, frame_x, frame_y, frame_w, frame_h);
    lv_obj_set_style_bg_color(frame_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(frame_panel, lv_color_hex(0x253043), 0);
    lv_obj_set_style_pad_all(frame_panel, 0, 0);

    nes_frame_image = lv_image_create(frame_panel);
    lv_obj_set_size(nes_frame_image,
                    landscape ? (int)((NES_VIEW_W * frame_scale) / LV_SCALE_NONE) :
                    NES_VIEW_W,
                    landscape ? (int)((NES_VIEW_H * frame_scale) / LV_SCALE_NONE) :
                    NES_VIEW_H);
    lv_image_set_scale(nes_frame_image, frame_scale);
    lv_obj_center(nes_frame_image);
    lv_obj_add_flag(nes_frame_image, LV_OBJ_FLAG_HIDDEN);

    nes_frame_placeholder = ui_label(frame_panel, "NES", &lv_font_montserrat_32,
                                     0x3DA5FF);
    lv_obj_center(nes_frame_placeholder);

    nes_game_status_label = ui_label(nes_game_panel, "Ready",
                                     &lv_font_montserrat_16, 0xD3DAE3);
    lv_obj_set_width(nes_game_status_label, landscape ? frame_w : 380);
    lv_label_set_long_mode(nes_game_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(nes_game_status_label, landscape ? frame_x : 28,
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
        int sys_y = body_h - 58;

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
        if(sys_y + 46 > body_h - 8) {
            sys_y = body_h - 54;
        }

        nes_add_control(nes_game_panel, dpad_x + pad, dpad_y - pad, pad, pad,
                        "Up", 0x1F2937, 0);
        nes_add_control(nes_game_panel, dpad_x + pad, dpad_y + pad, pad, pad,
                        "Down", 0x1F2937, 1);
        nes_add_control(nes_game_panel, dpad_x, dpad_y, pad, pad,
                        "Left", 0x1F2937, 2);
        nes_add_control(nes_game_panel, dpad_x + pad * 2, dpad_y, pad, pad,
                        "Right", 0x1F2937, 3);
        nes_add_control(nes_game_panel, action_x, action_y + ab / 3, ab, ab,
                        "B", 0x2563EB, 7);
        nes_add_control(nes_game_panel, action_x + ab + 18, action_y - ab / 4,
                        ab, ab, "A", 0xEF4D5A, 6);
        nes_add_control(nes_game_panel, margin, sys_y, sys_w, 46,
                        "Select", 0x374151, 4);
        nes_add_control(nes_game_panel, margin + sys_w + 14, sys_y,
                        sys_w, 46, "Start", 0x374151, 5);
    } else {
        nes_add_control(nes_game_panel, 54, 690, 82, 82, "Up", 0x1F2937, 0);
        nes_add_control(nes_game_panel, 54, 874, 82, 82, "Down", 0x1F2937, 1);
        nes_add_control(nes_game_panel, 0, 782, 82, 82, "Left", 0x1F2937, 2);
        nes_add_control(nes_game_panel, 108, 782, 82, 82, "Right", 0x1F2937, 3);
        nes_add_control(nes_game_panel, 205, 875, 84, 58, "Select", 0x374151, 4);
        nes_add_control(nes_game_panel, 300, 875, 84, 58, "Start", 0x374151, 5);
        nes_add_control(nes_game_panel, 355, 740, 94, 94, "B", 0x2563EB, 7);
        nes_add_control(nes_game_panel, 454, 685, 94, 94, "A", 0xEF4D5A, 6);
    }
}

void ui_nes_create(lv_obj_t *scr)
{
    ui_create_header(scr, "NES");
    nes_body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(nes_body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(nes_body, 0, 0);
    lv_obj_set_style_border_width(nes_body, 0, 0);
    lv_obj_set_style_pad_all(nes_body, 0, 0);

    nes_create_library(nes_body);
    nes_create_game(nes_body);
    nes_build_rom_list();
    ui_extension_keyboard_set_key_cb(nes_hardware_key_cb, NULL);
    nes_log("Hardware keyboard mapped: arrows/WASD dpad, Enter Start, Space/Shift Select, U/I/O A, H/J/K B, Esc stop/back");

    if(!nes_ui_timer) {
        nes_ui_timer = lv_timer_create(nes_ui_timer_cb, NES_REFRESH_TIMER_MS, NULL);
    }
}

void ui_nes_cleanup(void)
{
    nes_request_stop();
    if(nes_emulator_thread_valid) {
        pthread_join(nes_emulator_thread, NULL);
        nes_emulator_thread_valid = 0;
    }
    if(nes_ui_timer) {
        lv_timer_delete(nes_ui_timer);
        nes_ui_timer = NULL;
    }
    nes_audio_close();
    nes_stop_tick_thread();
    nes_free_frame_buffers();
    nes_lvgl_pressed_mask = 0;
    nes_touch_pressed_mask = 0;
    nes_keyboard_pressed_mask = 0;
    nes_controller_state = 0xFFFFFFFFU;
    ui_extension_keyboard_set_key_cb(NULL, NULL);
    nes_body = NULL;
    nes_library_panel = NULL;
    nes_game_panel = NULL;
    nes_list_panel = NULL;
    nes_status_label = NULL;
    nes_count_label = NULL;
    nes_title_label = NULL;
    nes_meta_label = NULL;
    nes_path_label = NULL;
    nes_cover_image = NULL;
    nes_cover_placeholder = NULL;
    nes_frame_image = NULL;
    nes_frame_placeholder = NULL;
    nes_game_status_label = NULL;
    memset(nes_control_btn, 0, sizeof(nes_control_btn));
}

int ui_nes_handle_back(void)
{
    if(!nes_game_panel || !lv_obj_is_valid(nes_game_panel) ||
       lv_obj_has_flag(nes_game_panel, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }

    if(nes_emulator_running) {
        nes_request_stop();
        if(nes_game_status_label && lv_obj_is_valid(nes_game_status_label)) {
            lv_label_set_text(nes_game_status_label, "Stopping...");
        }
    } else {
        nes_show_game_view(0);
    }
    app_request_fast_refresh();
    return 1;
}

extern "C" void *mem_alloc(int size, bool prefer_fast_memory)
{
    (void)prefer_fast_memory;
    if(size <= 0) {
        return NULL;
    }
    return malloc((size_t)size);
}

extern "C" void osd_setsound(void (*playfunc)(void *buffer, int size))
{
    nes_audio_playfunc = playfunc;
    if(playfunc) {
        nes_audio_open();
    }
}

extern "C" void osd_audio_frame(void)
{
    int samples;
    size_t bytes;

    if(!nes_audio_playfunc || !nes_audio_pipe || !nes_audio_buffer) {
        return;
    }

    nes_audio_accum += NES_AUDIO_RATE;
    samples = nes_audio_accum / NES_AUDIO_REFRESH;
    nes_audio_accum %= NES_AUDIO_REFRESH;
    if(samples <= 0 || samples > NES_AUDIO_MAX_SAMPLES) {
        samples = NES_AUDIO_RATE / NES_AUDIO_REFRESH;
    }

    memset(nes_audio_buffer, 0, (size_t)samples * sizeof(int16_t));
    nes_audio_playfunc(nes_audio_buffer, samples);

    bytes = (size_t)samples * sizeof(int16_t);
    if(fwrite(nes_audio_buffer, 1, bytes, nes_audio_pipe) != bytes) {
        nes_log("NES audio write failed");
        nes_audio_close();
    }
}

extern "C" void osd_getsoundinfo(sndinfo_t *info)
{
    if(!info) {
        return;
    }
    info->sample_rate = NES_AUDIO_RATE;
    info->bps = 16;
}

static int nes_video_init(int width, int height)
{
    (void)width;
    (void)height;
    return nes_prepare_frame_buffers() ? 0 : -1;
}

static void nes_video_shutdown(void)
{
}

static int nes_video_set_mode(int width, int height)
{
    (void)width;
    (void)height;
    return 0;
}

static void nes_video_set_palette(rgb_t *pal)
{
    if(!pal) {
        return;
    }
    for(int i = 0; i < 256; i++) {
        uint16_t color = (uint16_t)(((pal[i].r >> 3) << 11) |
                                    ((pal[i].g >> 2) << 5) |
                                    (pal[i].b >> 3));
        nes_palette565[i] = color;
    }
}

static void nes_video_clear(uint8 color)
{
    pthread_mutex_lock(&nes_frame_mutex);
    if(nes_frame_back) {
        for(size_t i = 0; i < (size_t)NES_VIEW_W * NES_VIEW_H; i++) {
            nes_frame_back[i] = nes_palette565[color];
        }
        uint16_t *tmp = nes_frame_front;
        nes_frame_front = nes_frame_back;
        nes_frame_back = tmp;
        nes_frame_seq++;
    }
    pthread_mutex_unlock(&nes_frame_mutex);
}

static bitmap_t *nes_video_lock_write(void)
{
    if(nes_lock_bitmap) {
        bmp_destroy(&nes_lock_bitmap);
    }
    nes_lock_bitmap = bmp_createhw(nes_screen_probe, NES_VIEW_W, NES_VIEW_H,
                                   NES_VIEW_W);
    return nes_lock_bitmap;
}

static void nes_video_free_write(int num_dirties, rect_t *dirty_rects)
{
    (void)num_dirties;
    (void)dirty_rects;
    if(nes_lock_bitmap) {
        bmp_destroy(&nes_lock_bitmap);
    }
}

static void nes_video_custom_blit(bitmap_t *bmp, int num_dirties,
                                  rect_t *dirty_rects)
{
    (void)num_dirties;
    (void)dirty_rects;
    if(!bmp || !bmp->line) {
        return;
    }

    pthread_mutex_lock(&nes_frame_mutex);
    if(nes_frame_back) {
        for(int y = 0; y < NES_VIEW_H; y++) {
            int src_y = (y * NES_SCREEN_HEIGHT) / NES_VIEW_H;
            const uint8_t *src = bmp->line[src_y];
            uint16_t *dst = nes_frame_back + y * NES_VIEW_W;
            for(int x = 0; x < NES_VIEW_W; x++) {
                int src_x = (x * NES_SCREEN_WIDTH) / NES_VIEW_W;
                dst[x] = nes_palette565[src[src_x]];
            }
        }
        uint16_t *tmp = nes_frame_front;
        nes_frame_front = nes_frame_back;
        nes_frame_back = tmp;
        nes_frame_seq++;
    }
    pthread_mutex_unlock(&nes_frame_mutex);
}

static viddriver_t nes_video_driver = {
    "K230 LVGL NES",
    nes_video_init,
    nes_video_shutdown,
    nes_video_set_mode,
    nes_video_set_palette,
    nes_video_clear,
    nes_video_lock_write,
    nes_video_free_write,
    nes_video_custom_blit,
    false
};

extern "C" void osd_getvideoinfo(vidinfo_t *info)
{
    if(!info) {
        return;
    }
    info->default_width = NES_SCREEN_WIDTH;
    info->default_height = NES_SCREEN_HEIGHT;
    info->driver = &nes_video_driver;
}

extern "C" int osd_init(void)
{
    nofrendo_log_chain_logfunc(nes_log_print);
    return nes_prepare_frame_buffers() ? 0 : -1;
}

extern "C" void osd_shutdown(void)
{
    nes_audio_close();
    nes_stop_tick_thread();
    if(nes_lock_bitmap) {
        bmp_destroy(&nes_lock_bitmap);
    }
}

extern "C" int osd_main(int argc, char *argv[])
{
    static char config_filename[] = NES_CONFIG_PATH;

    if(argc <= 0 || !argv || !argv[0]) {
        return -1;
    }
    mkdir(NES_ROM_DIR_PRIMARY, 0755);
    config.filename = config_filename;
    return main_loop(argv[0], system_autodetect);
}

extern "C" int osd_installtimer(int frequency, void *func, int funcsize,
                                void *counter, int countersize)
{
    (void)funcsize;
    (void)counter;
    (void)countersize;
    if(frequency <= 0 || !func) {
        return -1;
    }

    nes_stop_tick_thread();
    pthread_mutex_lock(&nes_tick_mutex);
    nes_tick_frequency = frequency;
    nes_tick_func = (void (*)(void))func;
    pthread_mutex_unlock(&nes_tick_mutex);
    nes_tick_thread_stop = 0;
    if(pthread_create(&nes_tick_thread, NULL, nes_tick_thread_main, NULL) != 0) {
        pthread_mutex_lock(&nes_tick_mutex);
        nes_tick_func = NULL;
        nes_tick_frequency = 0;
        pthread_mutex_unlock(&nes_tick_mutex);
        return -1;
    }
    nes_tick_thread_valid = 1;
    return 0;
}

extern "C" void osd_getinput(void)
{
    static const int ev[16] = {
        event_joypad1_up, event_joypad1_down, event_joypad1_left,
        event_joypad1_right, event_joypad1_select, event_joypad1_start,
        event_joypad1_a, event_joypad1_b, event_state_save, event_state_load,
        0, 0, 0, 0, 0, 0
    };
    uint32_t state;
    uint32_t changed;

    if(nes_stop_requested) {
        nes_stop_requested = 0;
        main_soft_quit();
        return;
    }

    state = __atomic_load_n(&nes_controller_state, __ATOMIC_RELAXED);
    changed = state ^ nes_input_old_state;
    nes_input_old_state = state;
    for(int i = 0; i < 16; i++) {
        if((changed & 1U) && ev[i]) {
            event_t handler = event_get(ev[i]);
            if(handler) {
                handler((state & 1U) ? INP_STATE_BREAK : INP_STATE_MAKE);
            }
        }
        changed >>= 1;
        state >>= 1;
    }
}

extern "C" void osd_getmouse(int *x, int *y, int *button)
{
    if(x) {
        *x = 0;
    }
    if(y) {
        *y = 0;
    }
    if(button) {
        *button = 0;
    }
}

extern "C" void osd_fullname(char *fullname, const char *shortname)
{
    if(!fullname || !shortname) {
        return;
    }
    strncpy(fullname, shortname, PATH_MAX);
    fullname[PATH_MAX - 1] = '\0';
}

extern "C" char *osd_newextension(char *string, const char *ext)
{
    char *dot;

    if(!string || !ext) {
        return string;
    }
    dot = strrchr(string, '.');
    if(!dot) {
        dot = string + strlen(string);
    }
    snprintf(dot, PATH_MAX - (dot - string), "%s", ext);
    return string;
}

extern "C" int osd_makesnapname(char *filename, int len)
{
    if(filename && len > 0) {
        filename[0] = '\0';
    }
    return -1;
}
