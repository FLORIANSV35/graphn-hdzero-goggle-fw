#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "ui/ui_main_menu.h"

extern page_pack_t pp_wifi;

extern void page_wifi_get_statusbar_text(char *buffer, int size);

// WiFi Share (Tools page): brings the hotspot and the web portal up for the
// time of the share, then back to whatever the WiFi settings say.
bool wifi_share_available(void);
bool wifi_share_active(void);
void wifi_share_start(void);
void wifi_share_stop(void);

#ifdef __cplusplus
}
#endif
