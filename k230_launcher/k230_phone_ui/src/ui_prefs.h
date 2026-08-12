#ifndef K230_PHONE_UI_PREFS_H
#define K230_PHONE_UI_PREFS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_PREFS_DIR "/root/.config/k230_phone_ui"
#define UI_PREFS_FILE UI_PREFS_DIR "/settings.conf"

int ui_prefs_get(const char *key, char *value, size_t value_len,
                 const char *fallback);
int ui_prefs_set(const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif
