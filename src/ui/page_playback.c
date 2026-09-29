#include "page_playback.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include <log/log.h>

#include "../conf/ui.h"

#include "common.hh"
#include "core/app_state.h"
#include "core/osd.h"
#include "driver/beep.h"
#include "lang/language.h"
#include "record/record_definitions.h"
#include "ui/page_common.h"
#include "ui/ui_player.h"
#include "ui/ui_style.h"
#include "util/filesystem.h"
#include "util/math.h"
#include "util/sdcard.h"
#include "util/system.h"
#if defined(EMULATOR_BUILD)
// Emulator: point the DVR folder at a local mock directory so you can drop a copy
// of your goggle's DCIM/100HDZRO/ folder (videos + .jpg thumbnails + .star.txt)
// anywhere and test playback on the desktop. Set HDZ_MEDIA_DIR (MUST end with
// '/'); defaults to ./mock-sd/ relative to the working directory.
static inline const char *emu_media_dir(void) {
    const char *d = getenv("HDZ_MEDIA_DIR");
    return (d && *d) ? d : "mock-sd/";
}
#define MEDIA_FILES_DIR emu_media_dir()
#else
#define MEDIA_FILES_DIR REC_diskPATH REC_packPATH // "/mnt/extsd/movies" --> "/mnt/extsd" "/movies/"
#endif
bool status_displayed = false;
lv_obj_t *status;
LV_IMG_DECLARE(img_star);
LV_IMG_DECLARE(img_arrow1);

static lv_coord_t col_dsc[] = {UI_PLAYBACK_COLS};
static lv_coord_t row_dsc[] = {UI_PLAYBACK_ROWS};

static media_db_t media_db;
static pb_ui_item_t pb_ui[ITEMS_LAYOUT_CNT];

static pb_day_entry_t day_list[MAX_DAY_ENTRIES];
static int day_count;
static lv_obj_t *day_labels[UI_PLAYBACK_DAYS_VISIBLE];
static lv_obj_t *day_cursor;
static void build_day_list(void);
static void update_day_list_ui(void);
static void update_day_cursor(void);

// Whether the currently-shown status box (page_playback_open_status_box) can
// be dismissed by RIGHT_KEY_CLICK/RIGHT_KEY_PRESS -- file-scope (not just
// pb_key()-local) so the background MP4-conversion thread can drive the same
// box for its progress/result message.
static bool status_deleting = false;
// Whether DIAL_KEY_CLICK on the current box should delete the selected clip
// (the original delete-confirmation box) rather than just dismiss an
// informational one (e.g. a conversion result).
static bool status_is_delete_confirm = false;

// Long-press action menu (RIGHT_KEY_PRESS on a clip): Favorite / Convert to
// MP4 (only for .ts clips) / Remove / Info.
typedef enum {
    PB_ACTION_FAVORITE,
    PB_ACTION_CONVERT,
    PB_ACTION_REMOVE,
    PB_ACTION_INFO,
} pb_action_t;
#define PB_ACTION_MAX_ROWS 4
static lv_obj_t *action_menu_bg;
static lv_obj_t *action_menu_row[PB_ACTION_MAX_ROWS];
static pb_action_t action_menu_action[PB_ACTION_MAX_ROWS];
static int action_menu_row_count;
static int action_menu_sel;
static bool action_menu_open;

// .ts -> .mp4 conversion (remux, no re-encode) via the standalone ts2mp4
// helper binary -- ffmpeg isn't linked into this process, only into the
// record/ts2mp4 executables (see CMakeLists.txt), so the actual work happens
// out of process.
static bool convert_running;
static char convert_ts_path[512];
static char convert_mp4_path[512];
static lv_obj_t *convert_progress_bar;

/**
 * Displays the status message box.
 */
static void page_playback_open_status_box(const char *title, const char *text) {
    status_displayed = true;
    lv_label_set_text(lv_msgbox_get_title(status), title);
    lv_label_set_text(lv_msgbox_get_text(status), text);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_HIDDEN);
}
/**
 * Cancel operation.
 */
static void page_playback_cancel() {
}

static bool page_playback_close_status_box() {
    lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);
    status_displayed = false;
    return status_displayed;
}

static void page_playback_on_click(uint8_t key, int sel) {
    pb_key(key);
}

static lv_obj_t *page_playback_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[128];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_PLAYBACK_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_PLAYBACK_VIEW_SIZE);
#if HDZBOXPRO
    lv_obj_set_style_pad_top(section, 68, 0);
#endif
    snprintf(buf, sizeof(buf), "%s:", _lang("Playback"));
    create_text(NULL, section, false, buf, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *cont = lv_obj_create(section);
    lv_obj_set_size(cont, UI_PAGE_PLAYBACK_VIEW_SIZE);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    // The section's left padding is inside the fixed page width. Shift the
    // whole FHD content block by one gap so its third tile stays in bounds.
    lv_obj_set_style_translate_x(cont, -ITEM_GAP_W, 0);
#endif

    for (uint32_t pos = 0; pos < ITEMS_LAYOUT_CNT; pos++) {
        pb_ui[pos]._img = lv_img_create(cont);
        lv_obj_set_size(pb_ui[pos]._img, UI_PAGE_PLAYBACK_ITEM_PREVIEW_W, UI_PAGE_PLAYBACK_ITEM_PREVIEW_H);
        lv_obj_add_flag(pb_ui[pos]._img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_style(pb_ui[pos]._img, &style_pb_dark, LV_PART_MAIN);

        pb_ui[pos]._arrow = lv_img_create(cont);
        lv_img_set_src(pb_ui[pos]._arrow, &img_arrow1);
        lv_obj_add_flag(pb_ui[pos]._arrow, LV_OBJ_FLAG_HIDDEN);

        pb_ui[pos]._star = lv_img_create(cont);
        lv_img_set_src(pb_ui[pos]._star, &img_star);
        lv_obj_add_flag(pb_ui[pos]._star, LV_OBJ_FLAG_HIDDEN);

        // .mp4 badge: a plain green dot (no icon asset for this), bottom-right
        // corner of the thumbnail.
        pb_ui[pos]._mp4_badge = lv_obj_create(cont);
        lv_obj_clear_flag(pb_ui[pos]._mp4_badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(pb_ui[pos]._mp4_badge, MP4_BADGE_SIZE, MP4_BADGE_SIZE);
        lv_obj_set_style_radius(pb_ui[pos]._mp4_badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pb_ui[pos]._mp4_badge, lv_color_hex(0x2ECC40), 0); // green
        lv_obj_set_style_bg_opa(pb_ui[pos]._mp4_badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pb_ui[pos]._mp4_badge, 0, 0);
        lv_obj_add_flag(pb_ui[pos]._mp4_badge, LV_OBJ_FLAG_HIDDEN);

        pb_ui[pos]._label = lv_label_create(cont);
        lv_obj_set_style_text_font(pb_ui[pos]._label, UI_PAGE_TEXT_FONT, 0);
        lv_obj_set_style_text_color(pb_ui[pos]._label, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
        lv_label_set_long_mode(pb_ui[pos]._label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(pb_ui[pos]._label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(pb_ui[pos]._label, LV_OBJ_FLAG_HIDDEN);

        pb_ui[pos].x = pos % ITEMS_LAYOUT_COLS;
        pb_ui[pos].y = (uint32_t)((double)(pos) / (double)(ITEMS_LAYOUT_COLS));

        pb_ui[pos].x = PB_X_START + pb_ui[pos].x * (UI_PAGE_PLAYBACK_ITEM_PREVIEW_W + ITEM_GAP_W);
        pb_ui[pos].y = PB_Y_START + pb_ui[pos].y * (UI_PAGE_PLAYBACK_ITEM_PREVIEW_H + ITEM_GAP_H);
        pb_ui[pos].state = ITEM_STATE_INVISIBLE;

        lv_obj_set_pos(pb_ui[pos]._img, pb_ui[pos].x + (ITEM_GAP_W >> 1), pb_ui[pos].y);

        lv_obj_set_pos(pb_ui[pos]._arrow, pb_ui[pos].x + (UI_PAGE_PLAYBACK_ITEM_PREVIEW_W >> 2) - 10,
                       pb_ui[pos].y + UI_PAGE_PLAYBACK_ITEM_PREVIEW_H + 10);

        lv_obj_set_pos(pb_ui[pos]._star, pb_ui[pos].x + 5, pb_ui[pos].y);

        lv_obj_set_pos(pb_ui[pos]._mp4_badge, pb_ui[pos].x + UI_PAGE_PLAYBACK_ITEM_PREVIEW_W - MP4_BADGE_SIZE - 5,
                       pb_ui[pos].y + UI_PAGE_PLAYBACK_ITEM_PREVIEW_H - MP4_BADGE_SIZE - 5);

        lv_obj_set_pos(pb_ui[pos]._label, pb_ui[pos].x + (UI_PAGE_PLAYBACK_ITEM_PREVIEW_W >> 2) + ITEM_GAP_W,
                       pb_ui[pos].y + UI_PAGE_PLAYBACK_ITEM_PREVIEW_H + 10);
    }

    // Day history strip: a "MM-YY" header once per distinct month, each
    // followed by one indented "DD" row per distinct day within it, most
    // recent at the top (matches seq order, see build_day_list), with a
    // cursor tracking whichever day row the highlighted clip is on. Text,
    // color and indent are set per-row in update_day_list_ui() since any
    // row can hold either a month header or a day, depending on the data.
    for (uint32_t i = 0; i < UI_PLAYBACK_DAYS_VISIBLE; i++) {
        day_labels[i] = lv_label_create(cont);
        lv_obj_set_style_text_font(day_labels[i], UI_PLAYBACK_DAYS_FONT, 0);
        lv_obj_set_style_text_color(day_labels[i], lv_color_hex(TEXT_COLOR_DEFAULT), 0);
        lv_obj_set_pos(day_labels[i], UI_PLAYBACK_DAYS_X, UI_PLAYBACK_DAYS_Y + i * UI_PLAYBACK_DAYS_ROW_H);
        lv_obj_add_flag(day_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    day_cursor = lv_obj_create(cont);
    lv_obj_set_size(day_cursor, 6, UI_PLAYBACK_DAYS_ROW_H - 4);
    lv_obj_set_style_bg_color(day_cursor, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(day_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(day_cursor, 0, 0);
    lv_obj_set_style_radius(day_cursor, 2, 0);
    lv_obj_set_pos(day_cursor, UI_PLAYBACK_DAYS_X, UI_PLAYBACK_DAYS_Y + 2);
    lv_obj_add_flag(day_cursor, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *label = lv_label_create(cont);
    snprintf(buf, sizeof(buf), "*%s\n**%s", _lang("Long press the Enter button to exit"), _lang("Long press the Func button for more options"));
    lv_label_set_text(label, buf);
    lv_obj_set_style_text_font(label, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(label, 10, 700);
    status = create_msgbox_item("Status", "None");
    lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);
    // The msgbox's own content area (lv_msgbox_get_content()) has no layout
    // of its own -- only the outer msgbox does -- so the text label and (a
    // few lines down) the conversion progress bar would otherwise both sit
    // at (0,0) inside it and overlap. Column flex stacks them and keeps
    // content's LV_SIZE_CONTENT height correct for whichever of them (just
    // the label, or the label + the bar) is actually visible.
    lv_obj_set_flex_flow(lv_msgbox_get_content(status), LV_FLEX_FLOW_COLUMN);

    // Long-press action menu: a small centered panel with up to 3 rows,
    // styled like the status box. Text and row count are (re)built per
    // clip in action_menu_build(); the selected row gets a solid accent
    // background (not just a text-color change) so it's actually visible.
    action_menu_bg = lv_obj_create(lv_scr_act());
    lv_obj_clear_flag(action_menu_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(action_menu_bg, UI_PAGE_MSG_BOX_SIZE);
    // Fixed height rather than LV_SIZE_CONTENT: the rows start hidden
    // (LV_OBJ_FLAG_HIDDEN, unhidden per clip in action_menu_build()), and
    // LVGL's content-size auto-sizing doesn't count hidden children, so the
    // container was sizing itself to ~one row at creation time.
    lv_obj_set_height(action_menu_bg, PB_ACTION_MAX_ROWS * 50 + 40);
    lv_obj_center(action_menu_bg);
    lv_obj_set_style_bg_color(action_menu_bg, lv_color_hex(UI_COLOR_BG_ROOT), 0);
    lv_obj_set_style_border_width(action_menu_bg, 3, 0);
    lv_obj_set_style_border_color(action_menu_bg, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_pad_all(action_menu_bg, 20, 0);
    lv_obj_add_flag(action_menu_bg, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < PB_ACTION_MAX_ROWS; i++) {
        action_menu_row[i] = lv_label_create(action_menu_bg);
        lv_obj_set_style_text_font(action_menu_row[i], UI_PAGE_TEXT_FONT, 0);
        lv_obj_set_style_text_color(action_menu_row[i], lv_color_hex(TEXT_COLOR_DEFAULT), 0);
        lv_obj_set_style_bg_opa(action_menu_row[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(action_menu_row[i], lv_color_hex(UI_STYLE_SELECT_BG_COLOR), 0);
        lv_obj_set_style_radius(action_menu_row[i], 4, 0);
        lv_obj_set_style_pad_hor(action_menu_row[i], 10, 0);
        lv_obj_set_style_pad_ver(action_menu_row[i], 6, 0);
        lv_obj_set_width(action_menu_row[i], UI_PAGE_MSG_BOX_SIZE - 40);
        lv_obj_align(action_menu_row[i], LV_ALIGN_TOP_LEFT, -10, i * 50);
        lv_obj_add_flag(action_menu_row[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Conversion progress bar: a real child of the status box's own content
    // area (not a floating widget positioned relative to it) so it's inside
    // the box's border and the box's flex layout -- already proven to
    // resize correctly for varying text, unlike the action menu's hand-rolled
    // absolute positioning -- grows to fit it automatically.
    convert_progress_bar = lv_bar_create(lv_msgbox_get_content(status));
    lv_obj_set_size(convert_progress_bar, UI_VERSION_PROGRESS_BAR_SIZE);
    lv_obj_set_width(convert_progress_bar, lv_pct(90)); // content is column flex now, so this is just sizing, not row-forcing
    lv_bar_set_range(convert_progress_bar, 0, 100);
    lv_bar_set_value(convert_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(convert_progress_bar, LV_OBJ_FLAG_HIDDEN);

    return page;
}

static void show_pb_item(uint8_t pos, char *label, bool star, bool is_mp4) {
    char fname[256];
    if (pb_ui[pos].state == ITEM_STATE_INVISIBLE) {
        lv_obj_add_flag(pb_ui[pos]._img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pb_ui[pos]._label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pb_ui[pos]._arrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pb_ui[pos]._star, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pb_ui[pos]._mp4_badge, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(pb_ui[pos]._label, label);
    lv_obj_clear_flag(pb_ui[pos]._label, LV_OBJ_FLAG_HIDDEN);

    const lv_coord_t labelPosX = pb_ui[pos].x + (UI_PAGE_PLAYBACK_ITEM_PREVIEW_W - lv_txt_get_width(label, strlen(label) - 2, &lv_font_montserrat_26, 0, 0)) / 2;
    const lv_coord_t labelPosY = pb_ui[pos].y + UI_PAGE_PLAYBACK_ITEM_PREVIEW_H + 10;
    lv_obj_set_pos(pb_ui[pos]._label, labelPosX, labelPosY);
    lv_obj_set_pos(pb_ui[pos]._arrow, labelPosX - lv_obj_get_width(pb_ui[pos]._arrow) - 5, labelPosY);

    snprintf(fname, sizeof(fname), "%s/%s." REC_packJPG, TMP_DIR, label);
    if (fs_file_exists(fname))
        snprintf(fname, sizeof(fname), "A:%s/%s." REC_packJPG, TMP_DIR, label);
    else
        osd_resource_path(fname, "%s", OSD_RESOURCE_720, DEF_VIDEOICON);
    lv_img_set_src(pb_ui[pos]._img, fname);

    if (pb_ui[pos].state == ITEM_STATE_HIGHLIGHT) {
        lv_obj_remove_style(pb_ui[pos]._img, &style_pb_dark, LV_PART_MAIN);
        lv_obj_add_style(pb_ui[pos]._img, &style_pb, LV_PART_MAIN);
        lv_obj_clear_flag(pb_ui[pos]._arrow, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_style(pb_ui[pos]._img, &style_pb, LV_PART_MAIN);
        lv_obj_add_style(pb_ui[pos]._img, &style_pb_dark, LV_PART_MAIN);
        lv_obj_add_flag(pb_ui[pos]._arrow, LV_OBJ_FLAG_HIDDEN);
    }

    if (star) {
        lv_obj_clear_flag(pb_ui[pos]._star, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pb_ui[pos]._star, LV_OBJ_FLAG_HIDDEN);
    }

    if (is_mp4) {
        lv_obj_clear_flag(pb_ui[pos]._mp4_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pb_ui[pos]._mp4_badge, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(pb_ui[pos]._img, LV_OBJ_FLAG_HIDDEN);
}

int get_videofile_cnt() {
    return media_db.count;
}

void clear_videofile_cnt() {
    media_db.count = 0;
    media_db.cur_sel = 0;
}

static media_file_node_t *get_list(int seq) {
    int seq_reserve = media_db.count - 1 - seq;
    return &media_db.list[seq_reserve];
}

// Directory a clip actually lives in -- the main clip folder, or its
// REC_favDIR subfolder for favourites. Every path built from a
// media_file_node_t goes through this instead of assuming MEDIA_FILES_DIR.
static void node_dir(const media_file_node_t *pnode, char *out, size_t outsz) {
    if (pnode->favorite)
        snprintf(out, outsz, "%s" REC_favDIR, MEDIA_FILES_DIR);
    else
        snprintf(out, outsz, "%s", MEDIA_FILES_DIR);
}

static bool get_seleteced(int seq, char *fname) {
    media_file_node_t *pnode = get_list(seq);
    if (!pnode)
        return false;
    char dir[300];
    node_dir(pnode, dir, sizeof(dir));
    sprintf(fname, "%s%s", dir, pnode->filename);
    return true;
}

static bool dvr_has_stars(const char *filename) {
    char star_file[256] = "";
    int count = snprintf(star_file, sizeof(star_file), "%s" REC_starSUFFIX, filename);

    return fs_file_exists(star_file);
}

typedef struct {
    char name[128]; // original filename, no directory component
    bool favorite;
    time_t mtime;
} pb_scan_entry_t;

static int pb_scan_compare(const void *a, const void *b) {
    const pb_scan_entry_t *ea = a;
    const pb_scan_entry_t *eb = b;
    if (ea->mtime != eb->mtime)
        return (ea->mtime < eb->mtime) ? -1 : 1;
    return strcoll(ea->name, eb->name);
}

// Appends every clip in `dir` (non-recursive) to `out`, starting at
// `out_count`, up to `max` total. Shared by the main clip folder and its
// REC_favDIR subfolder -- the only difference between the two is the
// `favorite` tag each entry gets.
static int pb_scan_dir(const char *dir, bool favorite, pb_scan_entry_t *out, int out_count, int max) {
    DIR *fd = opendir(dir);
    if (!fd)
        return out_count;

    struct dirent *in_file;
    while (out_count < max && (in_file = readdir(fd))) {
        if (in_file->d_name[0] == '.')
            continue;

        const char *dot = strrchr(in_file->d_name, '.');
        if (dot == NULL)
            continue;

        if (strcasecmp(dot, "." REC_packTS) != 0 && strcasecmp(dot, "." REC_packMP4) != 0)
            continue;

        char fname[512];
        snprintf(fname, sizeof(fname), "%s%s", dir, in_file->d_name);

        long size = fs_filesize(fname);
        size >>= 20; // in MB
        if (size < 5) {
            // skip small files
            continue;
        }

        snprintf(out[out_count].name, sizeof(out[out_count].name), "%s", in_file->d_name);
        out[out_count].favorite = favorite;
        out[out_count].mtime = fs_mtime(fname);
        out_count++;
    }
    closedir(fd);

    return out_count;
}

// One-time sweep for anyone upgrading from the old hot_-prefix scheme
// (renamed in place, same folder): relocate any leftover hot_-prefixed clip,
// and its thumbnail/star companions, into REC_favDIR, stripping the prefix.
// A no-op once none are left, so safe to run on every walk_sdcard().
static void migrate_legacy_favorites(const char *favdir) {
    DIR *fd = opendir(MEDIA_FILES_DIR);
    if (!fd)
        return;

    size_t const hot_len = strlen(REC_hotPREFIX);
    bool made_dir = false;
    struct dirent *in_file;
    while ((in_file = readdir(fd))) {
        if (strncmp(in_file->d_name, REC_hotPREFIX, hot_len) != 0)
            continue;

        const char *dot = strrchr(in_file->d_name, '.');
        if (!dot || (strcasecmp(dot, "." REC_packTS) != 0 && strcasecmp(dot, "." REC_packMP4) != 0))
            continue;

        char current_label[68]; // e.g. "hot_hdz_0001"
        snprintf(current_label, sizeof(current_label), "%.*s", (int)(dot - in_file->d_name), in_file->d_name);
        const char *stripped_name = in_file->d_name + hot_len; // "hdz_0001.ts"
        const char *stripped_label = current_label + hot_len;  // "hdz_0001"

        char dst[768];
        snprintf(dst, sizeof(dst), "%s%s", favdir, stripped_name);
        if (fs_file_exists(dst))
            // Same-named favourite already there -- leave this one where it
            // is rather than clobber it.
            continue;

        if (!made_dir) {
            char mkdir_cmd[320];
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", favdir);
            system_exec(mkdir_cmd);
            made_dir = true;
        }

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "mv %s%s %s", MEDIA_FILES_DIR, in_file->d_name, dst);
        system_exec(cmd);
        snprintf(cmd, sizeof(cmd), "mv %s%s." REC_packJPG " %s%s." REC_packJPG " 2>/dev/null",
                 MEDIA_FILES_DIR, current_label, favdir, stripped_label);
        system_exec(cmd);
        snprintf(cmd, sizeof(cmd), "mv %s%s" REC_starSUFFIX " %s%s" REC_starSUFFIX " 2>/dev/null",
                 MEDIA_FILES_DIR, in_file->d_name, favdir, stripped_name);
        system_exec(cmd);
    }
    closedir(fd);
}

static int walk_sdcard() {
    char fname[512];
    char favdir[300];
    snprintf(favdir, sizeof(favdir), "%s" REC_favDIR, MEDIA_FILES_DIR);

    migrate_legacy_favorites(favdir);

    media_db.count = 0;
    media_db.cur_sel = 0;

    static pb_scan_entry_t entries[MAX_VIDEO_FILES];
    int count = pb_scan_dir(MEDIA_FILES_DIR, false, entries, 0, MAX_VIDEO_FILES);
    count = pb_scan_dir(favdir, true, entries, count, MAX_VIDEO_FILES);
    qsort(entries, count, sizeof(entries[0]), pb_scan_compare);

    for (int i = 0; i < count; i++) {
        pb_scan_entry_t *entry = &entries[i];
        const char *dot = strrchr(entry->name, '.');

        media_file_node_t *pnode = &media_db.list[media_db.count];
        ZeroMemory(pnode->filename, sizeof(pnode->filename));
        ZeroMemory(pnode->label, sizeof(pnode->label));
        ZeroMemory(pnode->ext, sizeof(pnode->ext));
        strcpy(pnode->filename, entry->name);
        strncpy(pnode->label, entry->name, dot - entry->name);
        strcpy(pnode->ext, dot + 1);
        pnode->favorite = entry->favorite;
        pnode->mtime = entry->mtime;

        snprintf(fname, sizeof(fname), "%s%s", entry->favorite ? favdir : MEDIA_FILES_DIR, entry->name);
        pnode->star = dvr_has_stars(fname);
        pnode->size = fs_filesize(fname) >> 20;

        LOGI("%d: %s-%dMB%s", media_db.count, pnode->filename, pnode->size, entry->favorite ? " (favorite)" : "");

        media_db.count++;
    }

    // copy all thumbnail files (both folders) to /tmp
    snprintf(fname, sizeof(fname), "cp %s*." REC_packJPG " %s 2>/dev/null; cp %s*." REC_packJPG " %s 2>/dev/null",
             MEDIA_FILES_DIR, TMP_DIR, favdir, TMP_DIR);
    system_exec(fname);

    build_day_list();
    update_day_list_ui();

    return media_db.count;
}

// Walks seq order (0 = most recent, see get_list()) and builds the display
// rows: a "MM-YY" header the first time a month is seen, then one "DD" row
// per distinct day (within that month) that follows it. Capped at
// UI_PLAYBACK_DAYS_VISIBLE (and the backing array's MAX_DAY_ENTRIES): once a
// month header + its next day row wouldn't both fit, older entries just
// don't get a row rather than overflowing the list or leaving an orphaned
// header with no day underneath.
static void build_day_list(void) {
    day_count = 0;
    char last_month_label[8] = "";
    int last_mday = -1;

    for (int seq = 0; seq < media_db.count; seq++) {
        media_file_node_t *pnode = get_list(seq);
        struct tm tmv;
        localtime_r(&pnode->mtime, &tmv);

        char month_label[8];
        snprintf(month_label, sizeof(month_label), "%02d-%02d", tmv.tm_mon + 1, tmv.tm_year % 100);

        bool new_month = strcmp(last_month_label, month_label) != 0;
        if (!new_month && tmv.tm_mday == last_mday)
            continue; // same calendar day as the last row -- no new row needed

        int rows_needed = new_month ? 2 : 1; // month header (if new) + the day row itself
        if (day_count + rows_needed > UI_PLAYBACK_DAYS_VISIBLE || day_count + rows_needed > MAX_DAY_ENTRIES)
            break;

        if (new_month) {
            day_list[day_count].kind = PB_DAY_ROW_MONTH;
            snprintf(day_list[day_count].label, sizeof(day_list[day_count].label), "%s", month_label);
            day_list[day_count].start_seq = seq;
            day_count++;
            snprintf(last_month_label, sizeof(last_month_label), "%s", month_label);
        }

        day_list[day_count].kind = PB_DAY_ROW_DAY;
        snprintf(day_list[day_count].label, sizeof(day_list[day_count].label), "%02d", tmv.tm_mday);
        day_list[day_count].start_seq = seq;
        day_count++;

        last_mday = tmv.tm_mday;
    }
}

static void update_day_list_ui(void) {
    for (uint32_t i = 0; i < UI_PLAYBACK_DAYS_VISIBLE; i++) {
        if ((int)i < day_count) {
            bool is_month = day_list[i].kind == PB_DAY_ROW_MONTH;
            lv_label_set_text(day_labels[i], day_list[i].label);
            lv_obj_set_style_text_color(day_labels[i],
                                        lv_color_hex(is_month ? UI_COLOR_ACCENT : TEXT_COLOR_DEFAULT), 0);
            lv_obj_set_pos(day_labels[i], UI_PLAYBACK_DAYS_X + (is_month ? 0 : 22),
                          UI_PLAYBACK_DAYS_Y + i * UI_PLAYBACK_DAYS_ROW_H);
            lv_obj_clear_flag(day_labels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(day_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Repoints the cursor at whichever day row media_db.cur_sel currently falls
// on (month header rows are never a cursor target). Rows are ordered by
// increasing start_seq (top = most recent), so the target is the last DAY
// row whose start_seq doesn't exceed cur_sel.
static void update_day_cursor(void) {
    int idx = -1;
    for (int i = 0; i < day_count; i++) {
        if (day_list[i].kind != PB_DAY_ROW_DAY)
            continue;
        if (day_list[i].start_seq <= media_db.cur_sel)
            idx = i;
        else
            break;
    }

    if (idx < 0) {
        lv_obj_add_flag(day_cursor, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(day_cursor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(day_cursor, UI_PLAYBACK_DAYS_X, UI_PLAYBACK_DAYS_Y + idx * UI_PLAYBACK_DAYS_ROW_H + 2);
}

static void update_page() {
    uint32_t const page_num = (uint32_t)floor((double)media_db.cur_sel / ITEMS_LAYOUT_CNT);
    uint32_t const end_pos = media_db.count - page_num * ITEMS_LAYOUT_CNT;
    uint32_t const cur_pos = media_db.cur_sel - page_num * ITEMS_LAYOUT_CNT;

    for (uint8_t i = 0; i < ITEMS_LAYOUT_CNT; i++) {
        uint32_t seq = i + page_num * ITEMS_LAYOUT_CNT;
        if (seq < media_db.count) {
            media_file_node_t *pnode = get_list(seq);
            if (!pnode) {
                perror("update_page failed.");
                return;
            }

            if (i < cur_pos)
                pb_ui[i].state = ITEM_STATE_NORMAL;
            else if (i == cur_pos)
                pb_ui[i].state = ITEM_STATE_HIGHLIGHT;
            else if (i < end_pos)
                pb_ui[i].state = ITEM_STATE_NORMAL;
            else
                pb_ui[i].state = ITEM_STATE_INVISIBLE;

            // Favourites no longer carry a visible hot_ prefix in the label
            // (they live in their own folder instead), so reuse the star
            // icon to keep them distinguishable in the grid.
            show_pb_item(i, pnode->label, pnode->star || pnode->favorite, strcasecmp(pnode->ext, REC_packMP4) == 0);
        } else {
            pb_ui[i].state = ITEM_STATE_INVISIBLE;
            show_pb_item(i, NULL, false, false);
        }
    }

    update_day_cursor();
}

static void update_item(uint8_t cur_pos, uint8_t lst_pos) {
    if (cur_pos == lst_pos) {
        return;
    }

    lv_obj_clear_flag(pb_ui[cur_pos]._arrow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_style(pb_ui[cur_pos]._img, &style_pb_dark, LV_PART_MAIN);
    lv_obj_add_style(pb_ui[cur_pos]._img, &style_pb, LV_PART_MAIN);

    lv_obj_add_flag(pb_ui[lst_pos]._arrow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_style(pb_ui[lst_pos]._img, &style_pb, LV_PART_MAIN);
    lv_obj_add_style(pb_ui[lst_pos]._img, &style_pb_dark, LV_PART_MAIN);
}

// Moves a clip (and its thumbnail/star companions) into REC_favDIR, keeping
// its original filename -- favourites become their own folder rather than a
// renamed file mixed in with everything else, so they're easy to find when
// the card is plugged into a computer.
static void mark_video_file(int const seq) {
    media_file_node_t const *const pnode = get_list(seq);
    if (!pnode || pnode->favorite) {
        // already a favourite
        return;
    }

    char favdir[300];
    snprintf(favdir, sizeof(favdir), "%s" REC_favDIR, MEDIA_FILES_DIR);

    char dst[512];
    snprintf(dst, sizeof(dst), "%s%s", favdir, pnode->filename);
    if (fs_file_exists(dst)) {
        // Name collision (e.g. the DVR index counter wrapped) -- refuse
        // rather than silently overwriting an existing favourite.
        LOGE("mark_video_file: %s already exists, skipping", dst);
        return;
    }

    char mkdir_cmd[320];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", favdir);
    system_exec(mkdir_cmd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mv %s%s %s", MEDIA_FILES_DIR, pnode->filename, favdir);
    system_exec(cmd);
    snprintf(cmd, sizeof(cmd), "mv %s%s." REC_packJPG " %s 2>/dev/null", MEDIA_FILES_DIR, pnode->label, favdir);
    system_exec(cmd);
    snprintf(cmd, sizeof(cmd), "mv %s%s" REC_starSUFFIX " %s 2>/dev/null", MEDIA_FILES_DIR, pnode->filename, favdir);
    system_exec(cmd);

    walk_sdcard();
    media_db.cur_sel = constrain(seq, 0, (media_db.count - 1));
    update_page();
}

// Moves a favourited clip (and its companions) back out of REC_favDIR into
// the main clip folder -- the inverse of mark_video_file().
static void unmark_video_file(int const seq) {
    media_file_node_t const *const pnode = get_list(seq);
    if (!pnode || !pnode->favorite) {
        return;
    }

    char favdir[300];
    snprintf(favdir, sizeof(favdir), "%s" REC_favDIR, MEDIA_FILES_DIR);

    char dst[512];
    snprintf(dst, sizeof(dst), "%s%s", MEDIA_FILES_DIR, pnode->filename);
    if (fs_file_exists(dst)) {
        // Name collision -- refuse rather than silently overwriting.
        LOGE("unmark_video_file: %s already exists, skipping", dst);
        return;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mv %s%s %s", favdir, pnode->filename, MEDIA_FILES_DIR);
    system_exec(cmd);
    snprintf(cmd, sizeof(cmd), "mv %s%s." REC_packJPG " %s 2>/dev/null", favdir, pnode->label, MEDIA_FILES_DIR);
    system_exec(cmd);
    snprintf(cmd, sizeof(cmd), "mv %s%s" REC_starSUFFIX " %s 2>/dev/null", favdir, pnode->filename, MEDIA_FILES_DIR);
    system_exec(cmd);

    walk_sdcard();
    media_db.cur_sel = constrain(seq, 0, (media_db.count - 1));
    update_page();
}

static void toggle_favorite(int const seq) {
    media_file_node_t const *const pnode = get_list(seq);
    if (!pnode)
        return;
    if (pnode->favorite)
        unmark_video_file(seq);
    else
        mark_video_file(seq);
}

// The helper binary and, deliberately, its log + result + progress files --
// on the SD card (not /tmp, which is on internal storage the SD card
// doesn't expose) so a failure can actually be diagnosed by pulling the card.
#define TS2MP4_BIN           "/mnt/app/app/record/ts2mp4"
#define TS2MP4_LOG_FILE      REC_diskPATH "/ts2mp4.log"
#define TS2MP4_RESULT_FILE   REC_diskPATH "/ts2mp4.result"
#define TS2MP4_PROGRESS_FILE REC_diskPATH "/ts2mp4.progress"

// Three short beeps, evenly spaced -- beep_dur() just signals a single-slot
// beeper thread (see driver/beep.c), so calling it back-to-back without a
// gap would just overwrite the pending beep instead of queuing three.
static void beep_triple(void) {
    for (int i = 0; i < 3; i++) {
        beep();
        if (i < 2)
            usleep(150000);
    }
}

// Runs the ts2mp4 helper out of process (this process has no ffmpeg linked)
// and waits for it, bounded, polling a result file -- same defensive pattern
// as the SD format/repair flows in page_storage.c. Deletes the source .ts
// only once the .mp4 is confirmed written; on any failure the source is left
// untouched and any partial .mp4 is removed.
static void *convert_thread(void *arg) {
    (void)arg;
    unlink(TS2MP4_RESULT_FILE);
    unlink(TS2MP4_PROGRESS_FILE);

    // Grab the source clip's timestamps before it's converted (and, on
    // success, deleted) -- a freshly-written .mp4 would otherwise carry
    // today's date instead of when the clip was actually recorded, throwing
    // off the recency sort on this page.
    struct stat src_st;
    bool const have_src_stat = (stat(convert_ts_path, &src_st) == 0);

    // Wrapped in a subshell so the WHOLE sequence backgrounds -- "a; b &"
    // only backgrounds b, which would leave system_exec() blocking on the
    // helper itself (harmless here since we're already off the LVGL thread,
    // but it defeats the point of the bounded poll below).
    char cmd[1400];
    snprintf(cmd, sizeof(cmd),
             "( LD_LIBRARY_PATH=/lib/libffmpeg:/lib/eyesee-mpp %s \"%s\" \"%s\" \"%s\" > %s 2>&1; echo $? > %s ) &",
             TS2MP4_BIN, convert_ts_path, convert_mp4_path, TS2MP4_PROGRESS_FILE, TS2MP4_LOG_FILE, TS2MP4_RESULT_FILE);
    system_exec(cmd);

    int elapsed = 0;
    while (!fs_file_exists(TS2MP4_RESULT_FILE) && ++elapsed < 300) {
        int percent = -1;
        FILE *pf = fopen(TS2MP4_PROGRESS_FILE, "r");
        if (pf) {
            if (fscanf(pf, "%d", &percent) != 1)
                percent = -1;
            fclose(pf);
        }
        if (percent >= 0) {
            pthread_mutex_lock(&lvgl_mutex);
            lv_bar_set_value(convert_progress_bar, constrain(percent, 0, 100), LV_ANIM_OFF);
            pthread_mutex_unlock(&lvgl_mutex);
        }
        sleep(1);
    }

    bool ok = false;
    FILE *f = fopen(TS2MP4_RESULT_FILE, "r");
    if (f) {
        int code = -1;
        if (fscanf(f, "%d", &code) == 1)
            ok = (code == 0);
        fclose(f);
    }
    ok = ok && fs_file_exists(convert_mp4_path) && fs_filesize(convert_mp4_path) > 0;

    if (ok && have_src_stat) {
        struct utimbuf times = {.actime = src_st.st_atime, .modtime = src_st.st_mtime};
        utime(convert_mp4_path, &times);
    }

    if (ok) {
        char rm_cmd[600];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm \"%s\"", convert_ts_path);
        system_exec(rm_cmd);
    } else {
        unlink(convert_mp4_path); // don't leave a partial/broken file behind
    }

    beep_triple();

    pthread_mutex_lock(&lvgl_mutex);
    lv_obj_add_flag(convert_progress_bar, LV_OBJ_FLAG_HIDDEN);
    walk_sdcard(); // resets cur_sel to 0 (most recent) -- re-find the converted clip below instead
    if (ok) {
        char dir[300];
        for (int seq = 0; seq < media_db.count; seq++) {
            media_file_node_t const *const n = get_list(seq);
            node_dir(n, dir, sizeof(dir));
            char full[560];
            snprintf(full, sizeof(full), "%s%s", dir, n->filename);
            if (strcmp(full, convert_mp4_path) == 0) {
                media_db.cur_sel = seq;
                break;
            }
        }
    }
    update_page();
    page_playback_open_status_box(ok ? "Converted" : "Conversion failed",
                                   ok ? "Saved as .mp4." : "Could not convert this clip.\nSee ts2mp4.log at the SD card root.");
    status_deleting = true; // dismissible via RIGHT_KEY_CLICK/PRESS
    status_is_delete_confirm = false;
    pthread_mutex_unlock(&lvgl_mutex);

    convert_running = false;
    return NULL;
}

static void start_convert(int const seq) {
    media_file_node_t const *const pnode = get_list(seq);
    if (!pnode || convert_running || strcasecmp(pnode->ext, REC_packTS) != 0)
        return;

    if (!fs_file_exists(TS2MP4_BIN)) {
        page_playback_open_status_box("Convert to MP4", "ts2mp4 helper not found on this firmware.");
        status_deleting = true;
        status_is_delete_confirm = false;
        return;
    }

    char dir[300];
    node_dir(pnode, dir, sizeof(dir));

    char mp4_path[512];
    snprintf(mp4_path, sizeof(mp4_path), "%s%s.%s", dir, pnode->label, REC_packMP4);
    if (fs_file_exists(mp4_path)) {
        page_playback_open_status_box("Convert to MP4", "An .mp4 with this name already exists.");
        status_deleting = true;
        status_is_delete_confirm = false;
        return;
    }

    snprintf(convert_ts_path, sizeof(convert_ts_path), "%s%s", dir, pnode->filename);
    snprintf(convert_mp4_path, sizeof(convert_mp4_path), "%s", mp4_path);

    page_playback_open_status_box("Converting to MP4", "This can take a while for large clips.");
    status_deleting = true;
    status_is_delete_confirm = false;
    lv_bar_set_value(convert_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(convert_progress_bar, LV_OBJ_FLAG_HIDDEN);

    convert_running = true;
    pthread_t tid;
    if (pthread_create(&tid, NULL, convert_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        LOGE("start_convert: pthread_create failed");
        convert_running = false;
        lv_obj_add_flag(convert_progress_bar, LV_OBJ_FLAG_HIDDEN);
        status_deleting = page_playback_close_status_box();
    }
}

#define TS2MP4_INFO_FILE        REC_diskPATH "/ts2mp4.info"
#define TS2MP4_INFO_RESULT_FILE REC_diskPATH "/ts2mp4.info.result"

// Matches ts2mp4/main.c's DVR_VIDEO_WIDTH/HEIGHT (a separate process/binary,
// so not literally shared) -- every DVR clip is this fixed resolution.
#define INFO_VIDEO_WIDTH  1280
#define INFO_VIDEO_HEIGHT 720

// Date/size/extension come straight from what walk_sdcard() already scanned
// (media_file_node_t); duration and fps don't (they're not read off the
// filesystem), so those two go through a quick ts2mp4 --info probe -- same
// out-of-process pattern as the conversion, but read-only, no muxing.
// Resolution isn't probed either: every DVR clip is a fixed resolution
// (INFO_VIDEO_WIDTH/HEIGHT), so it's just stated directly.
static bool info_running;
static char info_ts_path[512];
static char info_date_str[32];
static char info_ext_str[16];
static int info_size_mb;

static void *info_thread(void *arg) {
    (void)arg;
    unlink(TS2MP4_INFO_RESULT_FILE);
    unlink(TS2MP4_INFO_FILE);

    char cmd[1200];
    snprintf(cmd, sizeof(cmd),
             "( LD_LIBRARY_PATH=/lib/libffmpeg:/lib/eyesee-mpp %s --info \"%s\" \"%s\" > %s 2>&1; echo $? > %s ) &",
             TS2MP4_BIN, info_ts_path, TS2MP4_INFO_FILE, TS2MP4_LOG_FILE, TS2MP4_INFO_RESULT_FILE);
    system_exec(cmd);

    int elapsed = 0;
    while (!fs_file_exists(TS2MP4_INFO_RESULT_FILE) && ++elapsed < 30) {
        sleep(1);
    }

    int duration_ms = -1, fps = -1;
    FILE *f = fopen(TS2MP4_INFO_FILE, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            sscanf(line, "duration_ms=%d", &duration_ms);
            sscanf(line, "fps=%d", &fps);
        }
        fclose(f);
    }

    char dur_buf[16];
    if (duration_ms >= 0)
        snprintf(dur_buf, sizeof(dur_buf), "%02d:%02d", (duration_ms / 1000) / 60, (duration_ms / 1000) % 60);
    else
        snprintf(dur_buf, sizeof(dur_buf), "?");

    char fps_buf[8];
    if (fps > 0)
        snprintf(fps_buf, sizeof(fps_buf), "%d", fps);
    else
        snprintf(fps_buf, sizeof(fps_buf), "?");

    char text[320];
    snprintf(text, sizeof(text),
             "Date: %s\nDuration: %s\nSize: %d MB\nFPS: %s\nResolution: %dx%d\nFormat: .%s",
             info_date_str, dur_buf, info_size_mb, fps_buf, INFO_VIDEO_WIDTH, INFO_VIDEO_HEIGHT, info_ext_str);

    pthread_mutex_lock(&lvgl_mutex);
    page_playback_open_status_box("Info", text);
    status_deleting = true; // dismissible via RIGHT_KEY_CLICK/PRESS (and any roller turn, see DIAL_KEY_UP/DOWN)
    status_is_delete_confirm = false;
    pthread_mutex_unlock(&lvgl_mutex);

    info_running = false;
    return NULL;
}

static void start_info(int const seq) {
    media_file_node_t const *const pnode = get_list(seq);
    if (!pnode || info_running || convert_running)
        return;

    char dir[300];
    node_dir(pnode, dir, sizeof(dir));
    snprintf(info_ts_path, sizeof(info_ts_path), "%s%s", dir, pnode->filename);
    snprintf(info_ext_str, sizeof(info_ext_str), "%s", pnode->ext);
    info_size_mb = pnode->size;

    struct tm tmv;
    localtime_r(&pnode->mtime, &tmv);
    snprintf(info_date_str, sizeof(info_date_str), "%02d-%02d-%02d %02d:%02d",
             tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year % 100, tmv.tm_hour, tmv.tm_min);

    page_playback_open_status_box("Info", "Reading clip info...");
    status_deleting = true;
    status_is_delete_confirm = false;

    info_running = true;
    pthread_t tid;
    if (pthread_create(&tid, NULL, info_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        LOGE("start_info: pthread_create failed");
        info_running = false;
        status_deleting = page_playback_close_status_box();
    }
}

// Builds the visible rows for the clip at `seq`: Favorite (toggle label),
// Convert to MP4 (only for .ts clips), Remove, Info -- in that order.
static void action_menu_build(int seq) {
    media_file_node_t const *const pnode = get_list(seq);
    action_menu_row_count = 0;
    if (!pnode)
        return;

    action_menu_action[action_menu_row_count++] = PB_ACTION_FAVORITE;
    if (strcasecmp(pnode->ext, REC_packTS) == 0)
        action_menu_action[action_menu_row_count++] = PB_ACTION_CONVERT;
    action_menu_action[action_menu_row_count++] = PB_ACTION_REMOVE;
    action_menu_action[action_menu_row_count++] = PB_ACTION_INFO;

    for (int i = 0; i < PB_ACTION_MAX_ROWS; i++) {
        if (i >= action_menu_row_count) {
            lv_obj_add_flag(action_menu_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const char *text;
        switch (action_menu_action[i]) {
        case PB_ACTION_FAVORITE:
            text = pnode->favorite ? _lang("Remove from Favorites") : _lang("Add to Favorites");
            break;
        case PB_ACTION_CONVERT:
            text = _lang("Convert to MP4");
            break;
        case PB_ACTION_REMOVE:
            text = _lang("Remove");
            break;
        default: // PB_ACTION_INFO
            text = _lang("Info");
            break;
        }
        lv_label_set_text(action_menu_row[i], text);
        lv_obj_clear_flag(action_menu_row[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void action_menu_update_highlight(void) {
    for (int i = 0; i < action_menu_row_count; i++) {
        bool const sel = (i == action_menu_sel);
        lv_obj_set_style_bg_opa(action_menu_row[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(action_menu_row[i], lv_color_hex(sel ? UI_COLOR_ACCENT : TEXT_COLOR_DEFAULT), 0);
    }
}

static void action_menu_open_for(int seq) {
    action_menu_build(seq);
    if (action_menu_row_count == 0)
        return;
    action_menu_sel = 0;
    action_menu_update_highlight();
    action_menu_open = true;
    lv_obj_clear_flag(action_menu_bg, LV_OBJ_FLAG_HIDDEN);
}

static void action_menu_close(void) {
    action_menu_open = false;
    lv_obj_add_flag(action_menu_bg, LV_OBJ_FLAG_HIDDEN);
}

static void delete_video_file(int seq) {
    media_file_node_t const *const pnode = get_list(seq);
    if (!pnode) {
        LOGE("delete_video_file failed. (PNODE ERROR)");
        return;
    }

    char dir[300];
    node_dir(pnode, dir, sizeof(dir));
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm %s%s.*", dir, pnode->label);

    if (system_exec(cmd) != -1) {
        walk_sdcard();
        sdcard_update_free_size(); // refresh cached free space shown in the status bar
        media_db.cur_sel = constrain(seq, 0, (media_db.count - 1));
        update_page();
        LOGD("delete_video_file successful.");
    } else {
        LOGE("delete_video_file failed.");
    }
}

// Absorbs the back gesture (physical dial long-press) while the action menu
// is open: btn_press() (input_device.c) routes a long-press straight to
// submenu_back() for APP_STATE_SUBMENU, bypassing pb_key()/its own
// DIAL_KEY_PRESS handling entirely -- so without this, long-pressing while
// the menu was open exited Playback with the menu panel left stuck on
// screen (action_menu_open never reset, action_menu_bg never re-hidden).
static bool page_playback_on_back(void) {
    if (action_menu_open) {
        action_menu_close();
        return true; // stay on the page
    }
    return false; // fall through to the normal exit
}

static void page_playback_exit() {
    action_menu_close(); // defensive: don't leave the menu panel stuck on screen
    page_playback_close_status_box();
    clear_videofile_cnt();
    update_page();
}

static void page_playback_enter() {
    const int ret = walk_sdcard();
    update_page();

    if (ret == 0) {
        // no files found, back out
        submenu_exit();
    }
}

void pb_key(uint8_t const key) {
    static bool done = true;
    static uint8_t state = 0; // 0= select video files, 1=playback
    char fname[128];
    uint32_t cur_page_num, lst_page_num;
    uint8_t cur_pos, lst_pos;

    if (state == 1) {
        if (mplayer_on_key(key)) {
            state = 0;
            app_state_push(APP_STATE_SUBMENU);
        }
        return;
    }

    if (!key || !media_db.count || (!done && status_displayed && !status_deleting)) {
        return;
    }

    if (action_menu_open) {
        switch (key) {
        case DIAL_KEY_UP:
            action_menu_sel = (action_menu_sel + 1) % action_menu_row_count;
            action_menu_update_highlight();
            break;

        case DIAL_KEY_DOWN:
            action_menu_sel = (action_menu_sel + action_menu_row_count - 1) % action_menu_row_count;
            action_menu_update_highlight();
            break;

        case DIAL_KEY_CLICK: {
            pb_action_t const action = action_menu_action[action_menu_sel];
            action_menu_close();
            switch (action) {
            case PB_ACTION_FAVORITE:
                toggle_favorite(media_db.cur_sel);
                break;
            case PB_ACTION_CONVERT:
                start_convert(media_db.cur_sel);
                break;
            case PB_ACTION_REMOVE:
                page_playback_open_status_box("Are you sure you want to DELETE the file",
                                               "Click center of dial to continue.\nClick function(right button) or scroll to exit.");
                status_deleting = true;
                status_is_delete_confirm = true;
                break;
            case PB_ACTION_INFO:
                start_info(media_db.cur_sel);
                break;
            }
            break;
        }

        case DIAL_KEY_PRESS:
        case RIGHT_KEY_CLICK:
            action_menu_close();
            break;

        default:
            break;
        }
        return;
    }

    done = false;
    switch (key) {
    case DIAL_KEY_UP: // up
        if (status_displayed) {
            page_playback_close_status_box();
            break;
        }
        lst_page_num = (uint32_t)floor((double)media_db.cur_sel / ITEMS_LAYOUT_CNT);
        lst_pos = media_db.cur_sel - lst_page_num * ITEMS_LAYOUT_CNT;

        if (media_db.cur_sel == (media_db.count - 1)) {
            media_db.cur_sel = 0;
        } else {
            media_db.cur_sel++;
        }

        cur_page_num = (uint32_t)floor((double)media_db.cur_sel / ITEMS_LAYOUT_CNT);
        cur_pos = media_db.cur_sel - cur_page_num * ITEMS_LAYOUT_CNT;

        if (lst_page_num == cur_page_num) {
            update_item(cur_pos, lst_pos);
            update_day_cursor();
        } else {
            update_page();
        }
        break;

    case DIAL_KEY_DOWN: // down
        if (status_displayed) {
            page_playback_close_status_box();
            break;
        }
        lst_page_num = (uint32_t)floor((double)media_db.cur_sel / ITEMS_LAYOUT_CNT);
        lst_pos = media_db.cur_sel - lst_page_num * ITEMS_LAYOUT_CNT;

        if (media_db.cur_sel) {
            media_db.cur_sel--;
        } else {
            media_db.cur_sel = (media_db.count - 1);
        }

        cur_page_num = (uint32_t)floor((double)media_db.cur_sel / ITEMS_LAYOUT_CNT);
        cur_pos = media_db.cur_sel - cur_page_num * ITEMS_LAYOUT_CNT;

        if (lst_page_num == cur_page_num) {
            update_item(cur_pos, lst_pos);
            update_day_cursor();
        } else {
            update_page();
        }
        break;

    case DIAL_KEY_CLICK: // Enter
        if (status_displayed) {
            if (status_is_delete_confirm)
                delete_video_file(media_db.cur_sel);
            status_deleting = page_playback_close_status_box();
            status_is_delete_confirm = false;
        } else if (get_seleteced(media_db.cur_sel, fname)) {
            mplayer_file(fname);
            state = 1;
            app_state_push(APP_STATE_PLAYBACK);
        }
        break;

    case DIAL_KEY_PRESS: // long press
        status_deleting = page_playback_close_status_box();
        page_playback_exit();
        break;

    case RIGHT_KEY_CLICK:
        if (status_displayed) {
            status_deleting = page_playback_close_status_box();
            status_is_delete_confirm = false;
        }
        // Short func press otherwise does nothing -- Favorite now only lives
        // in the long-press action menu (RIGHT_KEY_PRESS).
        break;

    case RIGHT_KEY_PRESS:
        if (status_displayed) {
            page_playback_close_status_box();
            status_is_delete_confirm = false;
        } else if (!convert_running && !info_running) {
            action_menu_open_for(media_db.cur_sel);
        }
        break;
    }
}
static void page_playback_on_roller(uint8_t key) {
    pb_key(key);
}

static void page_playback_on_right_button(bool is_short) {
    pb_key(is_short ? RIGHT_KEY_CLICK : RIGHT_KEY_PRESS);
}

page_pack_t pp_playback = {
    .name = "Playback",
    .create = page_playback_create,
    .enter = page_playback_enter,
    .exit = page_playback_exit,
    .on_back = page_playback_on_back,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = page_playback_on_roller,
    .on_click = page_playback_on_click,
    .on_right_button = page_playback_on_right_button,
};
