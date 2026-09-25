#include "hal/hal_input.h"
#include "core/logger.h"

#include <pspctrl.h>
#include <pspwlan.h>
#include <stdint.h>
#include <string.h>

#define CTRL_PEEK_POSITIVE_NID 0x3A622550u
#define CTRL_DIALOG_RELEASE_MASK \
    (PSP_CTRL_SELECT | PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_RIGHT | \
     PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | \
     PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | PSP_CTRL_SQUARE | \
     PSP_CTRL_NOTE | PSP_CTRL_SCREEN | PSP_CTRL_VOLUP | PSP_CTRL_VOLDOWN)

struct KernelCallArg {
    u32 arg1;
    u32 arg2;
    u32 arg3;
    u32 arg4;
    u32 arg5;
    u32 arg6;
    u32 arg7;
    u32 arg8;
    u32 arg9;
    u32 arg10;
    u32 arg11;
    u32 arg12;
    u32 ret1;
    u32 ret2;
};

extern u32 sctrlHENFindFunction(const char *module_name,
                                const char *library_name, u32 nid);
extern int kuKernelCall(void *function, struct KernelCallArg *args);

static u32 s_kernel_ctrl_peek;
static int s_kernel_ctrl_failed;

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

    s_kernel_ctrl_peek = sctrlHENFindFunction("sceController_Service",
                                              "sceCtrl_driver",
                                              CTRL_PEEK_POSITIVE_NID);
    s_kernel_ctrl_failed = 0;
    if (s_kernel_ctrl_peek == 0) {
        logLine("hal_input: kernel controller export missing; using user controller sample\n");
    } else {
        logLine("hal_input: kernel controller export resolved\n");
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
    static int home_dialog_active = 0;
    static int switches_initialized = 0;
    static int prev_hold = 0;
    static int prev_ctrl_wlan = 0;
    static int prev_wlan_api = 0;
    SceCtrlData pad;
    SceCtrlData kernel_pad;
    int ret;
    int wlan_api;
    int note_valid = 0;

    if (!out_state) {
        return;
    }

    out_state->buttons = 0;
    out_state->pressed = 0;
    out_state->note_valid = 0;
    out_state->hold = 0;
    wlan_api = sceWlanGetSwitchState();
    out_state->wlan_on = wlan_api ? 1 : 0;

    memset(&pad, 0, sizeof(pad));
    ret = sceCtrlPeekBufferPositive(&pad, 1);
    if (ret <= 0) {
        prev_buttons = 0;
        return;
    }

    /* In user mode PSP_CTRL_HOME remains set while the system exit dialog is
     * visible. Suppress every application action during the dialog and until
     * its navigation/confirmation buttons have all been released. */
    if (pad.Buttons & PSP_CTRL_HOME) {
        if (!home_dialog_active) {
            logLine("hal_input: HOME dialog opened; application input blocked\n");
        }
        home_dialog_active = 1;
        prev_buttons = 0;
        return;
    }

    memset(&kernel_pad, 0, sizeof(kernel_pad));
    if (s_kernel_ctrl_peek != 0 && !s_kernel_ctrl_failed) {
        struct KernelCallArg args;
        int bridge_rc;

        memset(&args, 0, sizeof(args));
        args.arg1 = (u32)(uintptr_t)&kernel_pad;
        args.arg2 = 1;
        bridge_rc = kuKernelCall((void *)s_kernel_ctrl_peek, &args);
        if (bridge_rc < 0 || (int)args.ret1 < 0) {
            logLine("hal_input: kernel controller read failed bridge=%d peek=%d; NOTE unavailable\n",
                    bridge_rc, (int)args.ret1);
            s_kernel_ctrl_failed = 1;
        } else if ((int)args.ret1 > 0) {
            note_valid = 1;
        }
    }

    if (note_valid) {
        pad.Buttons = (pad.Buttons & ~PSP_CTRL_NOTE) |
                      (kernel_pad.Buttons & PSP_CTRL_NOTE);
    }

    if (home_dialog_active) {
        prev_buttons = 0;
        if (pad.Buttons & CTRL_DIALOG_RELEASE_MASK) {
            return;
        }
        home_dialog_active = 0;
        logLine("hal_input: HOME dialog closed; application input resumed\n");
        return;
    }

    out_state->buttons = pad.Buttons;
    out_state->pressed = (pad.Buttons ^ prev_buttons) & pad.Buttons;
    out_state->note_valid = note_valid || s_kernel_ctrl_peek == 0 ||
                            s_kernel_ctrl_failed;
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
        if (out_state->pressed & PSP_CTRL_NOTE)     logLine("hal_input: NOTE pressed\n");
    }
}
