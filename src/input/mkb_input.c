/*
 * mkb_input.c -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * A hidden window on its own thread registers for Raw Input with
 * RIDEV_INPUTSINK, so mouse deltas and key presses arrive whether or
 * not the game's own window is the one with focus, and no matter which
 * window the runtime happens to present through (the GDI debug window,
 * the FMV window, a D3D11 swap chain -- the input layer should not
 * have to care).
 *
 * The poll side drains the accumulated deltas and turns them into a
 * stick deflection that is proportional to the movement since the last
 * poll: the title integrates deflection over its own frame, so the
 * total rotation comes out proportional to total mouse travel at any
 * frame rate, which is the property mouse aim has to keep.
 *
 * Small movements get a floor of RECOMP_MKB_DZ of full throw, because
 * every console title applies its own dead zone to the stick, and a
 * mouse correction smaller than it would be silently eaten.
 *
 * Which key presses which pad control is a table (mkb_bind), read on every
 * poll, so a port's menu can rebind it while the game runs.
 */

/* Raw input needs a modern _WIN32_WINNT; the toolkit's platform header
 * includes windows.h first, so these have to land before anything else. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define _CRT_SECURE_NO_WARNINGS

#include "mkb_input.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- configuration ----------------------------------------------------- */

typedef struct {
    double sens;        /* multiplier on the aim deflection          */
    double dz;          /* assumed game deadzone, 0..1               */
    double acc;         /* aim acceleration exponent, 1 = linear     */
    long   full;        /* mouse counts for full throw, per poll     */
    int    invert_y;    /* invert the vertical aim axis              */
    int    swap_sticks; /* mouse aims with the left stick instead    */
} MkbConfig;

static MkbConfig s_cfg = { 1.0, 0.18, 1.0, 8, 0, 0 };

/* One "key=value" line; returns 1 when the key was recognised. */
static int mkb_apply_kv(const char *key, const char *val)
{
    double d;
    long   l;

    if (!strcmp(key, "sens")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.sens = d;
    } else if (!strcmp(key, "dz")) {
        d = strtod(val, NULL); if (d >= 0.0 && d < 0.9) s_cfg.dz = d;
    } else if (!strcmp(key, "acc")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.acc = d;
    } else if (!strcmp(key, "full")) {
        l = strtol(val, NULL, 0); if (l > 0 && l < 1000) s_cfg.full = l;
    } else if (!strcmp(key, "invert_y")) {
        s_cfg.invert_y = atoi(val) != 0;
    } else if (!strcmp(key, "swap_sticks")) {
        s_cfg.swap_sticks = atoi(val) != 0;
    } else {
        return 0;
    }
    return 1;
}

static void mkb_load_config(void)
{
    static const char *kv[] = {
        "RECOMP_MKB_SENS", "sens",
        "RECOMP_MKB_DZ",   "dz",
        "RECOMP_MKB_ACC",  "acc",
        "RECOMP_MKB_FULL", "full",
        "RECOMP_MKB_INVERT_Y",  "invert_y",
        "RECOMP_MKB_SWAP_STICKS", "swap_sticks",
        NULL
    };
    char path[MAX_PATH];
    const char *ini;
    DWORD n;
    FILE *f;
    int i;

    for (i = 0; kv[i]; i += 2) {
        const char *v = getenv(kv[i]);
        if (v && *v)
            mkb_apply_kv(kv[i + 1], v);
    }

    ini = getenv("RECOMP_MKB_INI");
    if (ini && *ini) {
        snprintf(path, sizeof path, "%s", ini);
    } else {
        n = GetModuleFileNameA(NULL, path, sizeof path - 12);
        if (!n || n >= sizeof path - 12)
            return;
        snprintf(path + n, 12, "\\..\\mkb.ini");   /* beside the .exe */
    }
    f = fopen(path, "r");
    if (!f)
        return;
    {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            char *k, *v;
            if (!eq) continue;
            *eq = 0;
            k = line; v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            k[strcspn(k, " \t\r\n")] = 0;
            v[strcspn(v, " \t\r\n")] = 0;
            if (*k && *v)
                mkb_apply_kv(k, v);
        }
    }
    fclose(f);
}

double mkb_sensitivity(void)        { return s_cfg.sens; }
void   mkb_set_sensitivity(double s) { if (s > 0.0 && s < 100.0) s_cfg.sens = s; }
int    mkb_invert_y(void)           { return s_cfg.invert_y; }
void   mkb_set_invert_y(int on)     { s_cfg.invert_y = on != 0; }

/* ---- bindings ---------------------------------------------------------- */

/* Written by the menu's thread, read by the poll: one int at a time, and a
 * binding seen a poll late is a binding made a poll later. 
  * unwritt3n walk patch: added Run*/
static volatile int s_bind[MKB_ACTION_COUNT][MKB_SLOTS];

static const char *const k_action_key[MKB_ACTION_COUNT] = {
    "A", "B", "X", "Y", "Black", "White", "LeftTrigger", "RightTrigger",
    "Start", "Back", "LeftStickClick", "RightStickClick",
    "DpadUp", "DpadDown", "DpadLeft", "DpadRight",
    "MoveUp", "MoveDown", "MoveLeft", "MoveRight",
    "LookUp", "LookDown", "LookLeft", "LookRight",
    "Run",
};

const char *mkb_action_key(int action)
{
    return (unsigned)action < MKB_ACTION_COUNT ? k_action_key[action] : "";
}

int mkb_binding(int action, int slot)
{
    if ((unsigned)action >= MKB_ACTION_COUNT || (unsigned)slot >= MKB_SLOTS)
        return 0;
    return s_bind[action][slot];
}

void mkb_bind(int action, int slot, int code)
{
    int a, s;
    if ((unsigned)action >= MKB_ACTION_COUNT || (unsigned)slot >= MKB_SLOTS)
        return;
    if (code)
        for (a = 0; a < MKB_ACTION_COUNT; a++)
            for (s = 0; s < MKB_SLOTS; s++)
                if (s_bind[a][s] == code)
                    s_bind[a][s] = 0;
    s_bind[action][slot] = code;
}

void mkb_bindings_default(void)
{
    static const int k_def[MKB_ACTION_COUNT][MKB_SLOTS] = {
        [MKB_A]          = { VK_SPACE },
        [MKB_B]          = { 'E' },
        [MKB_X]          = { 'R' },
        [MKB_Y]          = { MKB_MOUSE_MIDDLE, MKB_WHEEL_UP },
        [MKB_BLACK]      = { 'F', MKB_MOUSE_X2 },
        [MKB_WHITE]      = { 'Q', MKB_MOUSE_X1 },
        [MKB_LTRIGGER]   = { MKB_MOUSE_RIGHT },
        [MKB_RTRIGGER]   = { MKB_MOUSE_LEFT },
        [MKB_START]      = { VK_RETURN, VK_ESCAPE },
        [MKB_BACK]       = { VK_TAB },
        [MKB_LTHUMB]     = { VK_LSHIFT },
        [MKB_RTHUMB]     = { VK_LCONTROL },
        [MKB_DPAD_UP]    = { VK_UP },
        [MKB_DPAD_DOWN]  = { VK_DOWN },
        [MKB_DPAD_LEFT]  = { VK_LEFT },
        [MKB_DPAD_RIGHT] = { VK_RIGHT },
        [MKB_MOVE_UP]    = { 'W' },
        [MKB_MOVE_DOWN]  = { 'S' },
        [MKB_MOVE_LEFT]  = { 'A' },
        [MKB_MOVE_RIGHT] = { 'D' },
    };
    memcpy((void *)s_bind, k_def, sizeof k_def);
}

void mkb_input_name(int code, char *buf, int size)
{
    static const char *const k_mouse[] = {
        "Mouse left", "Mouse right", "Mouse middle", "Mouse 4", "Mouse 5",
        "Wheel up", "Wheel down",
    };
    if (size <= 0)
        return;
    buf[0] = 0;
    if (!code)
        return;
    if (code >= MKB_MOUSE_LEFT && code <= MKB_WHEEL_DOWN) {
        snprintf(buf, size, "%s", k_mouse[code - MKB_MOUSE_LEFT]);
        return;
    }
    if (code > 0 && code < 256) {
        /* The layout's own name (localised, as Windows shows it). The
         * extended bit picks the navigation block over the keypad and the
         * right-hand modifiers over the left. */
        UINT sc = MapVirtualKeyA((UINT)code, MAPVK_VK_TO_VSC);
        LONG lp = (LONG)(sc << 16);
        switch (code) {
        case VK_RCONTROL: case VK_RMENU: case VK_LEFT: case VK_RIGHT:
        case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_HOME:
        case VK_END: case VK_INSERT: case VK_DELETE: case VK_DIVIDE:
        case VK_NUMLOCK: case VK_LWIN: case VK_RWIN: case VK_APPS:
            lp |= 1 << 24;
        }
        if (sc && GetKeyNameTextA(lp, buf, size) > 0) {
            if (code == VK_LSHIFT || code == VK_LCONTROL || code == VK_LMENU)
                strncat(buf, " (left)", (size_t)size - strlen(buf) - 1);
            else if (code == VK_RSHIFT || code == VK_RCONTROL || code == VK_RMENU)
                strncat(buf, " (right)", (size_t)size - strlen(buf) - 1);
            return;
        }
    }
    snprintf(buf, size, "Key 0x%02X", code);
}

/* ---- raw input thread -------------------------------------------------- */

/* Accumulators, written by the input thread and drained by the poll.
 * InterlockedExchange is the whole protocol: the input thread adds
 * with InterlockedAdd, the poll takes everything with Exchange(0). */
static volatile LONG s_dx, s_dy;
static volatile LONG s_wheel;        /* wheel steps since last poll   */
/* Mouse buttons (bit n = MKB_MOUSE_LEFT + n): held now, and pressed at
 * all since the last poll -- a click shorter than a poll still counts,
 * and a held button stays held across polls. */
static volatile LONG s_mouse_down, s_mouse_latch;
static volatile unsigned char s_vk_down[256];
static volatile LONG s_vk_latch[8];  /* 256 bits: pressed since last poll */

static volatile LONG s_active = 1;   /* F9 toggles this               */
static volatile LONG s_thread_up;
static volatile LONG s_suspended;

/* "Press a key": 0 idle, 1 waiting for everything to be released, 2 armed,
 * 3 done (s_cap_code: the input, or 0 when cancelled). */
static volatile LONG s_cap_state, s_cap_code;

/* F9 is read on the input thread, so the toggle works even while the
 * game is stalled on the poll side. */
#define MKB_TOGGLE_VK  VK_F9

static int nothing_down(void)
{
    int i;
    if (s_mouse_down)
        return 0;
    for (i = 0; i < 256; i++)
        if (s_vk_down[i])
            return 0;
    return 1;
}

/* A press, offered to a capture in progress. */
static void cap_offer(int code)
{
    if (s_cap_state != 2)
        return;
    if (code == MKB_TOGGLE_VK || code == VK_F6 || code == VK_F7 || code == VK_F8
        || code == VK_F11)
        return;                         /* the port's own keys */
    if (code == VK_DELETE || code == VK_HOME)
        code = 0;                       /* the menu keys cancel */
    InterlockedExchange(&s_cap_code, code);
    InterlockedExchange(&s_cap_state, 3);
}

static void mouse_edge(USHORT flags, USHORT down_flag, USHORT up_flag, int n)
{
    if (flags & down_flag) {
        if (!(s_mouse_down & (1 << n)))
            cap_offer(MKB_MOUSE_LEFT + n);
        InterlockedOr(&s_mouse_down, 1 << n);
        InterlockedOr(&s_mouse_latch, 1 << n);
    }
    if (flags & up_flag)
        InterlockedAnd(&s_mouse_down, ~(1 << n));
}

/* Raw input reports Shift, Ctrl and Alt as one key each; tell the sides
 * apart (the make code for Shift, the E0 prefix for the other two). */
static UINT side_vk(const RAWKEYBOARD *k)
{
    switch (k->VKey) {
    case VK_SHIFT:   return k->MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    case VK_CONTROL: return (k->Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:    return (k->Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
    default:         return k->VKey;
    }
}

static void mkb_handle_raw(HRAWINPUT h)
{
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof buf;
    RAWINPUT *ri = (RAWINPUT *)buf;

    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER))
            == (UINT)-1)
        return;

    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE *m = &ri->data.mouse;
        USHORT f = m->usButtonFlags;

        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (m->lLastX) InterlockedAdd(&s_dx, m->lLastX);
            if (m->lLastY) InterlockedAdd(&s_dy, m->lLastY);
        }
        mouse_edge(f, RI_MOUSE_LEFT_BUTTON_DOWN,   RI_MOUSE_LEFT_BUTTON_UP,   0);
        mouse_edge(f, RI_MOUSE_RIGHT_BUTTON_DOWN,  RI_MOUSE_RIGHT_BUTTON_UP,  1);
        mouse_edge(f, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2);
        mouse_edge(f, RI_MOUSE_BUTTON_4_DOWN,      RI_MOUSE_BUTTON_4_UP,      3);
        mouse_edge(f, RI_MOUSE_BUTTON_5_DOWN,      RI_MOUSE_BUTTON_5_UP,      4);
        if (f & RI_MOUSE_WHEEL) {
            SHORT d = (SHORT)m->usButtonData;
            if (d)
                cap_offer(d > 0 ? MKB_WHEEL_UP : MKB_WHEEL_DOWN);
            InterlockedAdd(&s_wheel, d / WHEEL_DELTA ? d / WHEEL_DELTA : (d > 0 ? 1 : -1));
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri->data.keyboard;
        UINT vk = side_vk(k);
        int down = k->Message == WM_KEYDOWN || k->Message == WM_SYSKEYDOWN;

        if (vk == MKB_TOGGLE_VK && down && !s_vk_down[vk]) {
            LONG now = !s_active;
            InterlockedExchange(&s_active, now);
            fprintf(stderr, "  [MKB] %s\n", now ? "captured"
                                                : "released (F9)");
            fflush(stderr);
        }
        if (vk && vk < 255) {           /* 255: a key with no VK (fake) */
            if (down) {
                if (!s_vk_down[vk])
                    cap_offer((int)vk);
                s_vk_down[vk] = 1;
                InterlockedOr(&s_vk_latch[vk >> 5], (LONG)(1UL << (vk & 31)));
            } else {
                s_vk_down[vk] = 0;
            }
        }
    }
    if (s_cap_state == 1 && nothing_down())
        InterlockedExchange(&s_cap_state, 2);
}

static LRESULT CALLBACK mkb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        mkb_handle_raw((HRAWINPUT)l);
        return 0;
    }
    if (m == WM_CLOSE || m == WM_DESTROY)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI mkb_thread(LPVOID unused)
{
    WNDCLASSA wc;
    HWND hwnd;
    RAWINPUTDEVICE rid[2];
    MSG msg;

    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = mkb_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "XboxRecompMKB";
    RegisterClassA(&wc);
    /* Not visible: it exists to own the WM_INPUT delivery, nothing else. */
    hwnd = CreateWindowExA(0, "XboxRecompMKB", "mkb", WS_OVERLAPPED,
                           0, 0, 0, 0, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        fprintf(stderr, "  [MKB] window creation failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;  rid[0].usUsage = 0x02;  /* mouse     */
    rid[1].usUsagePage = 0x01;  rid[1].usUsage = 0x06;  /* keyboard  */
    rid[0].dwFlags = RIDEV_INPUTSINK;   /* deliver without focus */
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fprintf(stderr, "  [MKB] RegisterRawInputDevices failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    InterlockedExchange(&s_thread_up, 1);
    fprintf(stderr, "  [MKB] mouse+keyboard captured (F9 releases; "
            "sens=%.2f dz=%.2f full=%ld)\n",
            s_cfg.sens, s_cfg.dz, s_cfg.full);
    fflush(stderr);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    InterlockedExchange(&s_thread_up, 0);
    return 0;
}

/* ---- capture ----------------------------------------------------------- */

void mkb_capture_begin(void)
{
    InterlockedExchange(&s_cap_code, 0);
    /* Armed at once when nothing is held (the click on the menu's button
     * has already been released); else after the next all-released. */
    InterlockedExchange(&s_cap_state, nothing_down() ? 2 : 1);
}

void mkb_capture_cancel(void)
{
    InterlockedExchange(&s_cap_code, 0);
    InterlockedExchange(&s_cap_state, 0);
}

int mkb_capture_result(void)
{
    LONG st = s_cap_state;
    if (st == 1 || st == 2)
        return -1;
    if (st == 3) {
        InterlockedExchange(&s_cap_state, 0);
        return (int)s_cap_code;
    }
    return 0;
}

/* ---- poll side --------------------------------------------------------- */

int mkb_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        /* Opt-in, like RECOMP_KEYBOARD: a keyboard silently acting as
         * player 1 is surprising when a real pad is what you meant to use.
         * RECOMP_MKB=1 turns the layer on. */
        const char *v = getenv("RECOMP_MKB");
        on = v && *v && *v != '0';
        if (on) {
            static LONG bound;
            if (InterlockedCompareExchange(&bound, 1, 0) == 0) {
                int a, s, any = 0;
                for (a = 0; a < MKB_ACTION_COUNT; a++)
                    for (s = 0; s < MKB_SLOTS; s++)
                        any |= s_bind[a][s];
                if (!any)               /* a port that set none gets Black's */
                    mkb_bindings_default();
            }
            mkb_load_config();
            if (InterlockedCompareExchange(&s_thread_up, 0, 0) == 0) {
                static LONG started = 0;
                if (InterlockedCompareExchange(&started, 1, 0) == 0)
                    CloseHandle(CreateThread(NULL, 0, mkb_thread,
                                             NULL, 0, NULL));
            }
        }
    }
    return on;
}

/* The recompiled game's process is the foreground one. Raw input with
 * INPUTSINK keeps arriving while the user is in another app, and the
 * mouse must go on behaving normally there. */
static int game_is_foreground(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Hide and confine the pointer while captured. Aiming with raw deltas
 * does not move the pointer by itself, but Windows still moves it from
 * the same physical movement, so it would wander across the screen and
 * end up clicking other apps' title bars. */
static void mkb_manage_cursor(int capture)
{
    static int hidden;          /* how many ShowCursor(FALSE) we owe  */
    static unsigned long last;

    unsigned long now = GetTickCount();
    if (now - last < 500 && (capture ? hidden : !hidden) == 0)
        return;
    last = now;

    if (capture) {
        RECT r;
        HWND fg = GetForegroundWindow();
        if (fg && GetWindowRect(fg, &r))
            ClipCursor(&r);
        if (hidden < 5) { ShowCursor(FALSE); hidden++; }
    } else {
        if (hidden > 0) { ShowCursor(TRUE); hidden--; }
        ClipCursor(NULL);
    }
}

/* Drop everything accumulated: what was moved or pressed while the pad
 * was not being read must not arrive all at once afterwards. */
/* ---- direct look ----
 * A title hook that turns the view itself (the game's own yaw and pitch,
 * not the right stick) takes the mouse's travel here, once a frame. While
 * it does, the mouse no longer deflects the stick; when it stops asking (a
 * vehicle, a camera of another kind) the stick takes the mouse back. */
static volatile LONG s_direct_tick;
#define DIRECT_LOOK_MS 1500u   /* past a loading hitch, not a vehicle ride */

int mkb_take_look(long *dx, long *dy)
{
    InterlockedExchange(&s_direct_tick, (LONG)(GetTickCount() | 1u));
    *dx = *dy = 0;
    if (!mkb_enabled() || !s_thread_up || !s_active || s_suspended || !game_is_foreground())
        return 0;
    *dx = InterlockedExchange(&s_dx, 0);
    *dy = InterlockedExchange(&s_dy, 0);
    if (s_cfg.invert_y)
        *dy = -*dy;
    return 1;
}

/* The mouse is the game's: captured, not suspended by a menu, not released
 * with F9. The pointer has no business on the screen then. */
int mkb_capturing(void)
{
    return mkb_enabled() && s_thread_up && s_active && !s_suspended;
}

static int direct_look(void)
{
    DWORD t = (DWORD)s_direct_tick;
    return t && GetTickCount() - t < DIRECT_LOOK_MS;
}

static void mkb_drain(void)
{
    int i;
    InterlockedExchange(&s_dx, 0);
    InterlockedExchange(&s_dy, 0);
    InterlockedExchange(&s_wheel, 0);
    InterlockedExchange(&s_mouse_latch, 0);
    for (i = 0; i < 8; i++)
        InterlockedExchange(&s_vk_latch[i], 0);
}

void mkb_set_suspended(int on)
{
    InterlockedExchange(&s_suspended, on != 0);
    if (on)
        ClipCursor(NULL);       /* the menu needs the whole screen */
    else
        mkb_drain();
}

/* Radial deadzone compensation: an input radius in [0..1] comes out as
 * [dz..1], so the smallest mouse correction still clears the title's
 * own deadzone, and the response stays linear above it. */
static void mkb_aim_vector(double ax, double ay, SHORT *out_x, SHORT *out_y)
{
    double r = sqrt(ax * ax + ay * ay);
    double len, fx, fy;
    long v;

    if (r <= 0.0) { *out_x = 0; *out_y = 0; return; }
    if (r > 1.0) r = 1.0;
    len = s_cfg.dz + (1.0 - s_cfg.dz) * pow(r, s_cfg.acc);
    len *= s_cfg.sens;
    if (len > 1.0) len = 1.0;
    fx = ax * len / sqrt(ax * ax + ay * ay);
    fy = ay * len / sqrt(ax * ax + ay * ay);
    v = (long)(fx * 32767.0);
    *out_x = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = (long)(fy * 32767.0);
    *out_y = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* A digital direction pair as a full stick throw, diagonals on the circle. */
static void dir_vector(int x, int y, SHORT *out_x, SHORT *out_y)
{
    double k = (x && y) ? 0.70710678 : 1.0;
    *out_x = (SHORT)(x * k * 32767.0);
    *out_y = (SHORT)(y * k * 32767.0);
}

/* The inputs seen this poll: held now or pressed since the last one. */
typedef struct {
    LONG vk[8];
    LONG mouse;
    LONG wheel;
} PollInputs;

static int code_pressed(const PollInputs *in, int code)
{
    if (code > 0 && code < 256)
        return s_vk_down[code] || ((unsigned long)in->vk[code >> 5] & (1UL << (code & 31)));
    if (code >= MKB_MOUSE_LEFT && code <= MKB_MOUSE_X2)
        return (in->mouse >> (code - MKB_MOUSE_LEFT)) & 1;
    if (code == MKB_WHEEL_UP)
        return in->wheel > 0;
    if (code == MKB_WHEEL_DOWN)
        return in->wheel < 0;
    return 0;
}

static int action_pressed(const PollInputs *in, int action)
{
    int s;
    for (s = 0; s < MKB_SLOTS; s++)
        if (s_bind[action][s] && code_pressed(in, s_bind[action][s]))
            return 1;
    return 0;
}

int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    static const BYTE k_analog[] = {        /* MKB_A..MKB_RTRIGGER */
        XBOX_BUTTON_A, XBOX_BUTTON_B, XBOX_BUTTON_X, XBOX_BUTTON_Y,
        XBOX_BUTTON_BLACK, XBOX_BUTTON_WHITE,
        XBOX_BUTTON_LTRIGGER, XBOX_BUTTON_RTRIGGER,
    };
    static const WORD k_digital[] = {       /* MKB_START..MKB_DPAD_RIGHT */
        XBOX_GAMEPAD_START, XBOX_GAMEPAD_BACK,
        XBOX_GAMEPAD_LEFT_THUMB, XBOX_GAMEPAD_RIGHT_THUMB,
        XBOX_GAMEPAD_DPAD_UP, XBOX_GAMEPAD_DPAD_DOWN,
        XBOX_GAMEPAD_DPAD_LEFT, XBOX_GAMEPAD_DPAD_RIGHT,
    };
    SHORT aim_x = 0, aim_y = 0, mov_x = 0, mov_y = 0;
    WORD b = 0;
    BYTE analog[8];
    LONG dx, dy;
    PollInputs in;
    int any = 0, i;
    static DWORD packet;

    {
        /* RECOMP_MKB_TRACE=1: which gate stops the reads, once a second. */
        static int gtrace = -1;
        static unsigned calls, g_port, g_thread, g_off, g_bg;
        static DWORD gt0;
        if (gtrace < 0) gtrace = getenv("RECOMP_MKB_TRACE") != NULL;
        if (gtrace) {
            DWORD now = GetTickCount();
            calls++;
            if (dwPort != 0) g_port++;
            else if (!s_thread_up) g_thread++;
            else if (!s_active || s_suspended) g_off++;
            else if (!game_is_foreground()) g_bg++;
            if (!gt0) gt0 = now;
            if (now - gt0 >= 1000) {
                fprintf(stderr, "  [MKB] %u calls/s: other port %u, no thread %u, off/suspended %u"
                        " (active %ld, suspended %ld), not foreground %u\n",
                        calls, g_port, g_thread, g_off, (long)s_active, (long)s_suspended, g_bg);
                calls = g_port = g_thread = g_off = g_bg = 0;
                gt0 = now;
            }
        }
    }
    if (!pState || dwPort != 0 || !mkb_enabled())
        return 0;
    if (!s_thread_up)
        return 0;

    if (!s_active || s_suspended) {
        mkb_manage_cursor(0);
        mkb_drain();
        return 0;
    }

    /* Foreground gate: while the user is elsewhere the mouse belongs to
     * whoever has it, and the game should read a pad at rest. */
    if (!game_is_foreground()) {
        mkb_manage_cursor(0);
        mkb_drain();
        return 0;
    }
    mkb_manage_cursor(1);

    /* ---- the mouse's aim, from its speed ----
     * The pad is read far more often than the title looks at it (USB
     * reports, ~340 a second, against one look a frame), and draining the
     * mouse on every read left most reads -- and so most of the title's
     * looks -- at rest: the camera barely turned. Every AIM_WINDOW_MS the
     * travel since the last window becomes a deflection, held for every read
     * until the next one: proportional to the mouse's speed, whatever the
     * read rate. s_cfg.full stays "counts for a full throw in a 60 Hz frame". */
    {
#define AIM_WINDOW_MS 8.0
        static LARGE_INTEGER t_last, freq;
        static SHORT cur_x, cur_y;
        static int trace = -1;
        static unsigned windows, moved;
        static double trace_ms;
        LARGE_INTEGER now;
        double ms;

        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        ms = t_last.QuadPart
           ? (double)(now.QuadPart - t_last.QuadPart) * 1000.0 / (double)freq.QuadPart : 1e9;
        if (direct_look()) {
            cur_x = cur_y = 0;              /* the title's hook has the mouse */
        } else if (ms >= AIM_WINDOW_MS) {
            /* After a long gap (a load, a pause) the travel is not a speed. */
            double per_frame = ms > 100.0 ? 1.0 : (1000.0 / 60.0) / ms;
            dx = InterlockedExchange(&s_dx, 0);
            dy = InterlockedExchange(&s_dy, 0);
            cur_x = cur_y = 0;
            if (dx || dy) {
                double ax =  (double)dx * per_frame / (double)s_cfg.full;
                double ay = -(double)dy * per_frame / (double)s_cfg.full;
                if (s_cfg.invert_y) ay = -ay;
                mkb_aim_vector(ax, ay, &cur_x, &cur_y);
            }
            t_last = now;

            /* RECOMP_MKB_TRACE=1: windows a second, and how many moved. */
            if (trace < 0) trace = getenv("RECOMP_MKB_TRACE") != NULL;
            if (trace) {
                windows++;
                if (dx || dy) moved++;
                trace_ms += ms > 100.0 ? 0.0 : ms;
                if (trace_ms >= 1000.0) {
                    fprintf(stderr, "  [MKB] %u aim windows/s, %u with movement\n", windows, moved);
                    windows = moved = 0;
                    trace_ms = 0.0;
                }
            }
        }
        aim_x = cur_x;
        aim_y = cur_y;
    }
    in.wheel = InterlockedExchange(&s_wheel, 0);
    in.mouse = InterlockedExchange(&s_mouse_latch, 0) | s_mouse_down;
    for (i = 0; i < 8; i++)
        in.vk[i] = InterlockedExchange(&s_vk_latch[i], 0);

    memset(analog, 0, sizeof analog);

    /* ---- buttons, as the table binds them ---- */
    for (i = 0; i < (int)sizeof k_analog; i++)
        if (action_pressed(&in, MKB_A + i))
            analog[k_analog[i]] = 255;
    for (i = 0; i < (int)(sizeof k_digital / sizeof k_digital[0]); i++)
        if (action_pressed(&in, MKB_START + i))
            b |= k_digital[i];

   /* ---- unwritt3n walk patch: walk by default, hold Left Shift to run ---- */
    {
        int mx = action_pressed(&in, MKB_MOVE_RIGHT) - action_pressed(&in, MKB_MOVE_LEFT);
        int my = action_pressed(&in, MKB_MOVE_UP) - action_pressed(&in, MKB_MOVE_DOWN);
        if (mx || my) {
            if (action_pressed(&in, MKB_RUN)) {
                /* Full square throw so diagonal sprinting keeps 100% forward axis */
                mov_x = (SHORT)(mx * 32767);
                mov_y = (SHORT)(my * 32767);
            } else {
                /* Walking: circular throw scaled to 65%, boosted on diagonals to clear deadzone */
                double walk_scale = (mx && my) ? (0.65 * 1.25) : 0.65;
                dir_vector(mx, my, &mov_x, &mov_y);
                mov_x = (SHORT)(mov_x * walk_scale);
                mov_y = (SHORT)(mov_y * walk_scale);
            }
        }
    }

    /* ---- aim: the mouse (above), else the look keys ---- */
    if (!aim_x && !aim_y) {
        int lx = action_pressed(&in, MKB_LOOK_RIGHT) - action_pressed(&in, MKB_LOOK_LEFT);
        int ly = action_pressed(&in, MKB_LOOK_UP) - action_pressed(&in, MKB_LOOK_DOWN);
        if (lx || ly) dir_vector(lx, ly, &aim_x, &aim_y);
    }

    /* ---- merge on top of whatever the host pad produced ---- */
    if (b)        { pState->Gamepad.wButtons |= b; any = 1; }
    for (i = 0; i < 8; i++) {
        if (analog[i]) {
            if (analog[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = analog[i];
            any = 1;
        }
    }
    if (aim_x || aim_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbLX = aim_x;
            pState->Gamepad.sThumbLY = aim_y;
        } else {
            pState->Gamepad.sThumbRX = aim_x;
            pState->Gamepad.sThumbRY = aim_y;
        }
        any = 1;
    }
    if (mov_x || mov_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbRX = mov_x;
            pState->Gamepad.sThumbRY = mov_y;
        } else {
            pState->Gamepad.sThumbLX = mov_x;
            pState->Gamepad.sThumbLY = mov_y;
        }
        any = 1;
    }

    /* Edge detection: a title polling for changes ignores a state whose
     * packet number did not move. */
    if (any)
        pState->dwPacketNumber = ++packet;
    return any;
}
