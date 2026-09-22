#include "page_autoscan.h"

#include <minIni.h>

#include "../conf/ui.h"

#include "core/settings.h"
#include "lang/language.h"
#include "ui/ui_style.h"

static lv_coord_t col_dsc[] = {UI_AUTOSCAN_COLS};
static lv_coord_t row_dsc[] = {UI_AUTOSCAN_ROWS};

// Row 0: how the goggles come up -- run a scan, boot straight to a channel,
// or open the menu (loading that same channel behind it either way).
static btn_group_t btn_group_startup;
// Row 1: which channel that is -- whatever was last active, or a specific
// source picked below.
static btn_group_t btn_group_then;
// Row 2 (2 grid rows): the specific source, only meaningful/selectable when
// "Then" is set to Source.
static btn_group_t btn_group_source;

enum { ROW_STARTUP = 0, ROW_THEN, ROW_SOURCE, ROW_SOURCE_CONT, ROW_BACK };

// Reflect "Then" onto the Source row's selectability and pill dimming --
// called at page creation and every time "Then" is toggled.
static void update_source_row_enabled(void) {
    bool enabled = (g_setting.autoscan.source != SETTING_AUTOSCAN_SOURCE_LAST);
    if (enabled) {
        lv_obj_add_flag(pp_autoscan.p_arr.panel[ROW_SOURCE], FLAG_SELECTABLE);
    } else {
        lv_obj_clear_flag(pp_autoscan.p_arr.panel[ROW_SOURCE], FLAG_SELECTABLE);
    }
    btn_group_enable(&btn_group_source, enabled);
}

static lv_obj_t *page_autoscan_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[128];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("Startup"));
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
    lv_obj_set_grid_cell(pp_autoscan.p_arr.panel[ROW_SOURCE], LV_GRID_ALIGN_STRETCH, 0, 6,
                         LV_GRID_ALIGN_STRETCH, ROW_SOURCE, 2);
    lv_obj_clear_flag(pp_autoscan.p_arr.panel[ROW_SOURCE_CONT], FLAG_SELECTABLE);
    pp_autoscan.p_arr.no_card |= 1u << ROW_SOURCE_CONT; // second row of the merged Source picker

    create_btn_group_item(&btn_group_startup, cont, 3, _lang("Startup"), _lang("Scan"), _lang("Boot"), _lang("Menu"), "", ROW_STARTUP);
    create_btn_group_item(&btn_group_then, cont, 2, _lang("Then"), _lang("Last"), _lang("Source"), "", "", ROW_THEN);
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    // Matches setting_autoscan_source_t order (minus Last, handled by "Then"
    // above); "Auto" (= Auto Detect) is BoxPro/G2 only (built-in analog).
    create_btn_group_item2(&btn_group_source, cont, 5, _lang("Source"), _lang("HDZero"), _lang("Analog"), _lang("AV In"), _lang("HDMI In"), _lang("Auto"), "", ROW_SOURCE);
#else
    create_btn_group_item2(&btn_group_source, cont, 4, _lang("Source"), _lang("HDZero"), _lang("Analog"), _lang("AV In"), _lang("HDMI In"), "", "", ROW_SOURCE);
#endif
    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 1);

    lv_obj_t *label2 = lv_label_create(cont);
    char note[320];
    snprintf(note, sizeof(note), "%s\n%s\n%s\n%s",
             _lang("*Scan runs a search at boot; Boot loads the picked channel directly"),
             _lang("*Menu does the same as Boot, then opens this menu on top of it"),
             _lang("*if 'Then' is Last, the goggles use whichever channel was last active"),
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
             _lang("*Scan has no effect on AV In or HDMI In; they always load directly"));
#else
             _lang("*Scan has no effect on Analog, AV In or HDMI In; they always load directly"));
#endif
    lv_label_set_text(label2, note);
    lv_obj_set_style_text_font(label2, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(label2, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label2, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(label2, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(label2, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(label2, LV_GRID_ALIGN_START, 1, 4,
                         LV_GRID_ALIGN_START, ROW_BACK + 1, 3);

    int startup_sel;
    if (g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_OFF) {
        startup_sel = 2; // Menu
    } else if (g_setting.autoscan.status == SETTING_AUTOSCAN_STATUS_ON &&
               !g_setting.autoscan.load_from_boot) {
        startup_sel = 0; // Scan
    } else {
        // ON + load_from_boot, or a pre-existing "Last" status from an older
        // firmware -- main.c treats both the same way, and this page always
        // writes ON + load_from_boot=true for "Boot" going forward.
        startup_sel = 1; // Boot
    }
    btn_group_set_sel(&btn_group_startup, startup_sel);

    bool then_source = (g_setting.autoscan.source != SETTING_AUTOSCAN_SOURCE_LAST);
    btn_group_set_sel(&btn_group_then, then_source ? 1 : 0);
    btn_group_set_sel(&btn_group_source, then_source ? g_setting.autoscan.source - 1 : 0);
    update_source_row_enabled();
    return page;
}

static void page_autoscan_on_click(uint8_t key, int sel) {
    if (sel == ROW_STARTUP) {
        btn_group_toggle_sel(&btn_group_startup);
        switch (btn_group_get_sel(&btn_group_startup)) {
        case 0: // Scan
            g_setting.autoscan.status = SETTING_AUTOSCAN_STATUS_ON;
            g_setting.autoscan.load_from_boot = false;
            break;
        case 1: // Boot
            g_setting.autoscan.status = SETTING_AUTOSCAN_STATUS_ON;
            g_setting.autoscan.load_from_boot = true;
            break;
        default: // Menu
            g_setting.autoscan.status = SETTING_AUTOSCAN_STATUS_OFF;
            g_setting.autoscan.load_from_boot = false;
            break;
        }
        ini_putl("autoscan", "status", g_setting.autoscan.status, SETTING_INI);
        settings_put_bool("autoscan", "load_from_boot", g_setting.autoscan.load_from_boot);
    } else if (sel == ROW_THEN) {
        btn_group_toggle_sel(&btn_group_then);
        if (btn_group_get_sel(&btn_group_then) == 0) {
            g_setting.autoscan.source = SETTING_AUTOSCAN_SOURCE_LAST;
        } else if (g_setting.autoscan.source == SETTING_AUTOSCAN_SOURCE_LAST) {
            // Switching to Source with nothing picked yet: default to HDZero.
            g_setting.autoscan.source = SETTING_AUTOSCAN_SOURCE_HDZERO;
            btn_group_set_sel(&btn_group_source, 0);
        }
        ini_putl("autoscan", "source", g_setting.autoscan.source, SETTING_INI);
        update_source_row_enabled();
    } else if (sel == ROW_SOURCE) {
        btn_group_toggle_sel(&btn_group_source);
        g_setting.autoscan.source = btn_group_get_sel(&btn_group_source) + 1;
        ini_putl("autoscan", "source", g_setting.autoscan.source, SETTING_INI);
    }
}

page_pack_t pp_autoscan = {
    .p_arr = {
        .cur = 0,
        .max = 5,
    },
    .name = "Startup",
    .create = page_autoscan_create,
    .enter = NULL,
    .exit = NULL,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = NULL,
    .on_click = page_autoscan_on_click,
    .on_right_button = NULL,
};
