#include "ui/ui_theme.h"

#include <stddef.h>

// The status bar icons carry a baked-in 0x131313 background, so the status
// bar itself is not themed (see UI_COLOR_BG_STATUSBAR in conf/ui.h).
#define BLACK_THEME(n, txt, dis, acc, tab_c, wbg, foc, idle, barbg, barfg, card, ddtxt, selc) \
    {                                                                                   \
        .name = n, .bg_root = 0x000000, .bg_panel = 0x000000, .accent = acc,            \
        .tab = acc, .border_idle = 0x2A2A2A, .widget_bg = wbg, .focus = foc,          \
        .idle_btn = idle, .bar_bg = barbg, .bar_fg = barfg, .select_bg = card,          \
        .dd_bg = wbg, .dd_text = ddtxt, .kb_bg = 0x000000, .kb_text = txt,              \
        .field_bg = wbg, .field_text = txt,                                             \
        .sel = selc, .text = txt, .text_disable = dis                                    \
    }

static const ui_theme_t themes[] = {
    // 0: the firmware's original look
    {
        .name = "Original", .bg_root = 0x131313, .bg_panel = 0x202020, .accent = 0xF44336,
        .tab = 0xFFFFFF, .border_idle = 0x606060, .widget_bg = 0x404040, .focus = 0x00A000,
        .idle_btn = 0x404040, .bar_bg = 0x008000, .bar_fg = 0x00FF00, .select_bg = 0x646464,
        .dd_bg = 0x646464, .dd_text = 0xFFFFFF, .kb_bg = 0x646464, .kb_text = 0x000000,
        .field_bg = 0, .field_text = 0, .sel = 0,
        .text = 0xFFFFFF, .text_disable = 0x808080,
    },
    //            name              text      disabled  accent    tab       widget    focus     idle      bar bg    bar fg    card      dropdown text
    BLACK_THEME("Braise",         0xFFC800, 0x6B5A10, 0xFF2A2A, 0xFF2A2A, 0x141414, 0xB01E1E, 0x1E1E1E, 0x5A4A00, 0xFFC800, 0x3A0C0C, 0xFFC800, 0xFFC800),
    BLACK_THEME("Ambre",          0xFFB347, 0x6B4A20, 0xFF6A00, 0xFF6A00, 0x140F08, 0xB84A00, 0x241A0E, 0x5A3A10, 0xFFB347, 0x3A1C08, 0xFFB347, 0xFFB347),
    BLACK_THEME("Glace",          0xE6FBFF, 0x4C6A73, 0x00E5FF, 0x00A6B8, 0x0D1418, 0x0097A7, 0x16303B, 0x0A4A55, 0x00E5FF, 0x0A3A46, 0xE6FBFF, 0x00E5FF),
    BLACK_THEME("Ultraviolet",    0xECE6FF, 0x5D5478, 0x8B5CFF, 0x6A40C8, 0x130F1C, 0x6A40C8, 0x211A33, 0x3B2A70, 0xC9B6FF, 0x241A40, 0xECE6FF, 0xC9B6FF),
    BLACK_THEME("Radar",          0xD2FFE1, 0x40694D, 0x39FF88, 0x1FAF5C, 0x0C150F, 0x1FAF5C, 0x14281B, 0x115C31, 0x39FF88, 0x0C3A20, 0xD2FFE1, 0x39FF88),
    BLACK_THEME("Magenta",        0xFFE3F1, 0x7A4A63, 0xFF2E93, 0xC01F6E, 0x180D13, 0xC01F6E, 0x2A1522, 0x6E1846, 0xFF2E93, 0x3A0C24, 0xFFE3F1, 0xFF2E93),
    BLACK_THEME("Rouge pur",      0xFF4D4D, 0x6B2020, 0xFF3B3B, 0xB02020, 0x140808, 0xB02020, 0x241010, 0x5A1414, 0xFF4D4D, 0x3A0C0C, 0xFF4D4D, 0xFF4D4D),
    BLACK_THEME("Bordeaux & or",  0xF3D9A4, 0x6E5C3C, 0xB3122E, 0xB3122E, 0x150A0C, 0x8A0E24, 0x261418, 0x5C4A1E, 0xE8B84A, 0x3A0C14, 0xF3D9A4, 0xE8B84A),
    BLACK_THEME("Citron",         0xF4FF9E, 0x5F6A24, 0xD4FF00, 0x8FAA00, 0x111406, 0x8FAA00, 0x1E240A, 0x475500, 0xD4FF00, 0x2A3300, 0xF4FF9E, 0xD4FF00),
    BLACK_THEME("Bleu electrique",0xDCE8FF, 0x435580, 0x2F6BFF, 0x2350C8, 0x0B1020, 0x2350C8, 0x141C36, 0x1A3A8A, 0x6E9BFF, 0x0F1E48, 0xDCE8FF, 0x6E9BFF),
    BLACK_THEME("Sarcelle",       0xD6FFF6, 0x3D6B62, 0x00C9A7, 0x009880, 0x0A1614, 0x009880, 0x12302B, 0x0A5A4C, 0x00C9A7, 0x083A32, 0xD6FFF6, 0x00C9A7),
    BLACK_THEME("Monochrome",     0xEDEDED, 0x5A5A5A, 0xFFFFFF, 0x808080, 0x111111, 0x606060, 0x1E1E1E, 0x404040, 0xFFFFFF, 0x2A2A2A, 0xEDEDED, 0xFFFFFF),
};

#define THEME_COUNT ((int)(sizeof(themes) / sizeof(themes[0])))

const ui_theme_t *g_ui_theme = &themes[UI_THEME_DEFAULT];

int ui_theme_count(void) {
    return THEME_COUNT;
}

const ui_theme_t *ui_theme_get(int index) {
    if (index < 0 || index >= THEME_COUNT)
        index = UI_THEME_DEFAULT;
    return &themes[index];
}

void ui_theme_apply(int index) {
    g_ui_theme = ui_theme_get(index);
}

uint32_t ui_theme_ink(uint32_t bg) {
    uint32_t r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    return (r * 299 + g * 587 + b * 114) / 1000 > 100 ? 0x000000 : 0xFFFFFF;
}

int ui_theme_pills(void) {
    return g_ui_theme->sel != 0;
}
