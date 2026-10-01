#pragma once

#include <stdbool.h>

extern bool isSleeping;

void go_sleep();
void wake_up();
void sleep_reminder();

// Reduced consumption while the WiFi Share window is up (Tools): receivers
// off, fans to minimum, panel dimmed -- the screen stays readable, unlike
// the full sleep mode. power_save_exit() puts everything back.
void power_save_enter(void);
void power_save_exit(void);
// While a long job runs (a conversion, a light copy) the fans go back to their normal setting,
// and to the minimum again when it is over. No effect outside a power save.
void power_save_fans(bool minimum);
