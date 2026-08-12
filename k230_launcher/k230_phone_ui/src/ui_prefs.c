#include "ui_prefs.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UI_PREFS_MAX_ITEMS 64
#define UI_PREFS_KEY_MAX 64
#define UI_PREFS_VALUE_MAX 160

typedef struct {
    char key[UI_PREFS_KEY_MAX];
    char value[UI_PREFS_VALUE_MAX];
} ui_pref_item_t;

static void prefs_trim(char *text)
{
    size_t len;
    char *start;

    if(!text) {
        return;
    }

    start = text;
    while(*start && isspace((unsigned char)*start)) {
        start++;
    }
    if(start != text) {
        memmove(text, start, strlen(start) + 1U);
    }

    len = strlen(text);
    while(len > 0 && isspace((unsigned char)text[len - 1U])) {
        text[--len] = '\0';
    }
}

static int prefs_key_valid(const char *key)
{
    size_t i;

    if(!key || !key[0] || strlen(key) >= UI_PREFS_KEY_MAX) {
        return 0;
    }

    for(i = 0; key[i]; i++) {
        unsigned char ch = (unsigned char)key[i];

        if(isspace(ch) || ch == '=' || ch == '#') {
            return 0;
        }
    }

    return 1;
}

static void prefs_copy(char *dst, size_t dst_len, const char *src)
{
    if(!dst || dst_len == 0) {
        return;
    }

    snprintf(dst, dst_len, "%s", src ? src : "");
}

static int prefs_ensure_dir(void)
{
    if(mkdir("/root/.config", 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    if(mkdir(UI_PREFS_DIR, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int prefs_load(ui_pref_item_t *items, size_t max_items)
{
    FILE *fp;
    char line[256];
    int count = 0;

    if(!items || max_items == 0) {
        return 0;
    }

    fp = fopen(UI_PREFS_FILE, "r");
    if(!fp) {
        return 0;
    }

    while(fgets(line, sizeof(line), fp) && count < (int)max_items) {
        char *sep;

        line[strcspn(line, "\r\n")] = '\0';
        prefs_trim(line);
        if(!line[0] || line[0] == '#') {
            continue;
        }

        sep = strchr(line, '=');
        if(!sep) {
            continue;
        }

        *sep++ = '\0';
        prefs_trim(line);
        prefs_trim(sep);
        if(!prefs_key_valid(line)) {
            continue;
        }

        prefs_copy(items[count].key, sizeof(items[count].key), line);
        prefs_copy(items[count].value, sizeof(items[count].value), sep);
        count++;
    }

    fclose(fp);
    return count;
}

static int prefs_save(const ui_pref_item_t *items, int count)
{
    char tmp_path[sizeof(UI_PREFS_FILE) + 8];
    FILE *fp;

    if(prefs_ensure_dir() != 0) {
        return -1;
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", UI_PREFS_FILE);
    fp = fopen(tmp_path, "w");
    if(!fp) {
        return -1;
    }

    fprintf(fp, "# k230_phone_ui persistent settings\n");
    for(int i = 0; i < count; i++) {
        fprintf(fp, "%s=%s\n", items[i].key, items[i].value);
    }

    if(fclose(fp) != 0) {
        unlink(tmp_path);
        return -1;
    }

    if(rename(tmp_path, UI_PREFS_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

int ui_prefs_get(const char *key, char *value, size_t value_len,
                 const char *fallback)
{
    ui_pref_item_t items[UI_PREFS_MAX_ITEMS];
    int count;

    if(!prefs_key_valid(key) || !value || value_len == 0) {
        return -1;
    }

    prefs_copy(value, value_len, fallback);
    count = prefs_load(items, UI_PREFS_MAX_ITEMS);
    for(int i = 0; i < count; i++) {
        if(strcmp(items[i].key, key) == 0) {
            prefs_copy(value, value_len, items[i].value);
            return 0;
        }
    }

    return -1;
}

int ui_prefs_set(const char *key, const char *value)
{
    ui_pref_item_t items[UI_PREFS_MAX_ITEMS];
    int count;

    if(!prefs_key_valid(key) || !value || strlen(value) >= UI_PREFS_VALUE_MAX) {
        return -1;
    }

    count = prefs_load(items, UI_PREFS_MAX_ITEMS);
    for(int i = 0; i < count; i++) {
        if(strcmp(items[i].key, key) == 0) {
            prefs_copy(items[i].value, sizeof(items[i].value), value);
            return prefs_save(items, count);
        }
    }

    if(count >= UI_PREFS_MAX_ITEMS) {
        return -1;
    }

    prefs_copy(items[count].key, sizeof(items[count].key), key);
    prefs_copy(items[count].value, sizeof(items[count].value), value);
    count++;
    return prefs_save(items, count);
}
