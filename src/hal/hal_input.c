#include "hal/hal_input.h"
#include "core/logger.h"

#include <pspctrl.h>
#include <pspwlan.h>

int hal_input_init(void)
{
    int ret;
    
    ret = sceCtrlSetSamplingCycle(0);
    if (ret < 0) {
        logLine("hal_input: failed to set sampling cycle (0x%08X)\n", ret);
        return -1;
    }
    
    ret = sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    if (ret < 0) {
        logLine("hal_input: failed to set analog mode (0x%08X)\n", ret);
        return -1;
    }
    
    logLine("hal_input: initialized (analog mode)\n");
    return 0;
}

void hal_input_shutdown(void)
{
    // Reset to default mode
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    logLine("hal_input: shutdown\n");
}

void hal_input_poll(InputState *out_state)
{
    static u32 prev_buttons = 0;
    static int switches_initialized = 0;
    static int prev_hold = 0;
    static int prev_ctrl_wlan = 0;
    static int prev_wlan_api = 0;
    SceCtrlData pad;
    int ret;
    int wlan_api;

    if (!out_state) {
        return;
    }

    out_state->buttons = 0;
    out_state->pressed = 0;
    out_state->hold = 0;
    wlan_api = sceWlanGetSwitchState();
    out_state->wlan_on = wlan_api ? 1 : 0;

    ret = sceCtrlPeekBufferPositive(&pad, 1);
    if (ret <= 0) {
        return;
    }

    out_state->buttons = pad.Buttons;
    out_state->pressed = (pad.Buttons ^ prev_buttons) & pad.Buttons;
    out_state->hold = (pad.Buttons & PSP_CTRL_HOLD) ? 1 : 0;
    prev_buttons = pad.Buttons;

    {
        int hold = out_state->hold;
        int ctrl_wlan = (pad.Buttons & PSP_CTRL_WLAN_UP) ? 1 : 0;

        if (!switches_initialized || hold != prev_hold ||
            ctrl_wlan != prev_ctrl_wlan || wlan_api != prev_wlan_api) {
            logLine("hal_switch: %s hold=%d ctrl_wlan=%d wlan_api=%d buttons=0x%08X\n",
                    switches_initialized ? "changed" : "initial",
                    hold, ctrl_wlan, wlan_api, (unsigned int)pad.Buttons);
            prev_hold = hold;
            prev_ctrl_wlan = ctrl_wlan;
            prev_wlan_api = wlan_api;
            switches_initialized = 1;
        }
    }

    // Log button presses (only when actually pressed, not held)
    if (out_state->pressed) {
        if (out_state->pressed & PSP_CTRL_SELECT)   logLine("hal_input: SELECT pressed\n");
        if (out_state->pressed & PSP_CTRL_START)    logLine("hal_input: START pressed\n");
        if (out_state->pressed & PSP_CTRL_UP)       logLine("hal_input: UP pressed\n");
        if (out_state->pressed & PSP_CTRL_RIGHT)    logLine("hal_input: RIGHT pressed\n");
        if (out_state->pressed & PSP_CTRL_DOWN)     logLine("hal_input: DOWN pressed\n");
        if (out_state->pressed & PSP_CTRL_LEFT)     logLine("hal_input: LEFT pressed\n");
        if (out_state->pressed & PSP_CTRL_LTRIGGER) logLine("hal_input: L TRIGGER pressed\n");
        if (out_state->pressed & PSP_CTRL_RTRIGGER) logLine("hal_input: R TRIGGER pressed\n");
        if (out_state->pressed & PSP_CTRL_TRIANGLE) logLine("hal_input: TRIANGLE pressed\n");
        if (out_state->pressed & PSP_CTRL_CIRCLE)   logLine("hal_input: CIRCLE (OK) pressed\n");
        if (out_state->pressed & PSP_CTRL_CROSS)    logLine("hal_input: CROSS (X) pressed\n");
        if (out_state->pressed & PSP_CTRL_SQUARE)   logLine("hal_input: SQUARE pressed\n");
    }
}
