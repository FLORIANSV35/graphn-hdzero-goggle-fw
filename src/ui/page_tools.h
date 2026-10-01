#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl/lvgl.h>

#include "ui/ui_main_menu.h"

extern page_pack_t pp_tools;

// Scan page integration (Tools > Scan Page = RSSI Scanner).
bool tools_scan_page_is_rssi(void);
bool tools_rssi_scan_active(void);
void tools_rssi_scan_open(void);
void tools_rssi_scan_close(void);

#ifdef __cplusplus
}
#endif
