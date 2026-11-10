/**
 * mkb_input.h -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * The goal: let a recompiled title be played like a native PC FPS --
 * raw mouse deltas drive the aim stick, WASD drives movement, mouse
 * buttons drive the triggers -- regardless of which path the title
 * takes to read its pad (the emulated USB device or the kernel's
 * XInput ordinals), because the merge happens in xbox_InputGetState,
 * which both go through.
 *
 * All knobs are env vars (and an optional mkb.ini), so no rebuild is
 * needed to tune the feel:
 *
 *   RECOMP_MKB=0|1        master switch            (default off, opt-in)
 *   RECOMP_MKB_SENS=f     mouse sensitivity        (default 1.0)
 *   RECOMP_MKB_FULL=n     mouse counts for a full  (default 8)
 *                         stick throw, per poll
 *   RECOMP_MKB_DZ=f       assumed game deadzone    (default 0.18)
 *   RECOMP_MKB_ACC=f      aim acceleration         (default 1.0 = off)
 *   RECOMP_MKB_INVERT_Y=1 invert vertical aim      (default off)
 *   RECOMP_MKB_INI=path   config file (same keys, one per line)
 *
 * F9 toggles mouse/keyboard capture at runtime. Capture is only
 * applied while the recompiled game's process has the foreground
 * window, so the mouse keeps working normally in other apps.
 */

#ifndef XBOXRECOMP_MKB_INPUT_H
#define XBOXRECOMP_MKB_INPUT_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when the layer is enabled (RECOMP_MKB not "0"). Cheap; cached. */
int mkb_enabled(void);

/*
 * Merge the mouse+keyboard virtual pad on top of *pState, in place.
 * Buttons OR in, analog buttons take the larger value, and the aim
 * stick is taken over by the mouse whenever the mouse moved since the
 * last poll. Bumps dwPacketNumber when the virtual pad contributed
 * anything, so a title watching for edges sees the presses.
 *
 * Returns 1 when the virtual pad is active and produced a usable
 * state (so a caller treating "no host pad" as an error can still
 * report a connected controller), 0 otherwise.
 */
int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState);

/* ---- bindings ----------------------------------------------------------
 *
 * Which keys and mouse buttons press which pad control: a table the poll
 * reads every time, so a change takes effect on the next frame. Each control
 * has MKB_SLOTS inputs; any of them pressed presses it. The mouse's movement
 * always drives the aim stick; the LOOK rows add keys for it. 
 * unwritt3n walk patch: added MKB_RUN*/
enum {
    MKB_A, MKB_B, MKB_X, MKB_Y, MKB_BLACK, MKB_WHITE,
    MKB_LTRIGGER, MKB_RTRIGGER, MKB_START, MKB_BACK,
    MKB_LTHUMB, MKB_RTHUMB,
    MKB_DPAD_UP, MKB_DPAD_DOWN, MKB_DPAD_LEFT, MKB_DPAD_RIGHT,
    MKB_MOVE_UP, MKB_MOVE_DOWN, MKB_MOVE_LEFT, MKB_MOVE_RIGHT,
    MKB_LOOK_UP, MKB_LOOK_DOWN, MKB_LOOK_LEFT, MKB_LOOK_RIGHT,
    MKB_RUN,
    MKB_ACTION_COUNT
};
#define MKB_SLOTS 2

/* Input codes: 0 unbound, 1..255 a Windows virtual key (left and right
 * Shift, Ctrl and Alt told apart: VK_LSHIFT...VK_RMENU), then the mouse. */
#define MKB_MOUSE_LEFT    0x101
#define MKB_MOUSE_RIGHT   0x102
#define MKB_MOUSE_MIDDLE  0x103
#define MKB_MOUSE_X1      0x104
#define MKB_MOUSE_X2      0x105
#define MKB_WHEEL_UP      0x106
#define MKB_WHEEL_DOWN    0x107

int  mkb_binding(int action, int slot);
/* Bind code to action's slot. The code is taken off every other control
 * first, so one key never presses two things. code 0 clears the slot. */
void mkb_bind(int action, int slot, int code);
/* The toolkit's own layout (Black's). A port sets its own with mkb_bind. */
void mkb_bindings_default(void);
/* A stable name for the action, for config files ("A", "DpadUp", ...). */
const char *mkb_action_key(int action);
/* The input's name for a menu: the keyboard layout's own key name. */
void mkb_input_name(int code, char *buf, int size);

/* "Press a key": the next key, mouse button or wheel step pressed after
 * everything is released. Delete or Home cancel (they open the port menu);
 * F9 is the capture toggle and is never taken. mkb_capture_result is -1
 * while waiting, 0 when cancelled, else the code. */
void mkb_capture_begin(void);
void mkb_capture_cancel(void);
int  mkb_capture_result(void);

double mkb_sensitivity(void);
void   mkb_set_sensitivity(double s);
int    mkb_invert_y(void);
void   mkb_set_invert_y(int on);

/* While set (the port's menu is open) the virtual pad gives nothing and the
 * pointer is free; on release, what was moved meanwhile is dropped. */
void mkb_set_suspended(int on);

/* The mouse's travel since the last call (counts; y down positive, or up
 * with invert), for a title hook that turns its view directly. Calling it
 * keeps the mouse off the right stick for the next 250 ms. 0 while the
 * mouse is not the game's (menu open, other window). */
int mkb_take_look(long *dx, long *dy);
/* The mouse belongs to the game now (no pointer should show). */
int mkb_capturing(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_MKB_INPUT_H */
