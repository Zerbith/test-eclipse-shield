/*
 * Android replacement for font.c. Fontconfig is not available on Android, so the UI font is
 * loaded straight from the system font directory (present on every Android TV / NVIDIA Shield).
 *
 * STATUS: first draft, not compiled on a device yet.
 */
#include "font.h"
#include "ui/config.h"
#include "res.h"

#include <stdlib.h>
#include <unistd.h>

static const char *const MAIN_FONT_CANDIDATES[] = {
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/NotoSans-Regular.ttf",
        "/system/fonts/DroidSans.ttf",
        NULL,
};

static const char *const FALLBACK_FONT_CANDIDATES[] = {
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/NotoSerifCJK-Regular.ttc",
        "/system/fonts/DroidSansFallback.ttf",
        NULL,
};

static const char *first_existing(const char *const *candidates) {
    for (int i = 0; candidates[i] != NULL; i++) {
        if (access(candidates[i], R_OK) == 0) {
            return candidates[i];
        }
    }
    return NULL;
}

static void fontset_destroy_set(app_fontset_t *set) {
    if (set->large != NULL) {
        lv_ft_font_destroy(set->large);
        set->large = NULL;
    }
    if (set->normal != NULL) {
        lv_ft_font_destroy(set->normal);
        set->normal = NULL;
    }
    if (set->small != NULL) {
        lv_ft_font_destroy(set->small);
        set->small = NULL;
    }
}

static lv_font_t *load_one(const char *name, const void *mem, size_t mem_size, int size) {
    lv_ft_info_t info = {.name = name, .mem = mem, .mem_size = mem_size,
            .style = FT_FONT_STYLE_NORMAL, .weight = size};
    return lv_ft_font_init(&info) ? info.font : NULL;
}

static bool fontset_load(app_fontset_t *set, const char *name, const void *mem, size_t mem_size) {
    set->normal = load_one(name, mem, mem_size, set->normal_size);
    set->large = load_one(name, mem, mem_size, set->large_size);
    set->small = load_one(name, mem, mem_size, set->small_size);
    if (set->small != NULL && set->normal != NULL && set->large != NULL) {
        return true;
    }
    fontset_destroy_set(set);
    return false;
}

int app_font_init(app_fonts_t *fonts, int dpi) {
    app_fontset_t fontset = {
            .small_size = _LV_DPX_CALC(dpi, 14),
            .normal_size = _LV_DPX_CALC(dpi, 16),
            .large_size = _LV_DPX_CALC(dpi, 19)
    };
    app_fontset_t iconfonts = fontset;
    if (!fontset_load(&iconfonts, "MaterialIcons", res_mat_iconfont_data, res_mat_iconfont_size)) {
        return -1;
    }
    const char *main_path = first_existing(MAIN_FONT_CANDIDATES);
    if (main_path == NULL || !fontset_load(&fontset, main_path, NULL, 0)) {
        fontset_destroy_set(&iconfonts);
        return -1;
    }
    const char *fallback_path = first_existing(FALLBACK_FONT_CANDIDATES);
    if (fallback_path != NULL) {
        app_fontset_t *fallback = calloc(1, sizeof(app_fontset_t));
        if (fallback != NULL) {
            fallback->small_size = fontset.small_size;
            fallback->normal_size = fontset.normal_size;
            fallback->large_size = fontset.large_size;
            if (fontset_load(fallback, fallback_path, NULL, 0)) {
                fontset.fallback = fallback;
                fontset.normal->fallback = fallback->normal;
                fontset.large->fallback = fallback->large;
                fontset.small->fallback = fallback->small;
            } else {
                free(fallback);
            }
        }
    }
    fonts->fonts = fontset;
    fonts->icons = iconfonts;
    return 0;
}

void app_font_deinit(app_fonts_t *fonts) {
    fontset_destroy_set(&fonts->fonts);
    fontset_destroy_set(&fonts->icons);
    if (fonts->fonts.fallback != NULL) {
        fontset_destroy_set(fonts->fonts.fallback);
        free(fonts->fonts.fallback);
        fonts->fonts.fallback = NULL;
    }
}
