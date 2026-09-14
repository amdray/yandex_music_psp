#ifndef YM_HAL_INPUT_H
#define YM_HAL_INPUT_H

#include <psptypes.h>

/**
 * Input state with button press detection
 */
typedef struct InputState {
    u32 buttons;  /**< Currently held buttons (bitmask of PSP_CTRL_*) */
    u32 pressed;  /**< Buttons just pressed this frame (edge detection) */
    int hold;
    int wlan_on;
} InputState;

/**
 * Initialize PSP controller subsystem
 * @return 0 on success, <0 on error
 */
int hal_input_init(void);

/**
 * Shutdown controller and reset to defaults
 */
void hal_input_shutdown(void);

/**
 * Poll current button state
 * @param out_state Output state (must not be NULL)
 */
void hal_input_poll(InputState *out_state);

#endif
