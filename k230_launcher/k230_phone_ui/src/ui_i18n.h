#ifndef K230_PHONE_UI_I18N_H
#define K230_PHONE_UI_I18N_H

#ifdef __cplusplus
extern "C" {
#endif

#define UI_LANG_EN "en"
#define UI_LANG_ZH "zh"
#define UI_LANG_JA "ja"
#define UI_LANGUAGE_KEY "ui.language"

void ui_i18n_init(void);
const char *ui_i18n_language(void);
int ui_i18n_is_chinese(void);
int ui_i18n_is_japanese(void);
int ui_i18n_set_language(const char *language);
const char *ui_tr(const char *text);

#ifdef __cplusplus
}
#endif

#endif
