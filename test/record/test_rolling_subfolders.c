// The rolling reaper (src/record/rolling.c) frees space by removing the oldest clips of the DVR
// folder. It must never go into a subfolder: Favorites/ holds the clips the user keeps, Light/ the
// light copies made for the WiFi portal. This builds a folder like the goggle's and checks it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../src/record/rolling.h"

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void touch(const char *dir, const char *sub, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s%s%s", dir, sub, sub[0] ? "/" : "", name);
    FILE *f = fopen(path, "w");
    check(f != NULL, "cannot create a test file");
    fputs("x", f);
    fclose(f);
}

static int exists(const char *dir, const char *sub, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s%s%s", dir, sub, sub[0] ? "/" : "", name);
    return access(path, F_OK) == 0;
}

int main(void) {
    char tmpl[] = "/tmp/rolling_test_XXXXXX";
    check(mkdtemp(tmpl) != NULL, "mkdtemp");
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/", tmpl);                 // the recorder's packPath ends with a slash

    char sub[320];
    snprintf(sub, sizeof(sub), "%sFavorites", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof(sub), "%sLight", dir);
    mkdir(sub, 0755);

    // the folder itself: two recorder clips, a favourite by prefix, something foreign
    touch(dir, "", "20260901-100000.ts");
    touch(dir, "", "hdz_0001.mp4");
    touch(dir, "", "hot_hdz_0002.ts");
    touch(dir, "", "notes.txt");
    // subfolders: names that WOULD qualify if the reaper looked inside
    touch(dir, "Favorites", "20260101-100000.ts");
    touch(dir, "Favorites", "hdz_0009.ts");
    touch(dir, "Light", "20260901-100000.mp4");

    RollingCandidate_t out[16];
    int n = rolling_scan(dir, NULL, out, 16);
    check(n == 2, "only the two clips of the folder itself are candidates");
    for (int i = 0; i < n; i++) {
        check(strcmp(out[i].name, "20260901-100000.ts") == 0 || strcmp(out[i].name, "hdz_0001.mp4") == 0,
              "a candidate that is not a clip of the folder itself");
    }

    // deleting by name only ever touches the folder itself
    check(rolling_delete_clip(dir, "20260101-100000.ts") != 0, "a clip that is only in Favorites must not be deletable");
    check(exists(dir, "Favorites", "20260101-100000.ts"), "the Favorites clip is still there");
    check(exists(dir, "Favorites", "hdz_0009.ts"), "the other Favorites clip is still there");
    check(exists(dir, "Light", "20260901-100000.mp4"), "the light copy is still there");
    check(rolling_delete_clip(dir, "20260901-100000.ts") == 0, "a clip of the folder itself is removed");
    check(!exists(dir, "", "20260901-100000.ts"), "and it is gone");
    check(exists(dir, "Light", "20260901-100000.mp4"), "its light copy of the same name is not touched");

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpl);
    (void)!system(cmd);
    puts("rolling subfolders: ok");
    return 0;
}
