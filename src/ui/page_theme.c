#include "page_theme.h"

#include <minIni.h>
#include <stdio.h>

#include "../conf/ui.h"

#include "core/settings.h"
#include "lang/language.h"
#include "ui/ui_style.h"
#include "ui/ui_theme.h"

#define SWATCH_COUNT 4

static lv_coord_t col_dsc[] = {160, 160, 160, 160, 160, 160, LV_GRID_TEMPLATE_LAST};
static lv_coord_t row_dsc[] = {60, 60, 60, 60, 60, 60, 60, LV_GRID_TEMPLATE_LAST};

static lv_obj_t *label_name;
static lv_obj_t *swatch[SWATCH_COUNT];
static int preview_idx;

static void show_preview(int idx) {
    const ui_theme_t *t = ui_theme_get(idx);
    char buf[64];

    snprintf(buf, sizeof(buf), "< %s >", t->name);
    lv_label_set_text(label_name, buf);

    // Swatches: text, accent, focus and progress colours of the picked theme.
    const uint32_t colors[SWATCH_COUNT] = {t->text, t->accent, t->focus, t->bar_fg};
    for (int i = 0; i < SWATCH_COUNT; i++)
        lv_obj_set_style_bg_color(swatch[i], lv_color_hex(colors[i]), 0);
}

static lv_obj_t *page_theme_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[128];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("Theme"));
    create_text(NULL, section, false, buf, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *cont = lv_obj_create(section);
    lv_obj_set_size(cont, UI_PAGE_VIEW_SIZE);
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_GRID);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);

    lv_obj_set_style_grid_column_dsc_array(cont, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(cont, row_dsc, 0);

    create_select_item(arr, cont);
    // Row 0 picks the theme, row 1 shows its colours (not selectable), row 2 is Back.
    lv_obj_clear_flag(pp_theme.p_arr.panel[1], FLAG_SELECTABLE);

    create_label_item(cont, _lang("Theme"), 1, 0, 1);
    label_name = create_label_item(cont, "", 2, 0, 3);

    for (int i = 0; i < SWATCH_COUNT; i++) {
        swatch[i] = lv_obj_create(cont);
        lv_obj_clear_flag(swatch[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(swatch[i], 100, 36);
        lv_obj_set_style_radius(swatch[i], 0, 0);
        lv_obj_set_style_border_width(swatch[i], 1, 0);
        lv_obj_set_style_border_color(swatch[i], lv_color_hex(0x808080), 0);
        lv_obj_set_grid_cell(swatch[i], LV_GRID_ALIGN_START, 2 + i, 1,
                             LV_GRID_ALIGN_CENTER, 1, 1);
    }

    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, 2, 1);

    lv_obj_t *note = lv_label_create(cont);
    snprintf(buf, sizeof(buf), "%s\n%s",
             _lang("Click to switch theme."),
             _lang("Restart the goggles to apply the new theme."));
    lv_label_set_text(note, buf);
    lv_obj_set_style_text_font(note, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(note, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(note, LV_GRID_ALIGN_START, 1, 5, LV_GRID_ALIGN_START, 3, 2);

    preview_idx = g_setting.ui_theme;
    show_preview(preview_idx);
    return page;
}

static void page_theme_on_click(uint8_t key, int sel) {
    if (sel != 0)
        return;

    preview_idx = (preview_idx + 1) % ui_theme_count();
    g_setting.ui_theme = preview_idx;
    ini_putl("ui", "theme", g_setting.ui_theme, SETTING_INI);
    show_preview(preview_idx);
}

page_pack_t pp_theme = {
    .p_arr = {
        .cur = 0,
        .max = 3,
    },
    .name = "Theme",
    .create = page_theme_create,
    .enter = NULL,
    .exit = NULL,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = NULL,
    .on_click = page_theme_on_click,
    .on_right_button = NULL,
};
