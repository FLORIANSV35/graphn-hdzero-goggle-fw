#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#define SD_BLOCK_DEVICE "/dev/mmcblk0"
// Early warning threshold, well above sdcard_is_full()'s near-empty one --
// low enough to still record for a while, high enough to give time to offload
// footage before the card actually fills up. User-configurable (Storage
// page) between MIN and MAX in STEP increments; g_setting.storage
// .low_space_alert_mb holds the current value.
#define SD_LOW_SPACE_MIN_MB     1000
#define SD_LOW_SPACE_MAX_MB     5000
#define SD_LOW_SPACE_STEP_MB    500
#define SD_LOW_SPACE_DEFAULT_MB 2000

bool sdcard_mounted();
bool sdcard_inserted();
void sdcard_update_free_size();
int sdcard_free_size();
bool sdcard_is_full();
// True once free space drops below g_setting.storage.low_space_alert_mb (but
// the card isn't unmounted/unreadable, which reports 0).
bool sdcard_is_low();
// Returns true when the FAT clean-shutdown flag says a check is warranted, or
// when the volume cannot be identified safely.
bool sdcard_filesystem_dirty();

#ifdef __cplusplus
}
#endif
