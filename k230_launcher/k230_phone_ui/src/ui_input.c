#include "ui_input.h"

#include "ui_hardware.h"
#include "ui_i18n.h"
#include "ui_meshtastic.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define UI_INPUT_LOG_PATH "/tmp/k230_input_dialog.log"
#define UI_INPUT_PINYIN_DICT_PATH "/root/app/k230_phone_ui/data/pinyin_common.txt"
#define UI_INPUT_PINYIN_DICT_LINE_MAX 4096
#define UI_INPUT_PINYIN_PAGE_SIZE 9U

typedef enum {
    UI_INPUT_KBD_LOWER = 0,
    UI_INPUT_KBD_UPPER,
    UI_INPUT_KBD_NUM,
    UI_INPUT_KBD_SYMBOL,
    UI_INPUT_KBD_PINYIN,
} ui_input_keyboard_mode_t;

struct ui_input_inline {
    lv_obj_t *overlay;
    lv_obj_t *dialog;
    lv_obj_t *textarea;
    lv_obj_t *eye_button;
    lv_obj_t *eye_label;
    lv_obj_t *error_label;
    lv_obj_t *keyboard;
    lv_obj_t *candidate_bar;
    int password_visible;
    int hardware_keyboard;
    ui_input_keyboard_mode_t keyboard_mode;
    char pinyin_comp[32];
    unsigned int pinyin_page;
    const char *candidate_map[16];
    char candidate_text[11][96];
    size_t min_length;
    const char *min_length_text;
    ui_input_submit_cb_t submit_cb;
    void *user_data;
    int inline_mode;
    lv_obj_t *keyboard_parent;
    ui_input_inline_layout_cb_t layout_cb;
    void *layout_user_data;
};

typedef struct ui_input_inline ui_input_dialog_state_t;

static ui_input_dialog_state_t *active_dialog;
static ui_input_dialog_state_t *active_inline;
static int soft_keyboard_enabled = 1;

static void ui_input_pinyin_update_candidates(ui_input_dialog_state_t *state);
static void ui_input_hardware_key_cb(int code, uint32_t key, int pressed,
                                     void *user_data);

static const char *const ui_input_kbd_lower_map[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "Shift", "123", "PY", "z", "x", "c", "v", "b", "n", "m", "Del", "\n",
    "Cancel", "Space", "Enter", ""
};

static const char *const ui_input_kbd_upper_map[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "123", "PY", "Z", "X", "C", "V", "B", "N", "M", "Del", "\n",
    "Cancel", "Space", "Enter", ""
};

static const char *const ui_input_kbd_num_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
    "abc", "#+=", ".", ",", "?", "!", "'", "Del", "\n",
    "Cancel", "Space", "Enter", ""
};

static const char *const ui_input_kbd_symbol_map[] = {
    "[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
    "_", "\\", "|", "~", "<", ">", "`", ".", ",", "?", "\n",
    "abc", "123", "-", "/", ":", ";", "!", "Del", "\n",
    "Cancel", "Space", "Enter", ""
};

static const char *const ui_input_kbd_pinyin_map[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "abc", "123", "z", "x", "c", "v", "b", "n", "m", "Del", "\n",
    "Cancel", "Space", "Enter", ""
};

typedef struct {
    const char *key;
    const char *candidates[20];
} ui_input_pinyin_entry_t;

typedef struct {
    const char *const *items;
    unsigned int count;
} ui_input_pinyin_candidates_t;

typedef struct {
    char *key;
    char **candidates;
    unsigned int count;
} ui_input_pinyin_dict_entry_t;

static ui_input_pinyin_dict_entry_t *ui_input_pinyin_dict;
static size_t ui_input_pinyin_dict_count;
static int ui_input_pinyin_dict_loaded;

static const ui_input_pinyin_entry_t ui_input_pinyin_table[] = {
    { "ai", { "爱", "矮", "哎", "挨", "碍", NULL } },
    { "ba", { "吧", "八", "把", "爸", "巴", "拔", "罢", NULL } },
    { "bu", { "不", "部", "步", "布", "补", "捕", NULL } },
    { "de", { "的", "得", "地", "德", NULL } },
    { "fa", { "发", "法", "罚", "乏", NULL } },
    { "ge", { "个", "各", "哥", "歌", "格", "隔", NULL } },
    { "guo", { "国", "过", "果", "锅", "郭", "裹", NULL } },
    { "hao", { "好", "号", "浩", "毫", "豪", "耗", NULL } },
    { "he", { "和", "喝", "河", "合", "何", "核", NULL } },
    { "kan", { "看", "砍", "刊", "堪", NULL } },
    { "le", { "了", "乐", "勒", NULL } },
    { "ma", { "吗", "妈", "马", "嘛", "码", "麻", NULL } },
    { "mei", { "没", "美", "每", "妹", "煤", "梅", NULL } },
    { "men", { "们", "门", "闷", NULL } },
    { "ni", { "你", "呢", "尼", "妮", "泥", "拟", "逆", "匿", "腻", NULL } },
    { "qu", { "去", "取", "区", "曲", "趣", NULL } },
    { "ren", { "人", "任", "认", "仁", NULL } },
    { "shi", { "是", "时", "事", "十", "使", "市", "识", "师", "试", "式", "世", NULL } },
    { "shui", { "水", "谁", "睡", "税", NULL } },
    { "ta", { "他", "她", "它", "塔", "踏", NULL } },
    { "wo", { "我", "握", "窝", "卧", "沃", NULL } },
    { "xie", { "谢", "写", "些", "鞋", "协", "斜", NULL } },
    { "yao", { "要", "药", "摇", "腰", "咬", NULL } },
    { "you", { "有", "又", "右", "油", "由", "友", "优", NULL } },
    { "zai", { "在", "再", "载", "灾", "仔", NULL } },
    { "zhong", { "中", "种", "重", "钟", "终", "众", NULL } },
};

static const ui_input_pinyin_entry_t *ui_input_pinyin_find(const char *key);
static int ui_input_pinyin_lookup(const char *key,
                                  ui_input_pinyin_candidates_t *out);
static void ui_input_pinyin_clear(ui_input_dialog_state_t *state);
static void ui_input_pinyin_insert(ui_input_dialog_state_t *state,
                                   const char *text);
static void ui_input_pinyin_commit_best(ui_input_dialog_state_t *state);
static int ui_input_pinyin_commit_candidate(ui_input_dialog_state_t *state,
                                            unsigned int index);
static int ui_input_pinyin_page(ui_input_dialog_state_t *state, int delta);
static void ui_input_hardware_sync_pinyin_mode(ui_input_dialog_state_t *state);
static void ui_input_position_hardware_candidate_bar(ui_input_dialog_state_t *state);
static lv_obj_t *ui_input_create_candidate_bar(lv_obj_t *parent, int height,
                                               int landscape,
                                               ui_input_dialog_state_t *state);

static void ui_input_log(const char *fmt, ...)
{
    FILE *fp = fopen(UI_INPUT_LOG_PATH, "a");
    va_list ap;

    if(!fp) {
        return;
    }
    fprintf(fp, "%llu ", (unsigned long long)ui_monotonic_us());
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\n");
    fclose(fp);
}

void ui_input_set_soft_keyboard_enabled(int enabled)
{
    soft_keyboard_enabled = enabled ? 1 : 0;
}

int ui_input_soft_keyboard_enabled(void)
{
    return soft_keyboard_enabled;
}

void ui_input_dialog_close_active(void)
{
    ui_input_dialog_state_t *state = active_dialog;

    if(!state) {
        return;
    }

    active_dialog = NULL;
    if(state->hardware_keyboard) {
        ui_extension_keyboard_set_key_cb(NULL, NULL);
        state->hardware_keyboard = 0;
    }
    if(state->candidate_bar && lv_obj_is_valid(state->candidate_bar) &&
       (!state->overlay ||
        lv_obj_get_parent(state->candidate_bar) != state->overlay)) {
        lv_obj_delete(state->candidate_bar);
        state->candidate_bar = NULL;
    }
    if(state->overlay && lv_obj_is_valid(state->overlay)) {
        lv_obj_delete(state->overlay);
    }
    free(state);
    app_request_fast_refresh();
}

static int ui_input_inline_reserved_h(ui_input_dialog_state_t *state)
{
    int reserved_h = 0;

    if(!state || !state->inline_mode) {
        return 0;
    }

    if(state->keyboard && lv_obj_is_valid(state->keyboard) &&
       !lv_obj_has_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_update_layout(state->keyboard);
        reserved_h = lv_obj_get_height(state->keyboard);
    }
    if(state->candidate_bar && lv_obj_is_valid(state->candidate_bar) &&
       state->keyboard_mode == UI_INPUT_KBD_PINYIN &&
       !lv_obj_has_flag(state->candidate_bar, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_update_layout(state->candidate_bar);
        reserved_h += lv_obj_get_height(state->candidate_bar) +
                      (reserved_h > 0 ? 6 : 0);
    }
    return reserved_h;
}

static void ui_input_inline_apply_layout(ui_input_dialog_state_t *state,
                                         int active)
{
    int reserved_h = active ? ui_input_inline_reserved_h(state) : 0;

    if(state && state->layout_cb) {
        state->layout_cb(active, reserved_h, state->layout_user_data);
    }
    ui_input_position_hardware_candidate_bar(state);
    app_request_fast_refresh();
}

static void ui_input_inline_hide_state(ui_input_dialog_state_t *state)
{
    if(!state || !state->inline_mode) {
        return;
    }
    if(state->keyboard && lv_obj_is_valid(state->keyboard)) {
        lv_obj_add_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if(state->candidate_bar && lv_obj_is_valid(state->candidate_bar)) {
        lv_obj_add_flag(state->candidate_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if(active_inline == state) {
        active_inline = NULL;
        if(state->hardware_keyboard) {
            ui_extension_keyboard_set_key_cb(NULL, NULL);
        }
    }
    state->pinyin_comp[0] = '\0';
    ui_input_inline_apply_layout(state, 0);
}

static void ui_input_inline_show_state(ui_input_dialog_state_t *state)
{
    if(!state || !state->inline_mode || !state->textarea ||
       !lv_obj_is_valid(state->textarea)) {
        return;
    }

    if(active_inline && active_inline != state) {
        ui_input_inline_hide_state(active_inline);
    }
    active_inline = state;
    if(state->keyboard && lv_obj_is_valid(state->keyboard)) {
        lv_obj_clear_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(state->keyboard);
        if(state->candidate_bar && lv_obj_is_valid(state->candidate_bar)) {
            lv_obj_move_foreground(state->candidate_bar);
        }
        ui_input_pinyin_update_candidates(state);
    } else {
        state->hardware_keyboard = 1;
        ui_input_hardware_sync_pinyin_mode(state);
        if(state->candidate_bar && lv_obj_is_valid(state->candidate_bar)) {
            lv_obj_move_foreground(state->candidate_bar);
        }
        ui_extension_keyboard_set_key_cb(ui_input_hardware_key_cb, state);
    }
    lv_obj_add_state(state->textarea, LV_STATE_FOCUSED);
    ui_input_inline_apply_layout(state, 1);
}

static void ui_input_align_dialog(ui_input_dialog_state_t *state)
{
    int32_t keyboard_h;
    int32_t reserved_h;

    if(!state) {
        return;
    }
    if(state->inline_mode) {
        ui_input_inline_apply_layout(state,
                                     active_inline == state ||
                                     (state->keyboard &&
                                      !lv_obj_has_flag(state->keyboard,
                                                       LV_OBJ_FLAG_HIDDEN)));
        return;
    }
    if(!state->dialog) {
        return;
    }

    if(!state->keyboard) {
        lv_obj_align(state->dialog, LV_ALIGN_CENTER, 0, 0);
        lv_obj_move_foreground(state->dialog);
        if(state->candidate_bar) {
            lv_obj_align(state->candidate_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
            if(state->keyboard_mode == UI_INPUT_KBD_PINYIN) {
                lv_obj_move_foreground(state->candidate_bar);
            }
        }
        return;
    }

    lv_obj_update_layout(state->keyboard);
    keyboard_h = lv_obj_get_height(state->keyboard);
    reserved_h = keyboard_h;
    if(state->candidate_bar &&
       state->keyboard_mode == UI_INPUT_KBD_PINYIN) {
        lv_obj_update_layout(state->candidate_bar);
        reserved_h += lv_obj_get_height(state->candidate_bar) + 6;
    }
    lv_obj_align(state->dialog, LV_ALIGN_BOTTOM_MID, 0, -reserved_h - 12);
    lv_obj_move_foreground(state->dialog);
    if(state->candidate_bar) {
        lv_obj_move_foreground(state->candidate_bar);
    }
    lv_obj_move_foreground(state->keyboard);
}

static void ui_input_submit(ui_input_dialog_state_t *state)
{
    char text[512];
    const char *src;
    size_t len;

    if(!state) {
        return;
    }

    src = state->textarea ? lv_textarea_get_text(state->textarea) : "";
    snprintf(text, sizeof(text), "%s", src ? src : "");
    len = strlen(text);

    if(state->min_length > 0U && len < state->min_length) {
        const char *message = state->min_length_text ?
                              state->min_length_text :
                              "Input is too short";

        if(state->error_label) {
            lv_label_set_text(state->error_label, ui_tr(message));
            lv_obj_clear_flag(state->error_label, LV_OBJ_FLAG_HIDDEN);
        }
        if(state->textarea) {
            lv_obj_add_state(state->textarea, LV_STATE_FOCUSED);
        }
        ui_input_log("reject title-textarea min_len=%u actual=%u",
                     (unsigned)state->min_length, (unsigned)len);
        app_request_fast_refresh();
        return;
    }

    if(state->submit_cb) {
        state->submit_cb(text, state->user_data);
    }
    if(state->inline_mode) {
        if(state->textarea && lv_obj_is_valid(state->textarea)) {
            lv_textarea_set_text(state->textarea, "");
            lv_obj_add_state(state->textarea, LV_STATE_FOCUSED);
        }
        state->pinyin_comp[0] = '\0';
        ui_input_pinyin_update_candidates(state);
        ui_input_inline_apply_layout(state, active_inline == state);
        return;
    }
    ui_input_dialog_close_active();
}

static void ui_input_submit_async(void *user_data)
{
    ui_input_dialog_state_t *state =
        (ui_input_dialog_state_t *)user_data;

    if(state && (state == active_dialog || state == active_inline)) {
        ui_input_submit(state);
    }
}

static void ui_input_cancel_async(void *user_data)
{
    ui_input_dialog_state_t *state =
        (ui_input_dialog_state_t *)user_data;

    if(state && state == active_dialog) {
        ui_input_dialog_close_active();
    } else if(state && state == active_inline) {
        ui_input_inline_hide_state(state);
    }
}

static void ui_input_hardware_key_cb(int code, uint32_t key, int pressed,
                                     void *user_data)
{
    ui_input_dialog_state_t *state =
        (ui_input_dialog_state_t *)user_data;
    size_t comp_len;

    if(!pressed || !state ||
       (!state->inline_mode && state != active_dialog) ||
       (state->inline_mode && state != active_inline) ||
       !state->textarea ||
       !lv_obj_is_valid(state->textarea)) {
        return;
    }

    ui_input_hardware_sync_pinyin_mode(state);
    comp_len = strlen(state->pinyin_comp);
    ui_input_log("hardware-key title-textarea code=%d key=0x%08X mode=%d comp=%s",
                 code, key, state->keyboard_mode, state->pinyin_comp);

    if(code == 11 && key == 0U && app_current_page_is(PAGE_MESHTASTIC)) {
        ui_meshtastic_trigger_voice_key();
        return;
    }

    switch(key) {
    case LV_KEY_ENTER:
        if(state->keyboard_mode == UI_INPUT_KBD_PINYIN && comp_len > 0U) {
            ui_input_pinyin_commit_best(state);
            return;
        }
        lv_async_call(ui_input_submit_async, state);
        return;
    case LV_KEY_ESC:
        lv_async_call(ui_input_cancel_async, state);
        return;
    case LV_KEY_BACKSPACE:
    case LV_KEY_DEL:
        if(state->keyboard_mode == UI_INPUT_KBD_PINYIN && comp_len > 0U) {
            state->pinyin_comp[comp_len - 1U] = '\0';
            state->pinyin_page = 0;
            ui_input_pinyin_update_candidates(state);
        } else {
            lv_textarea_delete_char(state->textarea);
        }
        app_request_fast_refresh();
        return;
    default:
        break;
    }

    if(state->keyboard_mode == UI_INPUT_KBD_PINYIN) {
        if((key == LV_KEY_RIGHT || key == LV_KEY_DOWN) &&
           ui_input_pinyin_page(state, 1)) {
            return;
        }
        if((key == LV_KEY_LEFT || key == LV_KEY_UP) &&
           ui_input_pinyin_page(state, -1)) {
            return;
        }
        if(key == ' ') {
            if(comp_len > 0U) {
                ui_input_pinyin_commit_best(state);
            } else {
                lv_textarea_add_char(state->textarea, ' ');
                app_request_fast_refresh();
            }
            return;
        }
        if(comp_len > 0U && key >= '1' && key <= '9' &&
           ui_input_pinyin_commit_candidate(state, (unsigned int)(key - '1'))) {
            return;
        }
        if(key >= 32U && key <= 126U &&
           isalpha((unsigned char)key)) {
            if(comp_len + 1U < sizeof(state->pinyin_comp)) {
                state->pinyin_comp[comp_len] =
                    (char)tolower((unsigned char)key);
                state->pinyin_comp[comp_len + 1U] = '\0';
                state->pinyin_page = 0;
                ui_input_pinyin_update_candidates(state);
                app_request_fast_refresh();
            }
            return;
        }
        if(key >= 32U && key <= 126U) {
            if(comp_len > 0U) {
                ui_input_pinyin_commit_best(state);
            }
            lv_textarea_add_char(state->textarea, key);
            app_request_fast_refresh();
            return;
        }
    } else if(key >= 32U && key <= 126U) {
        lv_textarea_add_char(state->textarea, key);
        app_request_fast_refresh();
    }
}

static int ui_input_is_keyboard_mode_key(const char *text)
{
    return text &&
           (strcmp(text, "123") == 0 || strcmp(text, "abc") == 0 ||
            strcmp(text, "Shift") == 0 || strcmp(text, "ABC") == 0 ||
            strcmp(text, "#+=") == 0 || strcmp(text, "PY") == 0);
}

static const ui_input_pinyin_entry_t *ui_input_pinyin_find(const char *key)
{
    if(!key || !key[0]) {
        return NULL;
    }
    for(size_t i = 0; i < sizeof(ui_input_pinyin_table) /
                       sizeof(ui_input_pinyin_table[0]); i++) {
        if(strcmp(ui_input_pinyin_table[i].key, key) == 0) {
            return &ui_input_pinyin_table[i];
        }
    }
    return NULL;
}

static char *ui_input_strdup_local(const char *src)
{
    size_t len;
    char *copy;

    if(!src) {
        return NULL;
    }
    len = strlen(src);
    copy = malloc(len + 1U);
    if(copy) {
        memcpy(copy, src, len + 1U);
    }
    return copy;
}

static void ui_input_pinyin_dict_append(ui_input_pinyin_dict_entry_t entry)
{
    ui_input_pinyin_dict_entry_t *next;

    next = realloc(ui_input_pinyin_dict,
                   (ui_input_pinyin_dict_count + 1U) *
                   sizeof(*ui_input_pinyin_dict));
    if(!next) {
        free(entry.key);
        for(unsigned int i = 0; i < entry.count; i++) {
            free(entry.candidates[i]);
        }
        free(entry.candidates);
        return;
    }
    ui_input_pinyin_dict = next;
    ui_input_pinyin_dict[ui_input_pinyin_dict_count++] = entry;
}

static int ui_input_pinyin_dict_add_candidate(ui_input_pinyin_dict_entry_t *entry,
                                              const char *candidate)
{
    char **next;

    if(!entry || !candidate || !candidate[0]) {
        return -1;
    }
    next = realloc(entry->candidates,
                   (entry->count + 1U) * sizeof(*entry->candidates));
    if(!next) {
        return -1;
    }
    entry->candidates = next;
    entry->candidates[entry->count] = ui_input_strdup_local(candidate);
    if(!entry->candidates[entry->count]) {
        return -1;
    }
    entry->count++;
    return 0;
}

static void ui_input_pinyin_load_dict(void)
{
    FILE *fp;
    char line[UI_INPUT_PINYIN_DICT_LINE_MAX];
    unsigned int line_no = 0;

    if(ui_input_pinyin_dict_loaded) {
        return;
    }
    ui_input_pinyin_dict_loaded = 1;

    fp = fopen(UI_INPUT_PINYIN_DICT_PATH, "r");
    if(!fp) {
        ui_input_log("pinyin dict missing path=%s", UI_INPUT_PINYIN_DICT_PATH);
        return;
    }

    while(fgets(line, sizeof(line), fp)) {
        ui_input_pinyin_dict_entry_t entry = {0};
        char *tab;
        char *saveptr = NULL;
        char *token;
        size_t len;

        line_no++;
        len = strlen(line);
        while(len > 0U &&
              (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
            line[--len] = '\0';
        }
        if(!line[0] || line[0] == '#') {
            continue;
        }
        tab = strchr(line, '\t');
        if(!tab) {
            continue;
        }
        *tab = '\0';
        entry.key = ui_input_strdup_local(line);
        if(!entry.key) {
            continue;
        }
        token = strtok_r(tab + 1, " ", &saveptr);
        while(token) {
            if(ui_input_pinyin_dict_add_candidate(&entry, token) != 0) {
                break;
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
        if(entry.count > 0U) {
            ui_input_pinyin_dict_append(entry);
        } else {
            free(entry.key);
            free(entry.candidates);
        }
    }
    fclose(fp);
    ui_input_log("pinyin dict loaded path=%s keys=%u lines=%u",
                 UI_INPUT_PINYIN_DICT_PATH,
                 (unsigned)ui_input_pinyin_dict_count, line_no);
}

static const ui_input_pinyin_dict_entry_t *
ui_input_pinyin_dict_find(const char *key)
{
    size_t left = 0;
    size_t right;

    ui_input_pinyin_load_dict();
    right = ui_input_pinyin_dict_count;
    while(left < right) {
        size_t mid = left + (right - left) / 2U;
        int cmp = strcmp(key, ui_input_pinyin_dict[mid].key);

        if(cmp == 0) {
            return &ui_input_pinyin_dict[mid];
        }
        if(cmp < 0) {
            right = mid;
        } else {
            left = mid + 1U;
        }
    }
    return NULL;
}

static int ui_input_pinyin_lookup(const char *key,
                                  ui_input_pinyin_candidates_t *out)
{
    const ui_input_pinyin_dict_entry_t *dict_entry;
    const ui_input_pinyin_entry_t *entry;
    unsigned int count = 0;

    if(out) {
        out->items = NULL;
        out->count = 0;
    }
    if(!key || !key[0] || !out) {
        return 0;
    }

    dict_entry = ui_input_pinyin_dict_find(key);
    if(dict_entry && dict_entry->count > 0U) {
        out->items = (const char *const *)dict_entry->candidates;
        out->count = dict_entry->count;
        return 1;
    }

    entry = ui_input_pinyin_find(key);
    if(!entry) {
        return 0;
    }
    while(count < (unsigned int)(sizeof(entry->candidates) /
                                 sizeof(entry->candidates[0])) &&
          entry->candidates[count]) {
        count++;
    }
    if(count > 0U) {
        out->items = entry->candidates;
        out->count = count;
        return 1;
    }
    return 0;
}

static void ui_input_pinyin_update_candidates(ui_input_dialog_state_t *state)
{
    ui_input_pinyin_candidates_t candidates;
    unsigned int count;
    unsigned int start;
    unsigned int shown = 0;
    size_t map_i = 0;
    size_t text_i = 0;

    if(!state || !state->candidate_bar) {
        return;
    }

    if(state->inline_mode && active_inline != state) {
        lv_obj_add_flag(state->candidate_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if(state->keyboard_mode != UI_INPUT_KBD_PINYIN) {
        lv_obj_add_flag(state->candidate_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    ui_input_pinyin_lookup(state->pinyin_comp, &candidates);
    count = candidates.count;
    if(!count && state->pinyin_comp[0]) {
        count = 1;
    }
    if(count) {
        unsigned int max_page = (count - 1U) / UI_INPUT_PINYIN_PAGE_SIZE;

        if(state->pinyin_page > max_page) {
            state->pinyin_page = max_page;
        }
    } else {
        state->pinyin_page = 0;
    }
    start = state->pinyin_page * UI_INPUT_PINYIN_PAGE_SIZE;

    lv_obj_clear_flag(state->candidate_bar, LV_OBJ_FLAG_HIDDEN);
    snprintf(state->candidate_text[text_i], sizeof(state->candidate_text[text_i]),
             "%s", state->pinyin_comp[0] ? state->pinyin_comp : "Pinyin");
    state->candidate_map[map_i++] = state->candidate_text[text_i++];

    if(candidates.count > 0U) {
        for(unsigned int i = start;
            i < count && shown < UI_INPUT_PINYIN_PAGE_SIZE &&
            map_i < (sizeof(state->candidate_map) /
                     sizeof(state->candidate_map[0])) - 1U &&
            text_i < sizeof(state->candidate_text) /
                     sizeof(state->candidate_text[0]);
            i++, shown++) {
            snprintf(state->candidate_text[text_i],
                     sizeof(state->candidate_text[text_i]), "%u %s",
                     shown + 1U, candidates.items[i]);
            state->candidate_map[map_i++] = state->candidate_text[text_i++];
        }
    } else if(state->pinyin_comp[0] && map_i < 7) {
        snprintf(state->candidate_text[text_i],
                 sizeof(state->candidate_text[text_i]), "1 %s",
                 state->pinyin_comp);
        state->candidate_map[map_i++] = state->candidate_text[text_i++];
    }

    state->candidate_map[map_i] = "";
    lv_buttonmatrix_set_map(state->candidate_bar, state->candidate_map);
    ui_input_position_hardware_candidate_bar(state);
    ui_input_log("pinyin candidates mode=%d inline=%d active=%d comp=%s page=%u count=%u shown=%u",
                 state->keyboard_mode, state->inline_mode,
                 state == active_inline, state->pinyin_comp,
                 state->pinyin_page, count, shown);
}

static void ui_input_position_hardware_candidate_bar(ui_input_dialog_state_t *state)
{
    lv_area_t area;
    int screen_w;
    int screen_h;
    int bar_w;
    int bar_h;
    int x;
    int y;
    int min_w;

    if(!state || !state->hardware_keyboard || state->keyboard ||
       !state->candidate_bar || !lv_obj_is_valid(state->candidate_bar) ||
       lv_obj_has_flag(state->candidate_bar, LV_OBJ_FLAG_HIDDEN) ||
       !state->textarea || !lv_obj_is_valid(state->textarea)) {
        return;
    }

    screen_w = ui_screen_width();
    screen_h = ui_screen_height();
    lv_obj_update_layout(state->textarea);
    lv_obj_update_layout(state->candidate_bar);
    lv_obj_get_coords(state->textarea, &area);

    bar_w = (int)(area.x2 - area.x1 + 1);
    bar_h = lv_obj_get_height(state->candidate_bar);
    if(bar_h < 36) {
        bar_h = ui_is_landscape() ? 42 : 52;
    }
    min_w = ui_is_landscape() ? 720 : 500;
    if(bar_w < min_w) {
        bar_w = min_w;
    }
    if(bar_w > screen_w - 24) {
        bar_w = screen_w - 24;
    }

    x = (int)(area.x1 + area.x2 + 1 - bar_w) / 2;
    if(x + bar_w > screen_w - 12) {
        x = screen_w - 12 - bar_w;
    }
    if(x < 12) {
        x = 12;
    }

    y = (int)area.y1 - bar_h - 6;
    if(y < 8) {
        y = (int)area.y2 + 6;
    }
    if(y + bar_h > screen_h - 8) {
        y = screen_h - 8 - bar_h;
    }
    if(y < 8) {
        y = 8;
    }

    lv_obj_set_size(state->candidate_bar, bar_w, bar_h);
    lv_obj_set_pos(state->candidate_bar, x, y);
    lv_obj_move_foreground(state->candidate_bar);
}

static void ui_input_keyboard_set_mode(ui_input_dialog_state_t *state,
                                       ui_input_keyboard_mode_t mode)
{
    if(!state) {
        return;
    }

    state->keyboard_mode = mode;
    if(mode != UI_INPUT_KBD_PINYIN) {
        state->pinyin_comp[0] = '\0';
    }
    if(state->hardware_keyboard && ui_extension_keyboard_active()) {
        ui_extension_keyboard_set_pinyin_enabled(mode == UI_INPUT_KBD_PINYIN);
    }
    if(state->keyboard) {
        switch(mode) {
        case UI_INPUT_KBD_UPPER:
            lv_buttonmatrix_set_map(state->keyboard, ui_input_kbd_upper_map);
            break;
        case UI_INPUT_KBD_NUM:
            lv_buttonmatrix_set_map(state->keyboard, ui_input_kbd_num_map);
            break;
        case UI_INPUT_KBD_SYMBOL:
            lv_buttonmatrix_set_map(state->keyboard, ui_input_kbd_symbol_map);
            break;
        case UI_INPUT_KBD_PINYIN:
            lv_buttonmatrix_set_map(state->keyboard, ui_input_kbd_pinyin_map);
            break;
        case UI_INPUT_KBD_LOWER:
        default:
            lv_buttonmatrix_set_map(state->keyboard, ui_input_kbd_lower_map);
            break;
        }
    }
    ui_input_pinyin_update_candidates(state);
    ui_input_align_dialog(state);
    app_request_fast_refresh();
}

static void ui_input_hardware_sync_pinyin_mode(ui_input_dialog_state_t *state)
{
    ui_input_keyboard_mode_t mode;

    if(!state || !state->hardware_keyboard) {
        return;
    }
    mode = ui_extension_keyboard_pinyin_enabled() ?
           UI_INPUT_KBD_PINYIN : UI_INPUT_KBD_LOWER;
    if(state->keyboard_mode == mode) {
        if(mode == UI_INPUT_KBD_PINYIN) {
            ui_input_pinyin_update_candidates(state);
            ui_input_align_dialog(state);
        }
        return;
    }
    state->keyboard_mode = mode;
    if(mode != UI_INPUT_KBD_PINYIN) {
        state->pinyin_comp[0] = '\0';
    }
    ui_input_pinyin_update_candidates(state);
    ui_input_align_dialog(state);
    app_request_fast_refresh();
}

static void ui_input_pinyin_clear(ui_input_dialog_state_t *state)
{
    if(!state) {
        return;
    }
    state->pinyin_comp[0] = '\0';
    state->pinyin_page = 0;
    ui_input_pinyin_update_candidates(state);
    app_request_fast_refresh();
}

static void ui_input_pinyin_insert(ui_input_dialog_state_t *state,
                                   const char *text)
{
    if(!state || !state->textarea || !text || !text[0]) {
        return;
    }
    lv_textarea_add_text(state->textarea, text);
    state->pinyin_comp[0] = '\0';
    state->pinyin_page = 0;
    if(state->keyboard_mode == UI_INPUT_KBD_PINYIN && state->keyboard) {
        lv_buttonmatrix_set_map(state->keyboard, ui_input_kbd_pinyin_map);
    }
    ui_input_pinyin_update_candidates(state);
    ui_input_align_dialog(state);
    app_request_fast_refresh();
}

static void ui_input_pinyin_commit_best(ui_input_dialog_state_t *state)
{
    ui_input_pinyin_candidates_t candidates;

    if(!state || !state->pinyin_comp[0]) {
        return;
    }
    ui_input_pinyin_lookup(state->pinyin_comp, &candidates);
    if(candidates.count > 0U && candidates.items[0]) {
        ui_input_pinyin_insert(state, candidates.items[0]);
    } else {
        ui_input_pinyin_insert(state, state->pinyin_comp);
    }
}

static int ui_input_pinyin_commit_candidate(ui_input_dialog_state_t *state,
                                            unsigned int index)
{
    ui_input_pinyin_candidates_t candidates;
    unsigned int real_index;

    if(!state || !state->pinyin_comp[0]) {
        return 0;
    }
    ui_input_pinyin_lookup(state->pinyin_comp, &candidates);
    if(candidates.count == 0U) {
        if(index == 0U) {
            ui_input_pinyin_insert(state, state->pinyin_comp);
            return 1;
        }
        return 0;
    }
    real_index = state->pinyin_page * UI_INPUT_PINYIN_PAGE_SIZE + index;
    if(real_index < candidates.count && candidates.items[real_index]) {
        ui_input_pinyin_insert(state, candidates.items[real_index]);
        return 1;
    }
    return 0;
}

static int ui_input_pinyin_page(ui_input_dialog_state_t *state, int delta)
{
    ui_input_pinyin_candidates_t candidates;
    unsigned int count;
    unsigned int max_page;

    if(!state || !state->pinyin_comp[0]) {
        return 0;
    }
    ui_input_pinyin_lookup(state->pinyin_comp, &candidates);
    count = candidates.count;
    if(count <= UI_INPUT_PINYIN_PAGE_SIZE) {
        return 0;
    }
    max_page = (count - 1U) / UI_INPUT_PINYIN_PAGE_SIZE;
    if(delta > 0) {
        if(state->pinyin_page >= max_page) {
            state->pinyin_page = 0;
        } else {
            state->pinyin_page++;
        }
    } else if(delta < 0) {
        if(state->pinyin_page == 0U) {
            state->pinyin_page = max_page;
        } else {
            state->pinyin_page--;
        }
    }
    ui_input_pinyin_update_candidates(state);
    app_request_fast_refresh();
    return 1;
}

static void ui_input_keyboard_send_button(ui_input_dialog_state_t *state,
                                          const char *text)
{
    if(!state || !text || !text[0]) {
        return;
    }

    if(strcmp(text, "123") == 0) {
        ui_input_keyboard_set_mode(state, UI_INPUT_KBD_NUM);
    } else if(strcmp(text, "abc") == 0) {
        ui_input_keyboard_set_mode(state, UI_INPUT_KBD_LOWER);
    } else if(strcmp(text, "ABC") == 0 ||
              strcmp(text, "Shift") == 0) {
        ui_input_keyboard_set_mode(state, UI_INPUT_KBD_UPPER);
    } else if(strcmp(text, "#+=") == 0) {
        ui_input_keyboard_set_mode(state, UI_INPUT_KBD_SYMBOL);
    } else if(strcmp(text, "PY") == 0) {
        ui_input_keyboard_set_mode(state, UI_INPUT_KBD_PINYIN);
    } else if(strcmp(text, "Cancel") == 0) {
        if(state->inline_mode) {
            ui_input_inline_hide_state(state);
        } else {
            ui_input_dialog_close_active();
        }
    } else if(strcmp(text, "Enter") == 0) {
        ui_input_submit(state);
    } else if(strcmp(text, "Del") == 0) {
        size_t len = strlen(state->pinyin_comp);

        if(state->keyboard_mode == UI_INPUT_KBD_PINYIN && len > 0U) {
            state->pinyin_comp[len - 1U] = '\0';
            state->pinyin_page = 0;
            ui_input_pinyin_update_candidates(state);
        } else if(state->textarea) {
            lv_textarea_delete_char(state->textarea);
        }
        app_request_fast_refresh();
    } else if(strcmp(text, "Space") == 0) {
        if(state->keyboard_mode == UI_INPUT_KBD_PINYIN &&
           state->pinyin_comp[0]) {
            ui_input_pinyin_commit_best(state);
        } else if(state->textarea) {
            lv_textarea_add_char(state->textarea, ' ');
            app_request_fast_refresh();
        }
    } else if(state->keyboard_mode == UI_INPUT_KBD_PINYIN &&
              strlen(text) == 1U &&
              isalpha((unsigned char)text[0])) {
        size_t len = strlen(state->pinyin_comp);

        if(len + 1U < sizeof(state->pinyin_comp)) {
            state->pinyin_comp[len] =
                (char)tolower((unsigned char)text[0]);
            state->pinyin_comp[len + 1U] = '\0';
            state->pinyin_page = 0;
            ui_input_pinyin_update_candidates(state);
            app_request_fast_refresh();
        }
    } else if(!ui_input_is_keyboard_mode_key(text) && state->textarea) {
        lv_textarea_add_text(state->textarea, text);
        app_request_fast_refresh();
    }
}

static void ui_input_keyboard_event_cb(lv_event_t *event)
{
    ui_input_dialog_state_t *state =
        (ui_input_dialog_state_t *)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if(code == LV_EVENT_VALUE_CHANGED && state && state->keyboard) {
        uint32_t btn_id = lv_buttonmatrix_get_selected_button(state->keyboard);
        const char *text;

        if(btn_id == LV_BUTTONMATRIX_BUTTON_NONE) {
            return;
        }
        text = lv_buttonmatrix_get_button_text(state->keyboard, btn_id);
        ui_input_keyboard_send_button(state, text);
    }
}

static void ui_input_candidate_event_cb(lv_event_t *event)
{
    ui_input_dialog_state_t *state =
        (ui_input_dialog_state_t *)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    uint32_t btn_id;
    const char *text;
    const char *candidate_text;

    if(code != LV_EVENT_VALUE_CHANGED || !state || !state->candidate_bar) {
        return;
    }

    btn_id = lv_buttonmatrix_get_selected_button(state->candidate_bar);
    if(btn_id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    text = lv_buttonmatrix_get_button_text(state->candidate_bar, btn_id);
    if(!text || !text[0] || strcmp(text, "Pinyin") == 0) {
        return;
    }
    if(strcmp(text, "Clear") == 0) {
        ui_input_pinyin_clear(state);
    } else if(strcmp(text, "Input") == 0) {
        if(state->pinyin_comp[0]) {
            ui_input_pinyin_insert(state, state->pinyin_comp);
        }
    } else if(isdigit((unsigned char)text[0]) && text[1] == ' ') {
        candidate_text = text + 2;
        if(candidate_text[0]) {
            ui_input_pinyin_insert(state, candidate_text);
        }
    } else if(strcmp(text, state->pinyin_comp) != 0) {
        ui_input_pinyin_insert(state, text);
    }
}

static void ui_input_event_cb(lv_event_t *event)
{
    ui_input_dialog_state_t *state =
        (ui_input_dialog_state_t *)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if(!state) {
        return;
    }

    if(code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        if(state->inline_mode) {
            ui_input_inline_show_state(state);
        } else if(state->keyboard && state->textarea) {
            lv_obj_clear_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN);
            ui_input_align_dialog(state);
        } else if(state->hardware_keyboard && state->textarea) {
            lv_obj_add_state(state->textarea, LV_STATE_FOCUSED);
        }
    } else if(code == LV_EVENT_READY) {
        ui_input_submit(state);
    } else if(code == LV_EVENT_CANCEL) {
        if(state->inline_mode) {
            ui_input_inline_hide_state(state);
        } else {
            ui_input_dialog_close_active();
        }
    }
}

static void ui_input_button_event_cb(lv_event_t *event)
{
    ui_input_dialog_state_t *state =
        (ui_input_dialog_state_t *)lv_event_get_user_data(event);
    const char *action = (const char *)lv_event_get_param(event);

    (void)action;
    ui_input_submit(state);
}

static void ui_input_cancel_button_event_cb(lv_event_t *event)
{
    (void)event;
    ui_input_dialog_close_active();
}

static void ui_input_password_toggle_event_cb(lv_event_t *event)
{
    ui_input_dialog_state_t *state =
        (ui_input_dialog_state_t *)lv_event_get_user_data(event);

    if(!state || !state->textarea || !state->eye_label) {
        return;
    }

    state->password_visible = !state->password_visible;
    lv_textarea_set_password_mode(state->textarea,
                                  state->password_visible ? false : true);
    lv_label_set_text(state->eye_label,
                      state->password_visible ? LV_SYMBOL_EYE_CLOSE :
                      LV_SYMBOL_EYE_OPEN);
    lv_obj_add_state(state->textarea, LV_STATE_FOCUSED);
    if(state->hardware_keyboard) {
        lv_obj_add_state(state->textarea, LV_STATE_FOCUSED);
    }
    app_request_fast_refresh();
}

static void ui_input_style_keyboard(lv_obj_t *obj, int landscape)
{
    lv_obj_set_style_text_font(obj,
                               ui_font_for_text("keyboard",
                                                landscape ?
                                                &lv_font_montserrat_14 :
                                                &lv_font_montserrat_16),
                               LV_PART_MAIN);
    lv_obj_set_style_text_font(obj,
                               ui_font_for_text("keyboard",
                                                landscape ?
                                                &lv_font_montserrat_14 :
                                                &lv_font_montserrat_16),
                               LV_PART_ITEMS);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, landscape ? 4 : 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(obj, landscape ? 4 : 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(obj, landscape ? 4 : 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x1C2530), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x2F3C4A),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xF2F5F8), LV_PART_ITEMS);
    lv_obj_set_style_border_width(obj, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(obj, 8, LV_PART_ITEMS);
}

static lv_obj_t *ui_input_create_candidate_bar(lv_obj_t *parent, int height,
                                               int landscape,
                                               ui_input_dialog_state_t *state)
{
    lv_obj_t *bar;

    if(!parent || !state) {
        return NULL;
    }
    bar = lv_buttonmatrix_create(parent);
    lv_obj_set_size(bar, ui_screen_width(), height);
    ui_input_style_keyboard(bar, landscape);
    lv_obj_set_style_text_font(bar,
                               ui_font_for_text("中文",
                                                &lv_font_montserrat_16),
                               LV_PART_MAIN);
    lv_obj_set_style_text_font(bar,
                               ui_font_for_text("中文",
                                                &lv_font_montserrat_16),
                               LV_PART_ITEMS);
    lv_obj_add_event_cb(bar, ui_input_candidate_event_cb,
                        LV_EVENT_VALUE_CHANGED, state);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    return bar;
}

void ui_input_dialog_open(const ui_input_dialog_config_t *config)
{
    ui_input_dialog_state_t *state;
    lv_obj_t *title;
    lv_obj_t *btn;
    int landscape = ui_is_landscape();
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int keyboard_h = landscape ? 156 : 330;
    int candidate_h = landscape ? 42 : 52;
    int dialog_w = landscape ? screen_w - 240 : 520;
    int dialog_h;
    int content_w;
    int textarea_w;
    int button_w;
    int button_gap = 16;
    int pad = landscape ? 20 : 22;
    int textarea_y;
    int textarea_h = 46;
    int error_y;
    int button_y;
    int button_h = 52;
    int use_soft_keyboard;

    if(!config) {
        return;
    }
    if(dialog_w > 880) {
        dialog_w = 880;
    }
    if(dialog_w < 500) {
        dialog_w = 500;
    }
    if(!landscape && dialog_w > screen_w - 48) {
        dialog_w = screen_w - 48;
    }
    if(keyboard_h > screen_h / 2) {
        keyboard_h = screen_h / 2;
    }
    content_w = dialog_w - pad * 2;
    textarea_w = content_w - (config->password_mode ? 70 : 0);
    button_w = (content_w - button_gap) / 2;
    textarea_y = pad + 50;
    error_y = textarea_y + textarea_h + 8;
    button_y = error_y + 32;
    dialog_h = button_y + button_h + pad;

    ui_input_dialog_close_active();
    use_soft_keyboard = ui_input_soft_keyboard_enabled() ||
                        !ui_extension_keyboard_active();
    ui_input_log("open title=%s password=%d soft_pref=%d hw_active=%d use_soft=%d",
                 config->title ? config->title : "Input",
                 config->password_mode ? 1 : 0,
                 ui_input_soft_keyboard_enabled(),
                 ui_extension_keyboard_active(),
                 use_soft_keyboard);

    state = calloc(1, sizeof(*state));
    if(!state) {
        return;
    }
    active_dialog = state;
    state->submit_cb = config->submit_cb;
    state->user_data = config->user_data;
    state->min_length = config->min_length;
    state->min_length_text = config->min_length_text;

    state->overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(state->overlay);
    lv_obj_align(state->overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(state->overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(state->overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(state->overlay, 0, 0);
    lv_obj_set_style_pad_all(state->overlay, 0, 0);
    lv_obj_clear_flag(state->overlay, LV_OBJ_FLAG_SCROLLABLE);

    state->dialog = ui_panel(state->overlay, 0, 0, dialog_w, dialog_h);
    lv_obj_set_style_bg_color(state->dialog, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(state->dialog, 16, 0);
    lv_obj_set_style_border_width(state->dialog, 1, 0);
    lv_obj_set_style_border_color(state->dialog, lv_color_hex(0x293644), 0);
    lv_obj_set_style_shadow_width(state->dialog, 18, 0);
    lv_obj_set_style_shadow_opa(state->dialog, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(state->dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(state->dialog, 0, 0);

    title = ui_label(state->dialog, config->title ? config->title : "Input",
                     &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_width(title, content_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title, pad, pad);

    state->textarea = lv_textarea_create(state->dialog);
    lv_obj_set_pos(state->textarea, pad, textarea_y);
    lv_obj_set_size(state->textarea, textarea_w, textarea_h);
    lv_textarea_set_one_line(state->textarea, true);
    lv_textarea_set_password_mode(state->textarea, config->password_mode ? true : false);
    lv_textarea_set_text(state->textarea,
                         config->initial_text ? config->initial_text : "");
    lv_textarea_set_max_length(state->textarea,
                               config->max_length ? (uint32_t)config->max_length :
                               NET_PASS_MAX - 1U);
    lv_textarea_set_placeholder_text(state->textarea,
                                     ui_tr(config->placeholder ?
                                           config->placeholder : ""));
    lv_obj_set_style_text_font(state->textarea,
                               ui_font_for_text("input", &lv_font_montserrat_18),
                               0);
    lv_obj_set_style_bg_color(state->textarea, lv_color_hex(0x1A222C), 0);
    lv_obj_set_style_text_color(state->textarea, lv_color_hex(0xF2F5F8), 0);
    lv_obj_set_style_radius(state->textarea, 12, 0);
    lv_obj_set_style_pad_left(state->textarea, 14, 0);
    lv_obj_set_style_pad_right(state->textarea, 14, 0);
    lv_obj_set_style_pad_top(state->textarea, 8, 0);
    lv_obj_set_style_pad_bottom(state->textarea, 8, 0);
    lv_obj_set_style_border_color(state->textarea, lv_color_hex(0x3DA5FF),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(state->textarea, 1, 0);
    lv_obj_add_event_cb(state->textarea, ui_input_event_cb, LV_EVENT_FOCUSED, state);
    lv_obj_add_event_cb(state->textarea, ui_input_event_cb, LV_EVENT_CLICKED, state);
    lv_obj_add_event_cb(state->textarea, ui_input_event_cb, LV_EVENT_READY, state);
    lv_obj_add_event_cb(state->textarea, ui_input_event_cb, LV_EVENT_CANCEL, state);

    if(config->password_mode) {
        state->eye_button = lv_obj_create(state->dialog);
        lv_obj_set_pos(state->eye_button, pad + textarea_w + 12, textarea_y);
        lv_obj_set_size(state->eye_button, textarea_h, textarea_h);
        lv_obj_set_style_bg_color(state->eye_button, lv_color_hex(0x1A222C), 0);
        lv_obj_set_style_bg_color(state->eye_button, lv_color_hex(0x263241),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(state->eye_button, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(state->eye_button, 12, 0);
        lv_obj_set_style_border_width(state->eye_button, 1, 0);
        lv_obj_set_style_border_color(state->eye_button, lv_color_hex(0x2A3A4A), 0);
        lv_obj_clear_flag(state->eye_button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(state->eye_button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(state->eye_button, 8);
        lv_obj_add_event_cb(state->eye_button,
                            ui_input_password_toggle_event_cb,
                            LV_EVENT_CLICKED, state);

        state->eye_label = ui_label(state->eye_button, LV_SYMBOL_EYE_OPEN,
                                    &lv_font_montserrat_22, 0xF2F5F8);
        lv_obj_center(state->eye_label);
        ui_make_click_forwarder(state->eye_label);
    }

    state->error_label = ui_label(state->dialog, "", &lv_font_montserrat_14,
                                  0xEF4D5A);
    lv_obj_set_pos(state->error_label, pad, textarea_y + textarea_h + 8);
    lv_obj_set_width(state->error_label, content_w);
    lv_label_set_long_mode(state->error_label, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(state->error_label, LV_OBJ_FLAG_HIDDEN);

    btn = ui_command_button(state->dialog, pad, button_y, button_w,
                            config->cancel_text ? config->cancel_text : "Cancel",
                            0x9AA4AF);
    lv_obj_set_height(btn, button_h);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A222C), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2A3644), 0);
    lv_obj_add_event_cb(btn, ui_input_cancel_button_event_cb, LV_EVENT_CLICKED,
                        state);

    btn = ui_command_button(state->dialog, pad + button_w + button_gap,
                            button_y, button_w,
                            config->submit_text ? config->submit_text : "Connect",
                            0x25C281);
    lv_obj_set_height(btn, button_h);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x123326), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x25C281), 0);
    lv_obj_add_event_cb(btn, ui_input_button_event_cb, LV_EVENT_CLICKED, state);

    if(use_soft_keyboard) {
        state->candidate_bar =
            ui_input_create_candidate_bar(state->overlay, candidate_h,
                                          landscape, state);
        lv_obj_align(state->candidate_bar, LV_ALIGN_BOTTOM_MID, 0, -keyboard_h);

        state->keyboard = lv_buttonmatrix_create(state->overlay);
        lv_obj_set_size(state->keyboard, ui_screen_width(), keyboard_h);
        lv_obj_align(state->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        ui_input_style_keyboard(state->keyboard, landscape);
        lv_obj_add_event_cb(state->keyboard, ui_input_keyboard_event_cb,
                            LV_EVENT_VALUE_CHANGED,
                            state);
        ui_input_keyboard_set_mode(state, UI_INPUT_KBD_LOWER);
    } else {
        state->candidate_bar =
            ui_input_create_candidate_bar(lv_layer_top(), candidate_h,
                                          landscape, state);
        if(state->candidate_bar) {
            lv_obj_set_pos(state->candidate_bar, 0, 0);
        }
        state->hardware_keyboard = 1;
        ui_input_hardware_sync_pinyin_mode(state);
        ui_extension_keyboard_set_key_cb(ui_input_hardware_key_cb, state);
        lv_obj_add_state(state->textarea, LV_STATE_FOCUSED);
    }

    ui_input_align_dialog(state);
    lv_obj_add_state(state->textarea, LV_STATE_FOCUSED);
    if(state->keyboard) {
        lv_obj_send_event(state->textarea, LV_EVENT_FOCUSED, NULL);
    }
    app_request_fast_refresh();
}

ui_input_inline_t *ui_input_inline_create(lv_obj_t *textarea,
                                          lv_obj_t *keyboard_parent,
                                          size_t max_length,
                                          ui_input_submit_cb_t submit_cb,
                                          void *submit_user_data,
                                          ui_input_inline_layout_cb_t layout_cb,
                                          void *layout_user_data)
{
    ui_input_dialog_state_t *state;
    int landscape = ui_is_landscape();
    int screen_h = ui_screen_height();
    int keyboard_h = landscape ? 156 : 330;
    int candidate_h = landscape ? 42 : 52;
    int use_soft_keyboard;

    if(!textarea || !lv_obj_is_valid(textarea)) {
        return NULL;
    }
    if(keyboard_h > screen_h / 2) {
        keyboard_h = screen_h / 2;
    }

    state = calloc(1, sizeof(*state));
    if(!state) {
        return NULL;
    }
    state->inline_mode = 1;
    state->textarea = textarea;
    state->keyboard_parent = keyboard_parent ? keyboard_parent : lv_screen_active();
    state->submit_cb = submit_cb;
    state->user_data = submit_user_data;
    state->layout_cb = layout_cb;
    state->layout_user_data = layout_user_data;

    lv_textarea_set_one_line(state->textarea, true);
    lv_textarea_set_max_length(state->textarea,
                               max_length ? (uint32_t)max_length :
                               NET_PASS_MAX - 1U);
    lv_obj_add_event_cb(state->textarea, ui_input_event_cb, LV_EVENT_FOCUSED,
                        state);
    lv_obj_add_event_cb(state->textarea, ui_input_event_cb, LV_EVENT_CLICKED,
                        state);
    lv_obj_add_event_cb(state->textarea, ui_input_event_cb, LV_EVENT_READY,
                        state);
    lv_obj_add_event_cb(state->textarea, ui_input_event_cb, LV_EVENT_CANCEL,
                        state);

    use_soft_keyboard = ui_input_soft_keyboard_enabled() ||
                        !ui_extension_keyboard_active();
    if(use_soft_keyboard) {
        state->candidate_bar =
            ui_input_create_candidate_bar(state->keyboard_parent, candidate_h,
                                          landscape, state);
        lv_obj_align(state->candidate_bar, LV_ALIGN_BOTTOM_MID, 0, -keyboard_h);

        state->keyboard = lv_buttonmatrix_create(state->keyboard_parent);
        lv_obj_set_size(state->keyboard, ui_screen_width(), keyboard_h);
        lv_obj_align(state->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        ui_input_style_keyboard(state->keyboard, landscape);
        lv_obj_add_event_cb(state->keyboard, ui_input_keyboard_event_cb,
                            LV_EVENT_VALUE_CHANGED, state);
        ui_input_keyboard_set_mode(state, UI_INPUT_KBD_LOWER);
        lv_obj_add_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN);
        if(state->candidate_bar) {
            lv_obj_add_flag(state->candidate_bar, LV_OBJ_FLAG_HIDDEN);
        }
        ui_input_inline_apply_layout(state, 0);
    } else {
        state->candidate_bar =
            ui_input_create_candidate_bar(lv_layer_top(), candidate_h,
                                          landscape, state);
        if(state->candidate_bar) {
            lv_obj_set_pos(state->candidate_bar, 0, 0);
        }
        state->hardware_keyboard = 1;
        ui_input_hardware_sync_pinyin_mode(state);
    }

    ui_input_log("inline create soft=%d hw_active=%d",
                 use_soft_keyboard, ui_extension_keyboard_active());
    return state;
}

void ui_input_inline_destroy(ui_input_inline_t *state)
{
    if(!state) {
        return;
    }
    ui_input_inline_hide_state(state);
    if(state->textarea && lv_obj_is_valid(state->textarea)) {
        lv_obj_remove_event_cb_with_user_data(state->textarea,
                                              ui_input_event_cb, state);
    }
    if(state->keyboard && lv_obj_is_valid(state->keyboard)) {
        lv_obj_delete(state->keyboard);
    }
    if(state->candidate_bar && lv_obj_is_valid(state->candidate_bar)) {
        lv_obj_delete(state->candidate_bar);
    }
    if(active_inline == state) {
        active_inline = NULL;
    }
    free(state);
    app_request_fast_refresh();
}

void ui_input_inline_focus(ui_input_inline_t *state)
{
    ui_input_inline_show_state(state);
}

void ui_input_inline_submit(ui_input_inline_t *state)
{
    if(state) {
        ui_input_submit(state);
    }
}

void ui_input_inline_hide(ui_input_inline_t *state)
{
    ui_input_inline_hide_state(state);
}

int ui_input_inline_is_active(ui_input_inline_t *state)
{
    return state && active_inline == state;
}

void ui_input_hide_inline_active(void)
{
    if(active_inline) {
        ui_input_inline_hide_state(active_inline);
    }
}
