/*
 * hakuX front end — the interface's languages (i18n.h).
 */
#include "native/app/ui/i18n.h"

#include <SDL3/SDL.h>
#include <string.h>

static const char *const k_str[S_COUNT][L_COUNT] = {
#define X(id, ...) {__VA_ARGS__},
#include "native/app/ui/i18n_strings.h"
#undef X
};

static const char *const k_code[L_COUNT] = {"en", "es", "fr", "de", "it", "pt", "ru", "pl", "ja", "zh", "ko"};
static const char *const k_name[L_COUNT] = {"English", "Español", "Français", "Deutsch", "Italiano", "Português",
                                            "Русский", "Polski", "日本語", "简体中文", "한국어"};

static enum UiLang g_lang = L_EN;

static int from_code(const char *code)
{
    for (int i = 0; i < L_COUNT; i++) {
        if (code && !SDL_strncasecmp(code, k_code[i], 2)) {
            return i;
        }
    }
    return -1;
}

void i18n_set(const char *code)
{
    int l = code && strcmp(code, "auto") ? from_code(code) : -1;
    if (l < 0) {
        int n = 0;
        SDL_Locale **loc = SDL_GetPreferredLocales(&n);
        for (int i = 0; loc && i < n && l < 0; i++) {
            l = from_code(loc[i]->language);
        }
        SDL_free(loc);
    }
    g_lang = l < 0 ? L_EN : (enum UiLang)l;
}

enum UiLang i18n_lang(void)
{
    return g_lang;
}

const char *i18n_code(enum UiLang l)
{
    return k_code[l];
}

const char *i18n_name(enum UiLang l)
{
    return k_name[l];
}

const char *T(enum UiStr s)
{
    const char *t = k_str[s][g_lang];
    return t && *t ? t : k_str[s][L_EN];
}

int i18n_console_lang(void)
{
    /* XC_LANGUAGE: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish,
     * 6 Italian, 7 Korean, 8 Chinese, 9 Portuguese */
    static const int k_map[L_COUNT] = {1, 5, 4, 3, 6, 9, 1, 1, 2, 8, 7};
    return k_map[g_lang];
}
