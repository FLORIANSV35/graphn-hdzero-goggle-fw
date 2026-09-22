#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Runtime menu colour theme. Colours are read while the UI is being built, so
// a change made in the Theme page is stored and takes effect on next boot.
typedef struct {
    const char *name;
    uint32_t bg_root;
    uint32_t bg_panel;
    uint32_t accent;      // frames, focus outlines, selection
    uint32_t tab;         // selected main-menu entry
    uint32_t border_idle; // unselected item outline
    uint32_t widget_bg;   // idle keys / buttons
    uint32_t focus;       // focused mode button
    uint32_t idle_btn;    // idle mode button
    uint32_t bar_bg;      // progress bar track
    uint32_t bar_fg;      // progress bar fill
    uint32_t select_bg;   // selected row background
    uint32_t dd_bg;       // dropdown background
    uint32_t dd_text;     // dropdown text
    uint32_t kb_bg;       // keyboard background
    uint32_t kb_text;     // keyboard text
    uint32_t field_bg;    // closed dropdown field (0 = keep the stock LVGL look)
    uint32_t field_text;  // closed dropdown field text
    uint32_t sel;         // chosen segment fill (0 = original look, no pills)
    uint32_t text;
    uint32_t text_disable;
} ui_theme_t;

#define UI_THEME_DEFAULT 1 // "Braise"

extern const ui_theme_t *g_ui_theme;

// Readable text colour (black or white) on the given background.
uint32_t ui_theme_ink(uint32_t bg);
// Pill / card look is used by every theme except "Original".
int ui_theme_pills(void);

int ui_theme_count(void);
const ui_theme_t *ui_theme_get(int index);
// Select the active theme (out-of-range falls back to the default).
void ui_theme_apply(int index);

#ifdef __cplusplus
}
#endif
