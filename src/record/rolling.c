/* Rolling ("loop") recording: reclaiming space by removing the oldest clips.
 *
 * Kept free of every project dependency except libc on purpose -- the file
 * selection rules are the part that must never be wrong, so they are compiled
 * and exercised directly by test/rolling on the build host. */
#include "rolling.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "record_definitions.h"

#define ROLLING_PATH_MAX (MAX_pathLEN + ROLLING_NAME_MAX + 16)

/* Returns the position of the container extension, or NULL when the name is not
 * a recorded clip. Thumbnails and star markers deliberately do not match: they
 * are removed as companions of their clip, never on their own. */
static const char *rolling_clip_extension(const char *name) {
    const char *dot = strrchr(name, '.');

    if (dot == NULL) {
        return NULL;
    }
    if (strcasecmp(dot, DOT REC_packTS) == 0 || strcasecmp(dot, DOT REC_packMP4) == 0) {
        return dot;
    }

    return NULL;
}

static bool rolling_all_digits(const char *s, int count) {
    for (int i = 0; i < count; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
    }

    return true;
}

bool rolling_is_candidate(const char *name) {
    if (name == NULL || name[0] == '.') {
        return false;
    }

    size_t const len = strlen(name);
    if (len == 0 || len >= ROLLING_NAME_MAX) {
        return false;
    }

    /* Favourites are renamed with the hot_ prefix by the playback page. That
     * rename is the user saying "keep this one", so the reaper skips them even
     * when it means rolling has nothing left to free. */
    if (strncmp(name, REC_hotPREFIX, strlen(REC_hotPREFIX)) == 0) {
        return false;
    }

    const char *dot = rolling_clip_extension(name);
    if (dot == NULL) {
        return false;
    }

    /* Digits and ELRS schemes: hdz_NNNN.ext and hdz_NNNN-<label>.ext */
    size_t const prefix_len = strlen(REC_packPREFIX);
    if (strncmp(name, REC_packPREFIX, prefix_len) == 0) {
        const char *p = name + prefix_len;
        if (!rolling_all_digits(p, REC_packIndexLEN)) {
            return false;
        }
        p += REC_packIndexLEN;
        if (p == dot) {
            return true;
        }
        return (*p == '-') && ((p + 1) < dot);
    }

    /* Date scheme: YYYYMMDD-HHMMSS.ext */
    if ((size_t)(dot - name) == 15 && name[8] == '-') {
        return rolling_all_digits(name, 8) && rolling_all_digits(name + 9, 6);
    }

    return false;
}

static int rolling_compare(const void *lhs, const void *rhs) {
    const RollingCandidate_t *a = (const RollingCandidate_t *)lhs;
    const RollingCandidate_t *b = (const RollingCandidate_t *)rhs;

    if (a->mtime < b->mtime) {
        return -1;
    }
    if (a->mtime > b->mtime) {
        return 1;
    }

    return strcmp(a->name, b->name);
}

static int rolling_newest_index(const RollingCandidate_t *list, int count) {
    int newest = 0;

    for (int i = 1; i < count; i++) {
        if (rolling_compare(&list[i], &list[newest]) > 0) {
            newest = i;
        }
    }

    return newest;
}

int rolling_scan(const char *packPath, const char *keepName,
                 RollingCandidate_t *out, int max) {
    if (packPath == NULL || out == NULL || max <= 0) {
        return 0;
    }

    DIR *dp = opendir(packPath);
    if (dp == NULL) {
        return 0;
    }

    int count = 0;
    int newest = 0;
    struct dirent *entry;
    char path[ROLLING_PATH_MAX];

    while ((entry = readdir(dp)) != NULL) {
        if (!rolling_is_candidate(entry->d_name)) {
            continue;
        }
        if (keepName != NULL && keepName[0] != '\0' &&
            strcmp(entry->d_name, keepName) == 0) {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s%s", packPath, entry->d_name) >= (int)sizeof(path)) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        /* rolling_is_candidate() already rejected anything this long; the
         * explicit bound keeps the copy provably safe for the compiler too. */
        size_t const name_len = strlen(entry->d_name);
        if (name_len >= sizeof(((RollingCandidate_t *)0)->name)) {
            continue;
        }

        RollingCandidate_t candidate;
        memcpy(candidate.name, entry->d_name, name_len + 1);
        candidate.mtime = st.st_mtime;

        if (count < max) {
            out[count] = candidate;
            if (rolling_compare(&out[count], &out[newest]) > 0) {
                newest = count;
            }
            count++;
        } else if (rolling_compare(&candidate, &out[newest]) < 0) {
            /* More clips than the caller's cap: keep the oldest `max` of them so
             * the reaper still deletes in true age order. */
            out[newest] = candidate;
            newest = rolling_newest_index(out, count);
        }
    }

    closedir(dp);
    qsort(out, count, sizeof(out[0]), rolling_compare);

    return count;
}

int rolling_delete_clip(const char *packPath, const char *name) {
    if (packPath == NULL || !rolling_is_candidate(name)) {
        return -1;
    }

    char path[ROLLING_PATH_MAX];

    /* The clip goes first: interrupted half-way this leaves an orphan thumbnail,
     * which the playback page ignores, rather than a clip with no preview. */
    if (snprintf(path, sizeof(path), "%s%s", packPath, name) >= (int)sizeof(path)) {
        return -1;
    }
    if (unlink(path) != 0) {
        return -1;
    }

    if (snprintf(path, sizeof(path), "%s%s%s", packPath, name, REC_starSUFFIX) < (int)sizeof(path)) {
        unlink(path);
    }

    const char *dot = strrchr(name, '.');
    if (dot != NULL &&
        snprintf(path, sizeof(path), "%s%.*s" DOT REC_packSnapTYPE,
                 packPath, (int)(dot - name), name) < (int)sizeof(path)) {
        unlink(path);
    }

    return 0;
}
