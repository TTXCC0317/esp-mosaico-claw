/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mosaic_i18n.h"

#include <string.h>

static mosaic_i18n_lang_t s_language = MOSAIC_I18N_LANG_EN;

mosaic_i18n_lang_t mosaic_i18n_parse(const char *tag)
{
    if (tag == NULL) {
        return MOSAIC_I18N_LANG_EN;
    }
    if (strcmp(tag, "zh") == 0 || strcmp(tag, "zh-CN") == 0 ||
            strcmp(tag, "cn") == 0 || strcmp(tag, "Chinese") == 0) {
        return MOSAIC_I18N_LANG_ZH;
    }
    return MOSAIC_I18N_LANG_EN;
}

const char *mosaic_i18n_tag(mosaic_i18n_lang_t lang)
{
    return lang == MOSAIC_I18N_LANG_ZH ? "zh" : "en";
}

void mosaic_i18n_set_language(mosaic_i18n_lang_t lang)
{
    s_language = lang;
}

mosaic_i18n_lang_t mosaic_i18n_language(void)
{
    return s_language;
}

bool mosaic_i18n_is_chinese(void)
{
    return s_language == MOSAIC_I18N_LANG_ZH;
}

const char *mosaic_i18n_tr(const char *en, const char *zh)
{
    if (s_language == MOSAIC_I18N_LANG_ZH && zh != NULL) {
        return zh;
    }
    return en != NULL ? en : "";
}

void mosaic_i18n_apply(esp_gsp_handle_t ui,
                       const mosaic_i18n_entry_t *entries, size_t count)
{
    if (ui == NULL || entries == NULL || count == 0 ||
            s_language != MOSAIC_I18N_LANG_ZH) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].zh == NULL) {
            continue;
        }
        (void)esp_gsp_set_text(ui, entries[i].bind, entries[i].zh);
    }
}