#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <time.h>

/* Longest DVR clip name the reaper handles: "hdz_" + 4 digits + '-' + a 43-char
 * ELRS label + ".mp4" fits comfortably, and anything longer was not written by
 * this recorder and is therefore not ours to delete. */
#define ROLLING_NAME_MAX 64

typedef struct {
    char   name[ROLLING_NAME_MAX];
    time_t mtime;
} RollingCandidate_t;

/* True when `name` is a clip this recorder wrote and may reclaim.
 *
 * Deliberately an allow-list, not a deny-list: only the two naming schemes the
 * recorder produces qualify, so anything else the user keeps in the DVR folder
 * is never touched. Favourites (the "hot_" prefix the playback page renames to)
 * are an explicit "keep this" and are excluded as well. */
bool rolling_is_candidate(const char *name);

/* Collects up to `max` reclaimable clips from `packPath`, oldest first.
 *
 * `keepName` is the bare file name of the clip currently being written, or NULL.
 * Returns the number of entries written to `out`. Ordering is by modification
 * time, with the file name breaking ties -- for both naming schemes the name is
 * itself chronological, which keeps the order sane on a goggle whose RTC has
 * lost its battery. */
int rolling_scan(const char *packPath, const char *keepName,
                 RollingCandidate_t *out, int max);

/* Removes one clip plus the files the recorder keeps beside it (the .jpg
 * thumbnail and the .star.txt in-flight marker). Returns 0 when the clip itself
 * was removed. Re-checks rolling_is_candidate() so a caller mistake cannot
 * delete a favourite or a foreign file. */
int rolling_delete_clip(const char *packPath, const char *name);

#ifdef __cplusplus
}
#endif
