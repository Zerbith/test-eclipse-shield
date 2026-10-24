#include "radial_menu.h"

#include <stdatomic.h>

#include "lvgl.h"
#include "util/i18n.h"

static atomic_bool g_open;
static atomic_int g_pad = -1;
static atomic_int g_selection = RADIAL_NONE;

static lv_obj_t *g_root;
static lv_obj_t *g_items[4];
static lv_obj_t *g_mouse_label;
static bool g_mouse_on;

bool radial_menu_is_open(void) { return atomic_load(&g_open); }
int radial_menu_owner_pad(void) { return atomic_load(&g_pad); }

void radial_menu_set_open(bool open, int pad) {
    atomic_store(&g_pad, open ? pad : -1);
    if (open) {
        atomic_store(&g_selection, RADIAL_NONE);
    }   /* closing KEEPS the selection: the UI thread reads it to know what was chosen */
    atomic_store(&g_open, open);
}

void radial_menu_set_selection(int sector) { atomic_store(&g_selection, sector); }
int radial_menu_selection(void) { return atomic_load(&g_selection); }

static lv_obj_t *make_item(lv_obj_t *parent, const char *text, lv_coord_t dx, lv_coord_t dy,
                           lv_obj_t **label_out) {
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_set_size(item, LV_DPX(230), LV_DPX(64));
    lv_obj_align(item, LV_ALIGN_CENTER, dx, dy);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(item, LV_DPX(12), 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_t *label = lv_label_create(item);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (label_out != NULL) {
        *label_out = label;
    }
    return item;
}

void radial_menu_show(bool mouse_on) {
    radial_menu_hide();
    g_mouse_on = mouse_on;
    g_root = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(g_root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(g_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_50, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_radius(g_root, 0, 0);

    const lv_coord_t h = LV_DPX(125), w = LV_DPX(250);
    g_items[RADIAL_MOUSE] = make_item(g_root, mouse_on ? locstr("Mouse: turn off") : locstr("Mouse: turn on"),
                                      0, -h, &g_mouse_label);
    g_items[RADIAL_KEYBOARD] = make_item(g_root, locstr("Keyboard"), w, 0, NULL);
    g_items[RADIAL_END_STREAM] = make_item(g_root, locstr("End stream"), 0, h, NULL);
    g_items[RADIAL_CANCEL] = make_item(g_root, locstr("Cancel"), -w, 0, NULL);

    lv_obj_t *hint = lv_label_create(g_root);
    lv_label_set_text(hint, locstr("Left stick: choose   Release Start: confirm"));
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
    radial_menu_refresh();
}

void radial_menu_refresh(void) {
    if (g_root == NULL) {
        return;
    }
    const int selected = radial_menu_selection();
    for (int i = 0; i < 4; i++) {
        if (g_items[i] == NULL) {
            continue;
        }
        lv_obj_set_style_bg_color(g_items[i],
                                  i == selected ? lv_palette_main(LV_PALETTE_BLUE) : lv_palette_darken(LV_PALETTE_GREY, 3), 0);
        lv_obj_set_style_bg_opa(g_items[i], LV_OPA_COVER, 0);
    }
}

void radial_menu_hide(void) {
    if (g_root != NULL) {
        lv_obj_del(g_root);
        g_root = NULL;
        for (int i = 0; i < 4; i++) {
            g_items[i] = NULL;
        }
        g_mouse_label = NULL;
    }
}
