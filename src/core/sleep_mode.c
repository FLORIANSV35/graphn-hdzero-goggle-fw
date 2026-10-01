#include "sleep_mode.h"

#include "core/app_state.h"
#include "core/common.hh"
#include "core/dvr.h"
#include "core/settings.h"
#include "driver/beep.h"
#include "driver/dm5680.h"
#include "driver/fans.h"
#include "driver/hardware.h"
#include "driver/rtc6715.h"
#include "driver/screen.h"
#include "log/log.h"
#include "ui/page_fans.h"

static app_state_t previousState;
static int fans_auto_mode_save;
static fan_speed_t fan_speed_save;

static uint16_t beepCnt = 0;

bool isSleeping = false;

void go_sleep() {
    LOGI("Entering sleep mode");
    previousState = g_app_state;
    app_state_push(APP_STATE_SLEEP);

    // Stop DVR
    dvr_cmd(DVR_STOP);
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    dvr_update_vi_conf(VR_1080P30);
#elif defined(HDZBOXPRO)
    dvr_update_vi_conf(VR_720P60);
#endif

    screen_set_inhibited(SCREEN_INHIBIT_SLEEP, true);

    // Turn off HDZero Receiver
    HDZero_Close();

    // Turn off Analog Receiver
    if (getHwRevision() == HW_REV_2) {
        DM5680_ExternalAnalog_Power(1);
    }
    rtc6715.init(0, 0);

    // Minimum fan
    fans_auto_mode_save = g_setting.fans.auto_mode;
    fan_speed_save.top = fan_speed.top;
    fan_speed_save.left = fan_speed.left;
    fan_speed_save.right = fan_speed.right;
    g_setting.fans.top_speed = MIN_FAN_TOP;
    g_setting.fans.left_speed = MIN_FAN_SIDE;
    g_setting.fans.right_speed = MIN_FAN_SIDE;
    g_setting.fans.auto_mode = 0;
    fans_top_setspeed(MIN_FAN_TOP);
    fans_left_setspeed(MIN_FAN_SIDE);
    fans_right_setspeed(MIN_FAN_SIDE);
    isSleeping = true;
    beepCnt = 0;
}

void wake_up() {
    LOGI("Exiting sleep mode");
    isSleeping = false;
    screen_set_inhibited(SCREEN_INHIBIT_SLEEP, false);

    Analog_Module_Power(1);
    g_setting.fans.right_speed = fan_speed_save.right;
    g_setting.fans.left_speed = fan_speed_save.left;
    fans_right_setspeed(fan_speed_save.right);
    fans_left_setspeed(fan_speed_save.left);

    g_setting.fans.top_speed = fan_speed_save.top;
    g_setting.fans.auto_mode = fans_auto_mode_save;
    fans_top_setspeed(fan_speed_save.top);

    app_state_push(previousState);
    if (previousState == APP_STATE_SUBMENU) {
        submenu_exit();
    } else if (previousState == APP_STATE_VIDEO) {
        app_switch_to_menu();          // Necessary to display the progress bar
        app_state_push(previousState); // Because app_switch_to_menu() pushes main menu state
        app_exit_menu();
    }
}

void sleep_reminder() {
    if (isSleeping == false) {
        return;
    }

#define BEEP_INTERVAL 4000
    if (++beepCnt == BEEP_INTERVAL) {
        beep_dur(BEEP_VERY_SHORT);
        beepCnt = 0;
    }
}

static bool power_save_on = false;
static int ps_fans_auto_mode;
static fan_speed_t ps_fan_speed;
static bool ps_fans_min;

void power_save_enter(void) {
    if (power_save_on)
        return;
    power_save_on = true;
    LOGI("Power save on");

    // The receivers are going off: nothing left to record, and the card is about to be read by the portal
    if (dvr_is_recording)
        dvr_cmd(DVR_STOP);

    // Same steps as go_sleep() for the receivers and the fans
    HDZero_Close();
    if (getHwRevision() == HW_REV_2) {
        DM5680_ExternalAnalog_Power(1);
    }
    rtc6715.init(0, 0);

    ps_fans_auto_mode = g_setting.fans.auto_mode;
    ps_fan_speed.top = fan_speed.top;
    ps_fan_speed.left = fan_speed.left;
    ps_fan_speed.right = fan_speed.right;
    g_setting.fans.top_speed = MIN_FAN_TOP;
    g_setting.fans.left_speed = MIN_FAN_SIDE;
    g_setting.fans.right_speed = MIN_FAN_SIDE;
    g_setting.fans.auto_mode = 0;
    fans_top_setspeed(MIN_FAN_TOP);
    fans_left_setspeed(MIN_FAN_SIDE);
    fans_right_setspeed(MIN_FAN_SIDE);
    ps_fans_min = true;

    // Dim, don't switch off: the share window has to stay readable
    screen.brightness(1);
}

void power_save_exit(void) {
    if (!power_save_on)
        return;
    power_save_on = false;
    ps_fans_min = false;
    LOGI("Power save off");

    screen.brightness(g_setting.image.oled);

    Analog_Module_Power(1);
    g_setting.fans.right_speed = ps_fan_speed.right;
    g_setting.fans.left_speed = ps_fan_speed.left;
    fans_right_setspeed(ps_fan_speed.right);
    fans_left_setspeed(ps_fan_speed.left);
    g_setting.fans.top_speed = ps_fan_speed.top;
    g_setting.fans.auto_mode = ps_fans_auto_mode;
    fans_top_setspeed(ps_fan_speed.top);
}

void power_save_fans(bool minimum) {
    if (!power_save_on || minimum == ps_fans_min)
        return;
    ps_fans_min = minimum;
    if (minimum) {
        g_setting.fans.top_speed = MIN_FAN_TOP;
        g_setting.fans.left_speed = MIN_FAN_SIDE;
        g_setting.fans.right_speed = MIN_FAN_SIDE;
        g_setting.fans.auto_mode = 0;
        fans_top_setspeed(MIN_FAN_TOP);
        fans_left_setspeed(MIN_FAN_SIDE);
        fans_right_setspeed(MIN_FAN_SIDE);
    } else {
        g_setting.fans.right_speed = ps_fan_speed.right;
        g_setting.fans.left_speed = ps_fan_speed.left;
        fans_right_setspeed(ps_fan_speed.right);
        fans_left_setspeed(ps_fan_speed.left);
        g_setting.fans.top_speed = ps_fan_speed.top;
        g_setting.fans.auto_mode = ps_fans_auto_mode;
        fans_top_setspeed(ps_fan_speed.top);
    }
}
