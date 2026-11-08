#include "log/log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define get_filename(file) (strrchr(file, '/') ? strrchr(file, '/') + 1 : file)

static int fd_log_file = -1;
static char offline_buffer[1 * 1024 * 1024];
static size_t offline_offset = 0;

#define VIDEO_LOG_MAX (8 * 1024 * 1024) // per video, then lines are dropped
static int fd_video_log = -1;
static char video_buffer[256 * 1024];
static size_t video_offset = 0;
static size_t video_written = 0;
static atomic_bool log_mutex_init = false; // was ATOMIC_VAR_INIT(false) — macro removed in C23 / modern GCC
static pthread_mutex_t log_mutex;

static const char *log_level_names[] = {
    "DEBUG",   // -2
    "VERBOSE", // -1
    "INFO",    // 0
    "WARN",    // 1
    "ERROR",   // 2
    "FATAL",   // 3
};

/**
 * Worker thread for updating log file on SD Card.
 */
static void *log_sync_thread(void *arg) {

    while (true) {
        pthread_mutex_lock(&log_mutex);

        if (fd_log_file >= 0 && offline_offset > 0) {
            write(fd_log_file, offline_buffer, offline_offset);
            offline_offset = 0;
        }

        if (fd_video_log >= 0 && video_offset > 0) {
            write(fd_video_log, video_buffer, video_offset);
            video_written += video_offset;
            video_offset = 0;
        }

        pthread_mutex_unlock(&log_mutex);

        sleep(1);
    }
}

/**
 * Once initialized detach until completed.
 */
static bool log_init_thread() {
    pthread_t tid;
    pthread_mutex_init(&log_mutex, NULL);

    if (!pthread_create(&tid, NULL, log_sync_thread, NULL)) {
        pthread_detach(tid);
    } else {
        return false;
    }
    return true;
}

int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) {
    // probably slow to check this every time
    if (!log_mutex_init) {

        log_mutex_init = log_init_thread();
    }

    pthread_mutex_lock(&log_mutex);

    va_list args1;
    va_start(args1, fmt);
    va_list args2;
    va_copy(args2, args1);
    char buf[1 + vsnprintf(NULL, 0, fmt, args1)];
    va_end(args1);
    vsnprintf(buf, sizeof buf, fmt, args2);
    va_end(args2);
    static char buffer[1024];

    // Timestamp
    struct timeval now;
    gettimeofday(&now, NULL);
    int milli = now.tv_usec / 1000;
    char timestamp[sizeof "YYYY-MM-DD HH:MM:SS.sss"];
    int bytes = strftime(timestamp, sizeof timestamp, "%F %T.", gmtime(&now.tv_sec));
    snprintf(&timestamp[bytes], sizeof(timestamp) - bytes, "%03d", milli);

    bytes =
        snprintf(buffer,
                 sizeof(buffer),
                 "%s [%s][%s:%s:%d] %s\r\n",
                 timestamp,
                 log_level_names[level + 2],
                 get_filename(file),
                 func,
                 line,
                 buf);

    int ret = printf("%s", buffer);
    fflush(stdout);

    // Copy or Rollover ?
    if (offline_offset + bytes < sizeof(offline_buffer)) {
        offline_offset += snprintf(&offline_buffer[offline_offset],
                                   sizeof(offline_buffer) - offline_offset - bytes,
                                   "%s",
                                   buffer);
    } else {
        offline_offset = snprintf(offline_buffer,
                                  sizeof(offline_buffer) - bytes,
                                  "%s",
                                  buffer);
    }

    // Per-video copy; a line that does not fit is dropped (the buffer is
    // flushed every second)
    if (fd_video_log >= 0) {
        size_t len = strnlen(buffer, sizeof(buffer));
        if (video_written + video_offset + len < VIDEO_LOG_MAX &&
            video_offset + len < sizeof(video_buffer)) {
            memcpy(&video_buffer[video_offset], buffer, len);
            video_offset += len;
        }
    }

    pthread_mutex_unlock(&log_mutex);

    return ret;
}

bool log_file_opened() {
    bool retval = false;

    pthread_mutex_lock(&log_mutex);

    retval = fd_log_file == -1 ? false : true;

    pthread_mutex_unlock(&log_mutex);

    return retval;
}

bool log_file_open(const char *filename) {
    bool retval = false;

    pthread_mutex_lock(&log_mutex);

    fd_log_file = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd_log_file >= 0) {
        retval = true;
    }

    pthread_mutex_unlock(&log_mutex);

    return retval;
}

void log_file_close() {
    pthread_mutex_lock(&log_mutex);

    if (fd_log_file >= 0) {
        close(fd_log_file);
        fd_log_file = -1;
    }

    pthread_mutex_unlock(&log_mutex);
}

bool log_video_open(const char *filename) {
    bool retval = false;

    log_video_close();

    pthread_mutex_lock(&log_mutex);

    fd_video_log = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd_video_log >= 0) {
        video_offset = 0;
        video_written = 0;
        retval = true;
    }

    pthread_mutex_unlock(&log_mutex);

    return retval;
}

void log_video_close() {
    pthread_mutex_lock(&log_mutex);

    if (fd_video_log >= 0) {
        if (video_offset > 0)
            write(fd_video_log, video_buffer, video_offset);
        video_offset = 0;
        close(fd_video_log);
        fd_video_log = -1;
    }

    pthread_mutex_unlock(&log_mutex);
}
