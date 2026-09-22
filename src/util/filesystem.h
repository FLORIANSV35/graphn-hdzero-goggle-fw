#pragma once

#include <stdbool.h>
#include <time.h>

bool fs_compare_files(char *f1, char *f2);
bool fs_path_exists(const char *path);
bool fs_file_exists(const char *filename);
bool fs_printf(const char *filename, const char *fmt, ...);
long fs_filesize(const char *filename);
// 0 if the file doesn't exist / stat() fails.
time_t fs_mtime(const char *filename);
const char *fs_basename(const char *path);
