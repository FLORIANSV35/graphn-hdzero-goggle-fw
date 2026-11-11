// dm5680_emu.c - Emulator-only mock of the DM5680 receiver's serial link.
//
// On the goggle, two DM5680 chips talk to the app over UART. The app polls them
// (version, valid-channel, RSSI) and they answer; dm5680.c's recv threads block in
// select() on the real serial fd until a reply arrives. The desktop emulator has no
// DM5680, so uart_open() hands back a socketpair instead of a tty (see uart.c). This
// module sits on the *device* end of those socketpairs and plays the receiver: it
// reads the goggle's command frames and writes back plausible telemetry, so the app
// sees a connected receiver -- and its recv threads block on real data at 0% CPU
// rather than spinning on a dead fd.
//
// Wire format (see dm5680.c):
//   host -> device command:  0xAA 0x55 LEN CMD [payload...]   (LEN = bytes after LEN)
//   device -> host response:  0xCC 0x33 LEN CMD [data...]      (LEN = bytes after LEN)

#ifdef EMULATOR_BUILD

#include "dm5680_emu.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>

#include "driver/uart.h"

// Canned telemetry the mock reports. Values chosen to look like a healthy link.
#define EMU_RX_VER   0x0F // receiver firmware version byte
#define EMU_RSSI_0   90
#define EMU_RSSI_1   88
#define EMU_DLQ      100  // downlink quality (0..100)
#define EMU_STAT     0
// Whether the mock reports a locked HDZero channel. 0 = present but no lock, so the
// app does NOT auto-tune out of the menu on boot/scan (the FPV view still shows the
// fake video -- compositing is gated on the view, not this flag). Set to 1 to
// emulate a live signal that autoscan will lock onto and switch to video.
#define EMU_CHANNEL_VALID 0

// DM5680 UART ports (must match uart_open() calls in uart_init()).
static const int kPorts[2] = {1, 2};

typedef struct {
    int fd; // device end of the UART socketpair
    pthread_t tid;
} emu_port_t;

static emu_port_t g_ports[2];

// Test hooks (emulator only, driven by the environment):
//   HDZ_EMU_RX_SILENT_MS=N  the mock on UART1 answers nothing for the first N ms
//                           (cold-start "receiver not listening" case)
//   HDZ_EMU_FUZZ=1          also send malformed/random frames on both UARTs
//   HDZ_EMU_SEED=S          seed of the fuzzer (default 1)
static uint32_t g_silent_ms = 0;
static struct timespec g_t0;

static uint32_t emu_elapsed_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - g_t0.tv_sec) * 1000 + (t.tv_nsec - g_t0.tv_nsec) / 1000000);
}

// Frame a response payload (CMD + data) as 0xCC 0x33 LEN <payload> and send it.
static void send_resp(int fd, const uint8_t *payload, uint8_t len) {
    uint8_t frame[64];
    if (len > sizeof(frame) - 3)
        return;
    frame[0] = 0xCC;
    frame[1] = 0x33;
    frame[2] = len;
    memcpy(&frame[3], payload, len);
    ssize_t rc = write(fd, frame, (size_t)len + 3);
    (void)rc;
}

// Answer one command from the goggle. Only the polling queries need a reply;
// config writes (fan speed, resets, etc.) are accepted silently, like real HW.
static void handle_cmd(int fd, uint8_t cmd) {
    switch (cmd) {
    case 0x01: { // version request
        uint8_t d[] = {0x01, EMU_RX_VER};
        send_resp(fd, d, sizeof(d));
        break;
    }
    case 0x11: { // valid-channel request -> report the channel as valid
        uint8_t d[] = {0x11, 1};
        send_resp(fd, d, sizeof(d));
        break;
    }
    case 0x12: { // RSSI request -> rssi0, rssi1, DLQ, Stat, crc(sum of the four)
        uint8_t crc = (uint8_t)(EMU_RSSI_0 + EMU_RSSI_1 + EMU_DLQ + EMU_STAT);
        uint8_t d[] = {0x12, EMU_RSSI_0, EMU_RSSI_1, EMU_DLQ, EMU_STAT, crc};
        send_resp(fd, d, sizeof(d));
        break;
    }
    case 0x14: { // VTX info -> type, ver, stat, crc(sum of the three)
        uint8_t type = 1, ver = 1, stat = 1;
        uint8_t crc = (uint8_t)(type + ver + stat);
        uint8_t d[] = {0x14, type, ver, stat, crc};
        send_resp(fd, d, sizeof(d));
        break;
    }
    default:
        break; // no reply expected
    }
}

// Read host command bytes and drive a tiny 0xAA/0x55/LEN/CMD/payload parser. The
// read() blocks until the goggle actually sends something, so an idle link costs
// no CPU -- exactly like waiting on a real UART.
static void *port_thread(void *arg) {
    emu_port_t *p = (emu_port_t *)arg;
    uint8_t buf[128];
    int state = 0;
    uint8_t remaining = 0;

    for (;;) {
        ssize_t n = read(p->fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n == 0)
                break; // peer closed
            usleep(5000);
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            uint8_t b = buf[i];
            switch (state) {
            case 0:
                if (b == 0xAA)
                    state = 1;
                break;
            case 1:
                state = (b == 0x55) ? 2 : 0;
                break;
            case 2: // LEN = number of bytes after LEN (CMD + payload)
                remaining = b;
                state = remaining ? 3 : 0;
                break;
            case 3: // CMD
                if (!(p == &g_ports[0] && emu_elapsed_ms() < g_silent_ms))
                    handle_cmd(p->fd, b);
                remaining--;
                state = remaining ? 4 : 0;
                break;
            case 4: // consume this command's payload bytes
                if (--remaining == 0)
                    state = 0;
                break;
            }
        }
    }
    return NULL;
}

// Malformed traffic: junk bytes, truncated frames, wrong lengths, OSD (0x15)
// frames with random MSP content. The app must neither crash nor hang.
static void *fuzz_thread(void *arg) {
    (void)arg;
    uint8_t frame[300];

    for (;;) {
        int port = rand() & 1;
        int fd = g_ports[port].fd;
        size_t n = 0;

        switch (rand() % 4) {
        case 0: // pure junk
            n = 1 + rand() % 64;
            for (size_t i = 0; i < n; i++)
                frame[i] = (uint8_t)rand();
            break;
        case 1: // framed, random command, random declared length vs real length
            frame[0] = 0xCC;
            frame[1] = 0x33;
            frame[2] = (uint8_t)rand();
            n = 3 + rand() % 40;
            for (size_t i = 3; i < n; i++)
                frame[i] = (uint8_t)rand();
            break;
        case 2: // OSD frame carrying a random MSP DisplayPort-looking payload
            frame[0] = 0xCC;
            frame[1] = 0x33;
            n = 4 + rand() % 120;
            frame[2] = (uint8_t)(n - 3);
            frame[3] = 0x15;
            for (size_t i = 4; i < n; i++)
                frame[i] = (rand() % 3) ? (uint8_t)rand() : "$MD<>"[rand() % 5];
            break;
        default: // truncated valid-looking RSSI / vtx frames
            frame[0] = 0xCC;
            frame[1] = 0x33;
            frame[2] = (uint8_t)(rand() % 8);
            frame[3] = (uint8_t)(0x11 + rand() % 4);
            n = 3 + rand() % 6;
            for (size_t i = 4; i < n; i++)
                frame[i] = (uint8_t)rand();
            break;
        }
        ssize_t rc = write(fd, frame, n);
        (void)rc;
        usleep(1000 + rand() % 9000);
    }
    return NULL;
}

void dm5680_emu_start(void) {
    int started = 0;
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    const char *e = getenv("HDZ_EMU_RX_SILENT_MS");
    g_silent_ms = e ? (uint32_t)atoi(e) : 0;
    if (g_silent_ms)
        LOGI("dm5680_emu: UART1 stays silent for %u ms", g_silent_ms);
    for (int i = 0; i < 2; i++) {
        int fd = uart_emu_peer_fd(kPorts[i]);
        if (fd < 0)
            continue;
        g_ports[i].fd = fd;
        if (pthread_create(&g_ports[i].tid, NULL, port_thread, &g_ports[i]) == 0)
            started++;
    }
    const char *fz = getenv("HDZ_EMU_FUZZ");
    if (fz && atoi(fz) && started) {
        const char *sd = getenv("HDZ_EMU_SEED");
        srand(sd ? (unsigned)atoi(sd) : 1u);
        pthread_t t;
        if (pthread_create(&t, NULL, fuzz_thread, NULL) == 0) {
            pthread_detach(t);
            LOGI("dm5680_emu: fuzzer running");
        }
    }
    LOGI("dm5680_emu: mock receiver attached on %d UART(s) (version, valid channel, RSSI)", started);
}

#endif // EMULATOR_BUILD
