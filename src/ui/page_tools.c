#include "page_tools.h"

#include <minIni.h>
#include <stdio.h>
#include <string.h>

#include "../conf/ui.h"

#include "core/app_state.h"
#include "core/battery.h"
#include "core/osd.h"
#include "core/scan_core.h"
#include "core/settings.h"
#include "driver/fans.h"
#include "driver/hardware.h"
#include "driver/nct75.h"
#include "driver/rtc6715.h"
#include "lang/language.h"
#include "core/sleep_mode.h"
#include "ui/page_scannow.h"
#include "ui/page_wifi.h"
#include "ui/ui_porting.h"
#include "ui/ui_style.h"
#include "ui/ui_theme.h"

// RSSI Scanner sweeps the built-in analog receiver (RTC6715), which only the
// G2 and Box Pro have: its RSSI pin reports RF energy in the tuned channel
// whatever the modulation, so it shows any activity even when nothing is a
// valid HDZero signal. The G1 has no such receiver, so it has no scanner.
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
#define RSSI_SCAN_ANALOG 1
#endif

// Tools: a grab bag of one-off utilities that don't warrant their own
// sidebar entry each -- Focus Chart and Frequency Chart just show a
// fullscreen reference image, RSSI Scanner sweeps every HDZero channel in a
// loop and charts it, Theme cycles the UI colour theme.
#define ROW_WIFI_SHARE  0
#define ROW_FOCUS_CHART 1
#define ROW_FREQ_CHART  2
#ifdef RSSI_SCAN_ANALOG
#define ROW_RSSI_SCAN   3
#define ROW_RSSI_RANGE  4
#define ROW_RSSI_STEP   5
#define ROW_SCAN_PAGE   6
#define ROW_TEMPERATURE 7
#ifndef HDZBOXPRO
#define ROW_THEME    8
#define ROW_BACK     9
#define TOOLS_ROW_COUNT 10
#else
// Box Pro never offered the Theme page (ui_main_menu.c used to gate it out
// with #if !defined(HDZBOXPRO)); keep that behaviour here.
#define ROW_BACK        8
#define TOOLS_ROW_COUNT 9
#endif
#else
#define ROW_TEMPERATURE 3
#define ROW_THEME       4
#define ROW_BACK        5
#define TOOLS_ROW_COUNT 6
#endif

#define SWATCH_COUNT 4
#define SWATCH_DOT 30 // diameter of a colour dot
#define SWATCH_GAP 14

#define RSSI_SCAN_CH_MAX 300 // 48 channels, or up to (5945-5361)/2+1 = 293 points in 2 MHz mode

static lv_coord_t col_dsc[] = {160, 160, 160, 160, 160, 160, LV_GRID_TEMPLATE_LAST};
// Thin rows (same as Record Option) so the rows and the note below them fit.
#ifdef HDZBOXPRO
#define TOOLS_ROW_H 32
#else
#define TOOLS_ROW_H 50
#endif
#define H TOOLS_ROW_H
static lv_coord_t row_dsc[] = {H, H, H, H, H, H, H, H, H, H, H, H, H, H, LV_GRID_TEMPLATE_LAST};
#undef H

static lv_obj_t *chart_img;
static bool chart_open;

// Temperature info row: g_temperature is refreshed by thread_peripheral every
// few seconds, in tenths of a degree C (same unit the page_fans.c thresholds use).
static lv_obj_t *temp_label;
static char temp_text[96];

#ifdef RSSI_SCAN_ANALOG
typedef struct {
    uint16_t freq_mhz;
    int8_t   analog_idx; // rtc6715 channel index
} rssi_scan_ch_t;

static rssi_scan_ch_t rssi_channels[RSSI_SCAN_CH_MAX];
static int rssi_ch_count;
static lv_obj_t *rssi_cont;
static lv_obj_t *rssi_chart;
static lv_chart_series_t *rssi_series;
static lv_obj_t *rssi_status_label;
static bool rssi_scan_active;
static int rssi_scan_idx;

// Sweep width: which slice of the spectrum the scanner covers.
enum { RSSI_RANGE_FULL = 0, RSSI_RANGE_LOWBAND, RSSI_RANGE_STANDARD, RSSI_RANGE_COUNT };
static const char *const rssi_range_name[RSSI_RANGE_COUNT] = {"Full", "Lowband", "Standard"};
static int rssi_range_mode;
// What the Scan page does: its normal scan, or open the RSSI Scanner instead.
static bool scan_page_rssi;
static lv_obj_t *scan_page_label;
static lv_obj_t *rssi_range_label;

// Sweep step: one point per analog channel, or a free sweep at a fixed step.
// The RTC6715 synth steps in 2 MHz, so every step is an exact multiple of it
// and 2 MHz is the finest there is.
enum { RSSI_STEP_CHANNELS = 0, RSSI_STEP_COARSE, RSSI_STEP_FINE, RSSI_STEP_COUNT };
static const char *const rssi_step_name[RSSI_STEP_COUNT] = {"Channels", "Coarse 4 MHz", "Fine 2 MHz"};
static const int rssi_step_mhz[RSSI_STEP_COUNT] = {0, 4, 2};
#define RSSI_FINE_SETTLE_US 40000 // PLL settle per point; adjacent points are a short hop
static int rssi_step_mode;
static lv_obj_t *rssi_step_label;
static lv_obj_t *rssi_lo_lbl, *rssi_hi_lbl;   // frequency labels under the plot corners
static lv_obj_t *band_lbl[48];                // band channel markers, A,B,E,F,R,L x 8
static lv_coord_t rssi_plot_w, rssi_plot_left;
#endif

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

#ifdef RSSI_SCAN_ANALOG
static bool rssi_borrowed_internal; // Expansion module switched off while scanning

// Auto range: the Y axis starts at RSSI_MV_BASE_MIN..RSSI_MV_BASE_MAX and only
// ever widens during a scan, so a strong emitter stretches the axis instead
// of clipping at the top. Raw mV is kept per frequency so the whole trace can
// be re-plotted when the range changes.
#define RSSI_MV_BASE_MIN 640
#define RSSI_MV_BASE_MAX 1200
static int rssi_lo_mv = RSSI_MV_BASE_MIN;
static int rssi_hi_mv = RSSI_MV_BASE_MAX;
static int rssi_mv[RSSI_SCAN_CH_MAX]; // -1 = not measured yet
static lv_obj_t *rssi_tick[5];        // Y axis labels, 0/25/50/75/100% of the range
static lv_coord_t rssi_plot_top, rssi_plot_h;

// The chart doesn't label its own axis: (re)place the mV tick labels along the
// left edge of the plot area.
static void update_rssi_ticks(void) {
    for (int i = 0; i < 5; i++) {
        int v = i * 25;
        char tick[12];
        snprintf(tick, sizeof(tick), "%d", rssi_lo_mv + (rssi_hi_mv - rssi_lo_mv) * v / 100);
        lv_label_set_text(rssi_tick[i], tick);
        lv_obj_update_layout(rssi_tick[i]);
        lv_obj_align_to(rssi_tick[i], rssi_chart, LV_ALIGN_OUT_LEFT_TOP, -8,
                        rssi_plot_top + rssi_plot_h - (rssi_plot_h * v / 100) -
                            lv_obj_get_height(rssi_tick[i]) / 2);
    }
}

// Scatter chart: each point carries its frequency as X, so the horizontal axis
// is linear in MHz (an index-based line chart spaced points evenly regardless
// of how far apart their frequencies are).
static void set_rssi_point(lv_chart_series_t *ser, int idx, lv_coord_t y) {
    lv_chart_set_value_by_id2(rssi_chart, ser, idx, rssi_channels[idx].freq_mhz, y);
}

// Frequency window of a sweep width mode, taken from the analog band tables
// (scan_analog_idx_to_mhz is A,B,E,F,R,L x 8): Lowband = L1..L8, Standard =
// E1..E8 (5645..5945). Full is unbounded.
static void rssi_range_window(int mode, int *lo, int *hi) {
    int first, last;
    if (mode == RSSI_RANGE_LOWBAND) {
        first = 40;
        last = 47;
    } else if (mode == RSSI_RANGE_STANDARD) {
        first = 16;
        last = 23;
    } else {
        *lo = 0;
        *hi = 0xFFFF;
        return;
    }
    *lo = *hi = scan_analog_idx_to_mhz[first];
    for (int i = first; i <= last; i++) {
        if (scan_analog_idx_to_mhz[i] < *lo)
            *lo = scan_analog_idx_to_mhz[i];
        if (scan_analog_idx_to_mhz[i] > *hi)
            *hi = scan_analog_idx_to_mhz[i];
    }
}

// Builds the sweep order for the current width, ascending by frequency so the
// chart's X axis is a real frequency sweep.
static void build_rssi_channel_list(void) {
    int n = 0;

    if (rssi_step_mode != RSSI_STEP_CHANNELS) {
        // Free sweep over what the receiver actually tunes: Lowband = 5361..5621, Standard = E1..E8 =
        // 5645..5945, Full = both ends.
        int lo = 5361, hi = 5945;
        if (rssi_range_mode == RSSI_RANGE_LOWBAND)
            hi = 5621;
        else if (rssi_range_mode == RSSI_RANGE_STANDARD)
            lo = 5645;
        int step = rssi_step_mhz[rssi_step_mode];
        for (int f = lo; f <= hi && n < RSSI_SCAN_CH_MAX; f += step, n++) {
            rssi_channels[n].freq_mhz = (uint16_t)f;
            rssi_channels[n].analog_idx = -1;
        }
        if (rssi_channels[n - 1].freq_mhz != hi && n < RSSI_SCAN_CH_MAX) {
            rssi_channels[n].freq_mhz = (uint16_t)hi;
            rssi_channels[n].analog_idx = -1;
            n++;
        }
        rssi_ch_count = n;
        return;
    }

    int win_lo, win_hi;
    rssi_range_window(rssi_range_mode, &win_lo, &win_hi);
    // scan_freq_table is already strictly ascending by frequency; skip the
    // HDZero-only rows, which the analog receiver has no channel for.
    for (size_t i = 0; i < scan_freq_table_len && n < RSSI_SCAN_CH_MAX; i++) {
        if (scan_freq_table[i].analog_channel < 0 ||
            scan_freq_table[i].freq_mhz < win_lo || scan_freq_table[i].freq_mhz > win_hi)
            continue;
        rssi_channels[n].freq_mhz = scan_freq_table[i].freq_mhz;
        rssi_channels[n].analog_idx = scan_freq_table[i].analog_channel;
        n++;
    }
    rssi_ch_count = n;
}

// (Re)configure the chart for the current sweep width: point count, X range,
// corner frequency labels and which band markers are visible and where.
static void apply_rssi_range(void) {
    build_rssi_channel_list();
    if (rssi_ch_count < 2)
        return;

    int f_min = rssi_channels[0].freq_mhz;
    int f_max = rssi_channels[rssi_ch_count - 1].freq_mhz;
    lv_chart_set_point_count(rssi_chart, rssi_ch_count);
    lv_chart_set_range(rssi_chart, LV_CHART_AXIS_PRIMARY_X, f_min, f_max);

    char buf[24];
    snprintf(buf, sizeof(buf), "%d MHz", f_min);
    lv_label_set_text(rssi_lo_lbl, buf);
    lv_obj_align_to(rssi_lo_lbl, rssi_chart, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    snprintf(buf, sizeof(buf), "%d MHz", f_max);
    lv_label_set_text(rssi_hi_lbl, buf);
    lv_obj_align_to(rssi_hi_lbl, rssi_chart, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 8);

    for (int k = 0; k < 48; k++) {
        int f = scan_analog_idx_to_mhz[k];
        if (f < f_min || f > f_max) {
            lv_obj_add_flag(band_lbl[k], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(band_lbl[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(band_lbl[k]);
        lv_obj_align_to(band_lbl[k], rssi_chart, LV_ALIGN_OUT_BOTTOM_LEFT,
                        rssi_plot_left + rssi_plot_w * (f - f_min) / (f_max - f_min) -
                            lv_obj_get_width(band_lbl[k]) / 2,
                        40 + (k / 8) * 27);
    }
}

static void start_rssi_scan(void) {
    apply_rssi_range();
#if defined(HDZGOGGLE2)
    // The Expansion module shares the analog input; scan on the Built-in
    // receiver for the duration (in memory only, same as Scan Now).
    if (g_setting.source.analog_module == SETTING_SOURCES_ANALOG_MODULE_EXTERNAL) {
        rssi_borrowed_internal = true;
        g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_INTERNAL;
        Analog_Module_Power(1);
    }
#endif
    rtc6715.init(1, 0);
    scan_core_notify_analog_powered_on();

    rssi_lo_mv = RSSI_MV_BASE_MIN;
    rssi_hi_mv = RSSI_MV_BASE_MAX;
    for (int i = 0; i < RSSI_SCAN_CH_MAX; i++)
        rssi_mv[i] = -1;
    update_rssi_ticks();
    for (int i = 0; i < rssi_ch_count; i++)
        set_rssi_point(rssi_series, i, LV_CHART_POINT_NONE);
    rssi_scan_idx = 0;
    rssi_scan_active = true;

    lv_obj_move_foreground(rssi_cont);
    lv_obj_clear_flag(rssi_cont, LV_OBJ_FLAG_HIDDEN);
    lvgl_screen_orbit(false);
}

static void stop_rssi_scan(void) {
    if (!rssi_scan_active)
        return;
    rssi_scan_active = false;
    lv_obj_add_flag(rssi_cont, LV_OBJ_FLAG_HIDDEN);
    lvgl_screen_orbit(g_setting.osd.orbit > 0);
    rtc6715.init(0, 0);
    scan_core_notify_analog_powered_off();
#if defined(HDZGOGGLE2)
    if (rssi_borrowed_internal) {
        rssi_borrowed_internal = false;
        g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_EXTERNAL;
        Analog_Module_Power(1);
    }
#endif
}

// One frequency per tick; scan_probe_analog() blocks for the PLL to lock,
// which paces the sweep.
static void rssi_scan_tick(void) {
    if (!rssi_scan_active)
        return;

    const rssi_scan_ch_t *c = &rssi_channels[rssi_scan_idx];
    uint16_t mv = 0;
    bool valid = false;
    if (c->analog_idx >= 0)
        scan_probe_analog((uint8_t)c->analog_idx, &mv, &valid);
    else
        valid = scan_probe_analog_freq(c->freq_mhz, RSSI_FINE_SETTLE_US, &mv);

    // Relative scale over the auto range; there is no dBm calibration in this
    // firmware. Widen the range when a reading falls outside it and re-plot
    // everything measured so far against the new axis.
    bool widened = false;
    if ((int)mv > rssi_hi_mv) {
        rssi_hi_mv = mv;
        widened = true;
    }
    if ((int)mv < rssi_lo_mv) {
        rssi_lo_mv = mv;
        widened = true;
    }
    rssi_mv[rssi_scan_idx] = mv;
    for (int i = 0; i < rssi_ch_count && widened; i++) {
        if (rssi_mv[i] >= 0)
            set_rssi_point(rssi_series, i, (rssi_mv[i] - rssi_lo_mv) * 100 / (rssi_hi_mv - rssi_lo_mv));
    }
    if (widened)
        update_rssi_ticks();
    else
        set_rssi_point(rssi_series, rssi_scan_idx, ((int)mv - rssi_lo_mv) * 100 / (rssi_hi_mv - rssi_lo_mv));

    char buf[48];
    snprintf(buf, sizeof(buf), "%u MHz  %u mV", c->freq_mhz, mv);
    lv_label_set_text(rssi_status_label, buf);

    rssi_scan_idx = (rssi_scan_idx + 1) % rssi_ch_count;
}
#endif // RSSI_SCAN_ANALOG

static void refresh_temp_label(void) {
    char line[96];
    // nct_read_temperature() returns -1 when a sensor can't be read.
    char t[3][16];
    // thread.c stores left as (reading + 100), so a failed read (-1) shows up as 99.
    int v[3] = {g_temperature.top, g_temperature.left == 99 ? -1 : g_temperature.left,
                g_temperature.right};
    for (int i = 0; i < 3; i++) {
        if (v[i] < 0)
            snprintf(t[i], sizeof(t[i]), "--");
        else
            snprintf(t[i], sizeof(t[i]), "%d.%d", v[i] / 10, v[i] % 10);
    }
#if defined(HDZBOXPRO)
    // BoxPro has a single NCT75, stored in g_temperature.top.
    snprintf(line, sizeof(line), "%s: %s C", _lang("Temperature"), t[0]);
#else
    snprintf(line, sizeof(line), "%s:  %s %s   %s %s   %s %s C", _lang("Temperature"),
             _lang("Top"), t[0], _lang("Left"), t[1], _lang("Right"), t[2]);
#endif
    if (strcmp(line, temp_text) != 0) {
        snprintf(temp_text, sizeof(temp_text), "%s", line);
        lv_label_set_text(temp_label, temp_text);
    }
}

// Runs from main_menu_update()'s per-page tick, which fires for every page
// whether or not it is open: everything here must be cheap when idle.
static void share_stop(void);
static void share_window_text(bool starting);

static void page_tools_on_update(uint32_t delta_ms) {
    (void)delta_ms;
    // The share ends as soon as the menu is left for the video.
    if (wifi_share_active() && !main_menu_is_shown())
        share_stop();
    static uint32_t share_acc;
    share_acc += delta_ms;
    if (wifi_share_active() && share_acc >= 1000) {
        share_acc = 0;
        share_window_text(false);
    }
    refresh_temp_label();
#ifdef RSSI_SCAN_ANALOG
    rssi_scan_tick();
#endif
}

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

static lv_obj_t *note_label;

// WiFi Share window: while it is up the hotspot and the web portal run.
static lv_obj_t *share_cont;
static lv_obj_t *share_label;

// Where the portal's conversion CGI keeps the running job (mkapp/app/portal/www/cgi-bin/convert.cgi)
#define PORTAL_CONV_DIR "/tmp/portal_conv/lock"

// "nom.ts: 45 %" while the portal is converting a clip, "" otherwise.
static void share_conversion_text(char *out, size_t size) {
    out[0] = '\0';
    FILE *f = fopen(PORTAL_CONV_DIR "/name", "r");
    if (!f)
        return;
    char name[96] = "";
    if (!fgets(name, sizeof(name), f))
        name[0] = '\0';
    fclose(f);
    name[strcspn(name, "\r\n")] = '\0';
    if (!name[0])
        return;
    int pct = 0;
    f = fopen(PORTAL_CONV_DIR "/progress", "r");
    if (f) {
        if (fscanf(f, "%d", &pct) != 1)
            pct = 0;
        fclose(f);
    }
    // the CGIs key a job "light_" + ("fav_" for a favourite) + NAME
    const char *shown = name;
    const char *what = "";
    if (strncmp(shown, "light_", 6) == 0) {
        shown += 6;
        what = "light copy ";
    }
    if (strncmp(shown, "fav_", 4) == 0)
        shown += 4;
    snprintf(out, size, "%s%s: %d %%", what, shown, pct);
}

// The share window: how to connect, then the goggle's own state while it shares.
static void share_window_text(bool starting) {
    char buf[480];
    if (starting) {
        snprintf(buf, sizeof(buf), "%s...", _lang("Starting the WiFi share"));
        lv_label_set_text(share_label, buf);
        return;
    }

    char bat[24] = "";
    battery_get_voltage_str(bat);

    char t[3][16];
    int v[3] = {g_temperature.top, g_temperature.left == 99 ? -1 : g_temperature.left, g_temperature.right};
    for (int k = 0; k < 3; k++) {
        if (v[k] < 0)
            snprintf(t[k], sizeof(t[k]), "--");
        else
            snprintf(t[k], sizeof(t[k]), "%d.%d", v[k] / 10, v[k] % 10);
    }

    char conv[128];
    share_conversion_text(conv, sizeof(conv));
    power_save_fans(conv[0] == '\0');          // a long job heats the chip: the fans run normally until it is over

    int n = snprintf(buf, sizeof(buf), "%s: %s\n%s: %s\n%s: http://%s\n\n%s: %s\n",
                     _lang("Network"), g_setting.wifi.ssid[0],
                     _lang("Password"), g_setting.wifi.passwd[0],
                     _lang("Open"), g_setting.wifi.ip_addr,
                     _lang("Battery"), bat);
#if defined(HDZBOXPRO)
    n += snprintf(buf + n, sizeof(buf) - n, "%s: %s C\n", _lang("Temperature"), t[0]);
#else
    n += snprintf(buf + n, sizeof(buf) - n, "%s: %s %s   %s %s   %s %s C\n", _lang("Temperature"),
                  _lang("Top"), t[0], _lang("Left"), t[1], _lang("Right"), t[2]);
#endif
    snprintf(buf + n, sizeof(buf) - n, "%s: %s", _lang("Conversion"), conv[0] ? conv : _lang("none"));

    if (strcmp(lv_label_get_text(share_label), buf) != 0)
        lv_label_set_text(share_label, buf);
}

static void share_start(void) {
    lv_obj_move_foreground(share_cont);
    lv_obj_clear_flag(share_cont, LV_OBJ_FLAG_HIDDEN);
    lvgl_screen_orbit(false);
    share_window_text(true);
    lv_refr_now(NULL); // draw "starting" first: bringing the hotspot up takes a few seconds
    wifi_share_start();
    power_save_enter();
    share_window_text(false);
}

static void share_stop(void) {
    if (!wifi_share_active() && lv_obj_has_flag(share_cont, LV_OBJ_FLAG_HIDDEN))
        return;
    lv_obj_add_flag(share_cont, LV_OBJ_FLAG_HIDDEN);
    lvgl_screen_orbit(g_setting.osd.orbit > 0);
    power_save_exit();
    wifi_share_stop();
}

// One short note for the selected row only, instead of every row's note at
// once, to leave room on the page.
static void update_note(int sel) {
    const char *text = "";

    switch (sel) {
    case ROW_FOCUS_CHART:
    case ROW_FREQ_CHART:
        text = "Click to display it fullscreen, click again to dismiss.";
        break;
#ifdef RSSI_SCAN_ANALOG
    case ROW_RSSI_SCAN:
        text = "Sweeps the analog channels with the built-in receiver and charts the signal strength. Click again to stop.";
        break;
    case ROW_RSSI_RANGE:
        text = "Frequency range swept: Full, Lowband or Standard (E1 to E8).";
        break;
    case ROW_RSSI_STEP:
        text = "Channels: one point per channel. Coarse 4 MHz and Fine 2 MHz sweep every frequency, slower.";
        break;
    case ROW_SCAN_PAGE:
        text = "What the Scan entry does: its normal scan, or open the RSSI Scanner instead.";
        break;
#endif
#ifndef HDZBOXPRO
    case ROW_THEME:
        text = "Click to switch theme. Restart the goggles to apply the new theme.";
        break;
#endif
    case ROW_WIFI_SHARE:
        text = "Starts the WiFi hotspot and the web portal (live view, videos). Ending the share switches the WiFi off.";
        break;
    default:
        break;
    }
    if (note_label)
        lv_label_set_text(note_label, _lang(text));
}

static void page_tools_enter(void) {
    update_note(pp_tools.p_arr.cur);
}

static void page_tools_on_roller(uint8_t key) {
    update_note(pp_tools.p_arr.cur);
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

#ifdef RSSI_SCAN_ANALOG
    snprintf(buf, sizeof(buf), "%s", _lang("RSSI Scanner"));
    create_label_item(cont, buf, 1, ROW_RSSI_SCAN, 5);

    rssi_range_mode = (int)ini_getl("tools", "rssi_range", RSSI_RANGE_FULL, SETTING_INI);
    if (rssi_range_mode < 0 || rssi_range_mode >= RSSI_RANGE_COUNT)
        rssi_range_mode = RSSI_RANGE_FULL;
    create_label_item(cont, _lang("RSSI Scan Width"), 1, ROW_RSSI_RANGE, 2);
    rssi_range_label = create_label_item(cont, "", 3, ROW_RSSI_RANGE, 3);
    snprintf(buf, sizeof(buf), "< %s >", _lang(rssi_range_name[rssi_range_mode]));
    lv_label_set_text(rssi_range_label, buf);

    scan_page_rssi = ini_getl("tools", "scan_page", 0, SETTING_INI) != 0;
    create_label_item(cont, _lang("Scan Page"), 1, ROW_SCAN_PAGE, 2);
    scan_page_label = create_label_item(cont, "", 3, ROW_SCAN_PAGE, 3);
    snprintf(buf, sizeof(buf), "< %s >", scan_page_rssi ? _lang("RSSI Scanner") : _lang("Scan"));
    lv_label_set_text(scan_page_label, buf);

    rssi_step_mode = (int)ini_getl("tools", "rssi_step", RSSI_STEP_CHANNELS, SETTING_INI);
    if (rssi_step_mode < 0 || rssi_step_mode >= RSSI_STEP_COUNT)
        rssi_step_mode = RSSI_STEP_CHANNELS;
    create_label_item(cont, _lang("RSSI Scan Step"), 1, ROW_RSSI_STEP, 2);
    rssi_step_label = create_label_item(cont, "", 3, ROW_RSSI_STEP, 3);
    snprintf(buf, sizeof(buf), "< %s >", _lang(rssi_step_name[rssi_step_mode]));
    lv_label_set_text(rssi_step_label, buf);
#endif

    // WiFi Share: hotspot + web portal for the time of the share (needs the WiFi module).
    create_label_item(cont, _lang("WiFi Share"), 1, ROW_WIFI_SHARE, 5);
    if (!wifi_share_available()) {
        lv_obj_clear_flag(pp_tools.p_arr.panel[ROW_WIFI_SHARE], FLAG_SELECTABLE);
        lv_obj_add_flag(pp_tools.p_arr.panel[ROW_WIFI_SHARE], LV_OBJ_FLAG_HIDDEN);
    }

    // Info row, not an action: shows the goggle's temperature probes live.
    lv_obj_clear_flag(pp_tools.p_arr.panel[ROW_TEMPERATURE], FLAG_SELECTABLE);
    temp_label = create_label_item(cont, "", 1, ROW_TEMPERATURE, 5);
    temp_text[0] = '\0';
    refresh_temp_label();

    // submenu_click() treats the last row as "Back" and leaves the page.
    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 1);

    lv_obj_t *note = lv_label_create(cont);
    int note_row = TOOLS_ROW_COUNT;
#ifndef HDZBOXPRO
    create_label_item(cont, _lang("Theme"), 1, ROW_THEME, 1);
    label_name = create_label_item(cont, "", 2, ROW_THEME, 3);

    // The picked theme's colours as round dots, to the right of its name.
    lv_obj_t *dots = lv_obj_create(cont);
    lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dots, 4 * (SWATCH_DOT + SWATCH_GAP), SWATCH_DOT + 4);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 0, 0);
    lv_obj_set_layout(dots, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, SWATCH_GAP, 0);
    lv_obj_set_grid_cell(dots, LV_GRID_ALIGN_START, 4, 2, LV_GRID_ALIGN_CENTER, ROW_THEME, 1);
    for (int i = 0; i < SWATCH_COUNT; i++) {
        swatch[i] = lv_obj_create(dots);
        lv_obj_clear_flag(swatch[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(swatch[i], SWATCH_DOT, SWATCH_DOT);
        lv_obj_set_style_radius(swatch[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(swatch[i], 1, 0);
        lv_obj_set_style_border_color(swatch[i], lv_color_hex(0x808080), 0);
    }

    preview_idx = g_setting.ui_theme;
    show_preview(preview_idx);
#endif

    // Only the note of the selected row is shown (see update_note()).
    note_label = note;
    lv_label_set_text(note, ""); // a new label reads "Text" by default
    lv_obj_set_style_text_font(note, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(note, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(note, LV_GRID_ALIGN_STRETCH, 1, 5, LV_GRID_ALIGN_START, note_row, 2); // stretch: lets the long notes wrap

    share_cont = lv_obj_create(lv_scr_act());
    lv_obj_add_flag(share_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(share_cont, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(share_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(share_cont, 0, 0);
    lv_obj_set_size(share_cont, DRAW_HOR_RES_FHD, DRAW_VER_RES_FHD);
    lv_obj_set_style_bg_color(share_cont, lv_color_hex(UI_COLOR_BG_ROOT), 0);
    lv_obj_set_style_bg_opa(share_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(share_cont, 0, 0);
    lv_obj_set_style_radius(share_cont, 0, 0);

    lv_obj_t *share_title = lv_label_create(share_cont);
    lv_label_set_text(share_title, _lang("WiFi sharing in progress"));
    lv_obj_set_style_text_font(share_title, UI_PAGE_TEXT_FONT, 0);
    lv_obj_set_style_text_color(share_title, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_align(share_title, LV_ALIGN_CENTER, 0, -210);

    share_label = lv_label_create(share_cont);
    lv_label_set_text(share_label, "");
    lv_obj_set_style_text_font(share_label, UI_PAGE_TEXT_FONT, 0);
    lv_obj_set_style_text_color(share_label, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_text_align(share_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(share_label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *share_btn = lv_label_create(share_cont);
    lv_label_set_text(share_btn, _lang("Click to end the sharing"));
    lv_obj_set_style_text_font(share_btn, UI_PAGE_TEXT_FONT, 0);
    lv_obj_set_style_text_color(share_btn, lv_color_hex(UI_COLOR_BG_ROOT), 0);
    lv_obj_set_style_bg_color(share_btn, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(share_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(share_btn, 30, 0);
    lv_obj_set_style_pad_hor(share_btn, 40, 0);
    lv_obj_set_style_pad_ver(share_btn, 14, 0);
    lv_obj_align(share_btn, LV_ALIGN_CENTER, 0, 190);

    chart_img = lv_img_create(lv_scr_act());
    lv_obj_add_flag(chart_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chart_img, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(chart_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(chart_img, 0, 0);
    lv_obj_set_size(chart_img, DRAW_HOR_RES_FHD, DRAW_VER_RES_FHD);
    chart_open = false;

#ifdef RSSI_SCAN_ANALOG
    build_rssi_channel_list();

    rssi_cont = lv_obj_create(lv_scr_act());
    lv_obj_add_flag(rssi_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(rssi_cont, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(rssi_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rssi_cont, 0, 0);
    lv_obj_set_size(rssi_cont, DRAW_HOR_RES_FHD, DRAW_VER_RES_FHD);
    lv_obj_set_style_bg_color(rssi_cont, lv_color_hex(UI_COLOR_BG_ROOT), 0);
    lv_obj_set_style_bg_opa(rssi_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rssi_cont, 0, 0);
    lv_obj_set_style_radius(rssi_cont, 0, 0);

    lv_obj_t *rssi_title = lv_label_create(rssi_cont);
    lv_label_set_text(rssi_title, _lang("RSSI Scanner"));
    lv_obj_set_style_text_font(rssi_title, UI_PAGE_TEXT_FONT, 0);
    lv_obj_set_style_text_color(rssi_title, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_align(rssi_title, LV_ALIGN_TOP_MID, 0, 20);

    rssi_status_label = lv_label_create(rssi_cont);
    lv_label_set_text(rssi_status_label, "");
    lv_obj_set_style_text_font(rssi_status_label, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_color(rssi_status_label, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_align(rssi_status_label, LV_ALIGN_TOP_MID, 0, 60);

    rssi_chart = lv_chart_create(rssi_cont);
    lv_chart_set_type(rssi_chart, LV_CHART_TYPE_SCATTER);
    lv_chart_set_point_count(rssi_chart, rssi_ch_count);
    lv_chart_set_range(rssi_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_range(rssi_chart, LV_CHART_AXIS_PRIMARY_X, rssi_channels[0].freq_mhz,
                       rssi_channels[rssi_ch_count - 1].freq_mhz);
    lv_chart_set_div_line_count(rssi_chart, 3, 7); // horizontal lines at 25/50/75
    lv_obj_set_size(rssi_chart, lv_pct(85), lv_pct(50)); // leaves room for the band rows below
    lv_obj_align(rssi_chart, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_size(rssi_chart, 5, LV_PART_INDICATOR); // a dot per measured frequency
    // The chart defaults to a light theme background -- match the page's dark
    // theme instead of leaving a white plot area behind the red trace.
    lv_obj_set_style_bg_color(rssi_chart, lv_color_hex(UI_COLOR_BG_PANEL), 0);
    lv_obj_set_style_bg_opa(rssi_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(rssi_chart, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_line_color(rssi_chart, lv_color_hex(0x404040), LV_PART_MAIN);
    // Keep a margin inside the plot so a value of 0 isn't hidden on the border.
    lv_obj_set_style_pad_all(rssi_chart, 8, LV_PART_MAIN);
    rssi_series = lv_chart_add_series(rssi_chart, lv_color_hex(UI_COLOR_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_line_width(rssi_chart, 3, LV_PART_ITEMS);

    lv_obj_update_layout(rssi_chart);
    rssi_plot_h = lv_obj_get_content_height(rssi_chart);
    rssi_plot_top = lv_obj_get_style_pad_top(rssi_chart, LV_PART_MAIN) +
                    lv_obj_get_style_border_width(rssi_chart, LV_PART_MAIN);
    for (int i = 0; i < 5; i++) {
        rssi_tick[i] = lv_label_create(rssi_cont);
        lv_obj_set_style_text_font(rssi_tick[i], UI_PAGE_LABEL_FONT, 0);
        lv_obj_set_style_text_color(rssi_tick[i], lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    }
    update_rssi_ticks();

    rssi_lo_lbl = lv_label_create(rssi_cont);
    lv_obj_set_style_text_font(rssi_lo_lbl, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_color(rssi_lo_lbl, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    rssi_hi_lbl = lv_label_create(rssi_cont);
    lv_obj_set_style_text_font(rssi_hi_lbl, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_color(rssi_hi_lbl, lv_color_hex(TEXT_COLOR_DEFAULT), 0);

    // Band markers, one row per band under the frequency labels, each in its
    // own colour (same hues as the reference frequency chart). All 48 are
    // created once; apply_rssi_range() shows the ones inside the current
    // sweep width at their channel's real frequency.
    static const char band_letter[6] = {'A', 'B', 'E', 'F', 'R', 'L'};
    static const uint32_t band_color[6] = {0x40E0D0, 0xB5E61D, 0xFFA040, 0xA070FF, 0xFF5C7A, 0x6EA8FF};
    rssi_plot_w = lv_obj_get_content_width(rssi_chart);
    rssi_plot_left = lv_obj_get_style_pad_left(rssi_chart, LV_PART_MAIN) +
                     lv_obj_get_style_border_width(rssi_chart, LV_PART_MAIN);
    for (int k = 0; k < 48; k++) {
        char name[4];
        snprintf(name, sizeof(name), "%c%d", band_letter[k / 8], k % 8 + 1);
        band_lbl[k] = lv_label_create(rssi_cont);
        lv_label_set_text(band_lbl[k], name);
        lv_obj_set_style_text_font(band_lbl[k], UI_PAGE_LABEL_FONT, 0);
        lv_obj_set_style_text_color(band_lbl[k], lv_color_hex(band_color[k / 8]), 0);
    }
    apply_rssi_range();

    rssi_scan_active = false;
#endif


    return page;
}

// Scan page hooks: with "Scan Page" set to RSSI Scanner, the Scan sidebar
// entry opens the scanner overlay instead of running a scan.
bool tools_scan_page_is_rssi(void) {
#ifdef RSSI_SCAN_ANALOG
    return scan_page_rssi;
#else
    return false;
#endif
}

bool tools_rssi_scan_active(void) {
#ifdef RSSI_SCAN_ANALOG
    return rssi_scan_active;
#else
    return false;
#endif
}

void tools_rssi_scan_open(void) {
#ifdef RSSI_SCAN_ANALOG
    if (!rssi_scan_active)
        start_rssi_scan();
#endif
}

void tools_rssi_scan_close(void) {
#ifdef RSSI_SCAN_ANALOG
    stop_rssi_scan();
#endif
}

static void page_tools_exit(void) {
    hide_chart();
    share_stop();
    update_note(-1);
#ifdef RSSI_SCAN_ANALOG
    stop_rssi_scan();
#endif
}

static void page_tools_on_click(uint8_t key, int sel) {
    if (wifi_share_active()) {
        share_stop();
        return;
    }
    if (chart_open) {
        // Any click while a chart overlay is up just dismisses it back to
        // the row list, rather than leaving the whole Tools page.
        hide_chart();
        return;
    }
#ifdef RSSI_SCAN_ANALOG
    if (rssi_scan_active) {
        stop_rssi_scan();
        return;
    }
#endif

    switch (sel) {
    case ROW_FOCUS_CHART:
        show_chart(true);
        break;
    case ROW_FREQ_CHART:
        show_chart(false);
        break;
    case ROW_WIFI_SHARE:
        if (wifi_share_available())
            share_start();
        break;
#ifdef RSSI_SCAN_ANALOG
    case ROW_RSSI_SCAN:
        start_rssi_scan();
        break;
    case ROW_RSSI_STEP: {
        char buf[32];
        rssi_step_mode = (rssi_step_mode + 1) % RSSI_STEP_COUNT;
        ini_putl("tools", "rssi_step", rssi_step_mode, SETTING_INI);
        snprintf(buf, sizeof(buf), "< %s >", _lang(rssi_step_name[rssi_step_mode]));
        lv_label_set_text(rssi_step_label, buf);
        break;
    }
    case ROW_SCAN_PAGE: {
        char buf[32];
        scan_page_rssi = !scan_page_rssi;
        ini_putl("tools", "scan_page", scan_page_rssi, SETTING_INI);
        snprintf(buf, sizeof(buf), "< %s >", scan_page_rssi ? _lang("RSSI Scanner") : _lang("Scan"));
        lv_label_set_text(scan_page_label, buf);
        page_scannow_apply_page_mode();
        break;
    }
    case ROW_RSSI_RANGE: {
        char buf[32];
        rssi_range_mode = (rssi_range_mode + 1) % RSSI_RANGE_COUNT;
        ini_putl("tools", "rssi_range", rssi_range_mode, SETTING_INI);
        snprintf(buf, sizeof(buf), "< %s >", _lang(rssi_range_name[rssi_range_mode]));
        lv_label_set_text(rssi_range_label, buf);
        break;
    }
#endif
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
    .enter = page_tools_enter,
    .exit = page_tools_exit,
    .on_created = NULL,
    .on_update = page_tools_on_update,
    .on_roller = page_tools_on_roller,
    .on_click = page_tools_on_click,
    .on_right_button = NULL,
};
