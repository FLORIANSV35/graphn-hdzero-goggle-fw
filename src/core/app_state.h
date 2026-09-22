#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "ui/page_common.h"

typedef enum {
    APP_STATE_MAINMENU = 0,
    APP_STATE_SUBMENU = 1,
    APP_STATE_PLAYBACK = 2,

    // in this state, the menu pages' on_roller is called,
    // but the selected submenu item selection (e.g. pp_osd.p_arr.cur)
    // is not automatically changed.
    APP_STATE_SUBMENU_ITEM_FOCUSED = 3,

    APP_STATE_VIDEO = 10,

    // the preview for image settings
    APP_STATE_IMS = 11,
    // the preview for osd element positioning settings
    APP_STATE_OSD_ELEMENT_PREV = 12,

    // WiFi configuration page
    APP_STATE_WIFI = 13,

    APP_STATE_USER_INPUT_DISABLED = 20,

    APP_STATE_SLEEP = 30,
} app_state_t;

extern app_state_t g_app_state;

void app_state_push(app_state_t state);

void app_switch_to_menu();
// Same UI-side transition as app_switch_to_menu() (hides the OSD/video-hole
// layer, switches the display plane to UI, shows the menu) but skips
// HDZero_Close/rtc6715 reset/etc, which would otherwise tear down a source
// that's already running. Used by Startup="Menu", which never enters video
// in the first place (the single physical display plane can't show video
// and menu at once, so loading a channel here would force a visible flash
// before the menu could appear) -- app_exit_menu() loads the picked source
// on demand instead, the first time the user actually leaves the menu.
void app_switch_to_menu_keep_source();
void app_exit_menu();
void app_switch_to_analog(bool is_av_in);
void app_switch_to_hdmi_in();
void app_switch_to_hdzero(bool is_default);
void hdzero_switch_channel(int channel);
#ifdef __cplusplus
}
#endif
