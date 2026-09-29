#include "page_tools.h"

#include <minIni.h>
#include <stdio.h>

#include "../conf/ui.h"

#include "core/osd.h"
#include "core/settings.h"
#include "lang/language.h"
#include "ui/ui_porting.h"
#include "ui/ui_style.h"
#include "ui/ui_theme.h"

// Tools: a grab bag of one-off utilities that don't warrant their own
// sidebar entry each -- Focus Chart and Frequency Chart just show a
// fullscreen reference image, Theme cycles the UI colour theme.
#define ROW_FOCUS_CHART 0
#define ROW_FREQ_CHART  1
#ifndef HDZBOXPRO
// Box Pro never offered the Theme page either (ui_main_menu.c used to gate
// it out with #if !defined(HDZBOXPRO)); keep that behaviour here.
#define ROW_THEME    2
#define ROW_SWATCHES 3
#define TOOLS_ROW_COUNT 4
#else
// submenu_click() treats the last selectable row as an implicit "Back", so
// a real, clickable row can never be last -- pad with a non-selectable row.
#define ROW_PAD         2
#define TOOLS_ROW_COUNT 3
#endif

#define SWATCH_COUNT 4

static lv_coord_t col_dsc[] = {160, 160, 160, 160, 160, 160, LV_GRID_TEMPLATE_LAST};
static lv_coord_t row_dsc[] = {60, 60, 60, 60, 60, 60, 60, LV_GRID_TEMPLATE_LAST};

static lv_obj_t *chart_img;
static bool chart_open;

#ifndef HDZBOXPRO
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
#endif

static void show_chart(bool is_focus_chart) {
    char filename[128];
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    osd_resource_path(filename, "%s", OSD_RESOURCE_1080, is_focus_chart ? FOCUS_CHART_IMG : FREQ_CHART_IMG);
#elif defined(HDZBOXPRO)
    osd_resource_path(filename, "%s", OSD_RESOURCE_720, is_focus_chart ? FOCUS_CHART_IMG : FREQ_CHART_IMG);
#endif
    lv_img_set_src(chart_img, filename);

    lv_obj_move_foreground(chart_img);
    lv_obj_clear_flag(chart_img, LV_OBJ_FLAG_HIDDEN);
    lvgl_screen_orbit(false);
    chart_open = true;
}

static void hide_chart(void) {
    if (!chart_open)
        return;
    lv_obj_add_flag(chart_img, LV_OBJ_FLAG_HIDDEN);
    lvgl_screen_orbit(g_setting.osd.orbit > 0);
    chart_open = false;
}

static lv_obj_t *page_tools_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[128];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("Tools"));
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

    snprintf(buf, sizeof(buf), "%s", _lang("Focus Chart"));
    create_label_item(cont, buf, 1, ROW_FOCUS_CHART, 5);

    snprintf(buf, sizeof(buf), "%s", _lang("Frequency Chart"));
    create_label_item(cont, buf, 1, ROW_FREQ_CHART, 5);

    lv_obj_t *note = lv_label_create(cont);
    int note_row = TOOLS_ROW_COUNT;
    // Wider than buf: the combined note sentences run past 128 bytes and
    // snprintf into the shared buf silently truncated mid-sentence.
    char note_buf[256];

#ifndef HDZBOXPRO
    // Row 3 (colour swatches) is decorative, not a real entry.
    lv_obj_clear_flag(pp_tools.p_arr.panel[ROW_SWATCHES], FLAG_SELECTABLE);

    create_label_item(cont, _lang("Theme"), 1, ROW_THEME, 1);
    label_name = create_label_item(cont, "", 2, ROW_THEME, 3);

    for (int i = 0; i < SWATCH_COUNT; i++) {
        swatch[i] = lv_obj_create(cont);
        lv_obj_clear_flag(swatch[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(swatch[i], 100, 36);
        lv_obj_set_style_radius(swatch[i], 0, 0);
        lv_obj_set_style_border_width(swatch[i], 1, 0);
        lv_obj_set_style_border_color(swatch[i], lv_color_hex(0x808080), 0);
        lv_obj_set_grid_cell(swatch[i], LV_GRID_ALIGN_START, 2 + i, 1,
                             LV_GRID_ALIGN_CENTER, ROW_SWATCHES, 1);
    }

    snprintf(note_buf, sizeof(note_buf), "%s\n%s\n%s",
             _lang("Click Focus Chart or Frequency Chart to display it fullscreen, click again to dismiss."),
             _lang("Click Theme to switch theme."),
             _lang("Restart the goggles to apply the new theme."));

    preview_idx = g_setting.ui_theme;
    show_preview(preview_idx);
#else
    lv_obj_clear_flag(pp_tools.p_arr.panel[ROW_PAD], FLAG_SELECTABLE);

    snprintf(note_buf, sizeof(note_buf), "%s",
             _lang("Click Focus Chart or Frequency Chart to display it fullscreen, click again to dismiss."));
#endif

    lv_label_set_text(note, note_buf);
    lv_obj_set_style_text_font(note, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(note, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(note, LV_GRID_ALIGN_START, 1, 5, LV_GRID_ALIGN_START, note_row, 2);

    chart_img = lv_img_create(lv_scr_act());
    lv_obj_add_flag(chart_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chart_img, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(chart_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(chart_img, 0, 0);
    lv_obj_set_size(chart_img, DRAW_HOR_RES_FHD, DRAW_VER_RES_FHD);
    chart_open = false;

    return page;
}

static void page_tools_exit(void) {
    hide_chart();
}

static void page_tools_on_click(uint8_t key, int sel) {
    if (chart_open) {
        // Any click while a chart overlay is up just dismisses it back to
        // the row list, rather than leaving the whole Tools page.
        hide_chart();
        return;
    }

    switch (sel) {
    case ROW_FOCUS_CHART:
        show_chart(true);
        break;
    case ROW_FREQ_CHART:
        show_chart(false);
        break;
#ifndef HDZBOXPRO
    case ROW_THEME:
        preview_idx = (preview_idx + 1) % ui_theme_count();
        g_setting.ui_theme = preview_idx;
        ini_putl("ui", "theme", g_setting.ui_theme, SETTING_INI);
        show_preview(preview_idx);
        break;
#endif
    default:
        break;
    }
}

page_pack_t pp_tools = {
    .p_arr = {
        .cur = 0,
        .max = TOOLS_ROW_COUNT,
    },
    .name = "Tools",
    .create = page_tools_create,
    .enter = NULL,
    .exit = page_tools_exit,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = NULL,
    .on_click = page_tools_on_click,
    .on_right_button = NULL,
};
