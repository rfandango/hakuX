/*
 * hakuX front end — the interface's languages (i18n.c, the strings in
 * i18n_strings.h). The system's language unless one is chosen.
 */
#ifndef NATIVE_APP_UI_I18N_H
#define NATIVE_APP_UI_I18N_H

enum UiLang { L_EN, L_ES, L_FR, L_DE, L_IT, L_PT, L_RU, L_PL, L_JA, L_ZH, L_KO, L_COUNT };

#define X(id, ...) id,
enum UiStr {
#include "native/app/ui/i18n_strings.h"
    S_COUNT
};
#undef X

/* "auto" (or NULL): the system's preferred language; else a code ("es"). */
void i18n_set(const char *code);
enum UiLang i18n_lang(void);
const char *i18n_code(enum UiLang l);
const char *i18n_name(enum UiLang l);   /* in its own language */
const char *T(enum UiStr s);
/* The console's language (XC_LANGUAGE 1-9) closest to the interface's. */
int i18n_console_lang(void);

#endif
