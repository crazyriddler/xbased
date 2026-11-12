/*
 * hakuX front end — controller mapping (input_map.h).
 */
#include "native/app/ui/input_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_name[IM_INPUTS] = {"a", "b", "x", "y", "black", "white", "lt", "rt", "start", "back",
                                              "ls", "rs", "up", "down", "left", "right", "lup", "ldown", "lleft",
                                              "lright", "rup", "rdown", "rleft", "rright"};
static const char *const k_label[IM_INPUTS] = {
    "A", "B", "X", "Y", "Black", "White", "Left trigger", "Right trigger", "Start", "Back", "Left stick (click)",
    "Right stick (click)", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left stick up", "Left stick down",
    "Left stick left", "Left stick right", "Right stick up", "Right stick down", "Right stick left",
    "Right stick right"};
/* the keyboard's defaults (player 1) */
static const SDL_Keycode k_kb_def[IM_INPUTS] = {
    SDLK_SPACE, SDLK_LSHIFT, SDLK_Q, SDLK_E, SDLK_C, SDLK_Z, SDLK_1, SDLK_3, SDLK_RETURN, SDLK_BACKSPACE, SDLK_F, SDLK_H,
    SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_W, SDLK_S, SDLK_A, SDLK_D, SDLK_I, SDLK_K, SDLK_J, SDLK_L};
/* the gamepad's: 1 + button, 1000 + axis * 2 + negative */
#define B(x) (1 + (x))
#define AX(a, neg) (1000 + (a) * 2 + (neg))
static const int k_pad_def[IM_INPUTS] = {
    B(SDL_GAMEPAD_BUTTON_SOUTH), B(SDL_GAMEPAD_BUTTON_EAST), B(SDL_GAMEPAD_BUTTON_WEST), B(SDL_GAMEPAD_BUTTON_NORTH),
    B(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), B(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), AX(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0),
    AX(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0), B(SDL_GAMEPAD_BUTTON_START), B(SDL_GAMEPAD_BUTTON_BACK),
    B(SDL_GAMEPAD_BUTTON_LEFT_STICK), B(SDL_GAMEPAD_BUTTON_RIGHT_STICK), B(SDL_GAMEPAD_BUTTON_DPAD_UP),
    B(SDL_GAMEPAD_BUTTON_DPAD_DOWN), B(SDL_GAMEPAD_BUTTON_DPAD_LEFT), B(SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
    AX(SDL_GAMEPAD_AXIS_LEFTY, 1), AX(SDL_GAMEPAD_AXIS_LEFTY, 0), AX(SDL_GAMEPAD_AXIS_LEFTX, 1),
    AX(SDL_GAMEPAD_AXIS_LEFTX, 0), AX(SDL_GAMEPAD_AXIS_RIGHTY, 1), AX(SDL_GAMEPAD_AXIS_RIGHTY, 0),
    AX(SDL_GAMEPAD_AXIS_RIGHTX, 1), AX(SDL_GAMEPAD_AXIS_RIGHTX, 0)};
static const char *const k_hk_name[IM_HOTKEYS] = {"menu", "save", "load", "turbo", "pause", "shot", "full"};
static const SDL_Keycode k_hk_def[IM_HOTKEYS] = {SDLK_F1, SDLK_F5, SDLK_F8, SDLK_TAB, SDLK_F2, SDLK_F12, SDLK_F11};
static const enum UiStr k_hk_label[IM_HOTKEYS] = {S_HK_MENU, S_HK_SAVE, S_HK_LOAD, S_HK_TURBO,
                                                  S_HK_PAUSE, S_HK_SHOT, S_HK_FULL};

const char *im_input_label(int i)
{
    /* the face buttons and Start/Back as the controller prints them; the rest in the interface's language */
    static char buf[96];
    static const enum UiStr k_dir[4] = {S_DIR_UP, S_DIR_DOWN, S_DIR_LEFT, S_DIR_RIGHT};
    switch (i) {
    case IM_BLACK: return T(S_IN_BLACK);
    case IM_WHITE: return T(S_IN_WHITE);
    case IM_LT: return T(S_IN_LT);
    case IM_RT: return T(S_IN_RT);
    case IM_LS: return T(S_IN_LS);
    case IM_RS: return T(S_IN_RS);
    default: break;
    }
    if (i >= IM_DUP && i <= IM_DRIGHT) {
        snprintf(buf, sizeof(buf), "%s %s", T(S_IN_DPAD), T(k_dir[i - IM_DUP]));
        return buf;
    }
    if (i >= IM_LUP && i <= IM_LRIGHT) {
        snprintf(buf, sizeof(buf), "%s %s", T(S_IN_LSTICK), T(k_dir[i - IM_LUP]));
        return buf;
    }
    if (i >= IM_RUP && i <= IM_RRIGHT) {
        snprintf(buf, sizeof(buf), "%s %s", T(S_IN_RSTICK), T(k_dir[i - IM_RUP]));
        return buf;
    }
    return k_label[i];
}

enum UiStr im_hotkey_label(int h)
{
    return k_hk_label[h];
}

static void map_key(int player, int input, char *out, size_t n)
{
    snprintf(out, n, "map.p%d.%s", player + 1, k_name[input]);
}

/* "<key name>|<pad element>" -> its parts, the defaults where absent */
static void parse(const Config *c, int player, int input, SDL_Keycode *key, int *pad)
{
    char k[48];
    map_key(player, input, k, sizeof(k));
    const char *v = cfg_get(c, k);
    *key = player == 0 ? k_kb_def[input] : SDLK_UNKNOWN;
    *pad = k_pad_def[input];
    if (!v) {
        return;
    }
    char kb[64];
    const char *bar = strchr(v, '|');
    snprintf(kb, sizeof(kb), "%.*s", bar ? (int)(bar - v) : (int)strlen(v), v);
    *key = kb[0] ? SDL_GetKeyFromName(kb) : SDLK_UNKNOWN;
    const char *p = bar ? bar + 1 : "";
    if (p[0] == 'b') {
        *pad = 1 + atoi(p + 1);
    } else if (p[0] == 'a') {
        const int a = atoi(p + 1);
        *pad = 1000 + a * 2 + (strchr(p, '-') != NULL);
    } else {
        *pad = 0;
    }
}

static void store(Config *c, int player, int input, SDL_Keycode key, int pad)
{
    char k[48], v[96], pe[16] = "";
    map_key(player, input, k, sizeof(k));
    if (pad >= 1000) {
        snprintf(pe, sizeof(pe), "a%d%c", (pad - 1000) / 2, (pad & 1) ? '-' : '+');
    } else if (pad > 0) {
        snprintf(pe, sizeof(pe), "b%d", pad - 1);
    }
    snprintf(v, sizeof(v), "%s|%s", key ? SDL_GetKeyName(key) : "", pe);
    cfg_set(c, k, v);
}

/* a pad's input as its face says it: A, LB, RT, Left stick X+, D-pad up */
static void pad_name(int pad, char *out, size_t n)
{
    static const char *const buttons[] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "LS", "RS", "LB", "RB",
                                          "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
    static const char *const axes[] = {"Left stick X", "Left stick Y", "Right stick X", "Right stick Y", "LT", "RT"};
    if (!pad) {
        snprintf(out, n, "-");
    } else if (pad >= 1000) {
        const int a = (pad - 1000) / 2;
        const char *nm = a < 6 ? axes[a] : SDL_GetGamepadStringForAxis((SDL_GamepadAxis)a);
        snprintf(out, n, "%s%s", nm ? nm : "axis", a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "" : (pad & 1) ? "-" : "+");
    } else {
        const int b = pad - 1;
        const char *nm = b < 15 ? buttons[b] : SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
        snprintf(out, n, "%s", nm ? nm : "?");
    }
}

void im_binding_name(const Config *c, int player, int input, char *out, size_t n)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    char pn[48];
    pad_name(p, pn, sizeof(pn));
    snprintf(out, n, "%s  /  %s", k ? SDL_GetKeyName(k) : "-", pn);
}

void im_bind_key(Config *c, int player, int input, SDL_Keycode key)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    store(c, player, input, key, p);
}

void im_bind_button(Config *c, int player, int input, int button)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    store(c, player, input, k, 1 + button);
}

void im_bind_axis(Config *c, int player, int input, int sa)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    const int a = (sa < 0 ? -sa : sa) - 1;
    store(c, player, input, k, 1000 + a * 2 + (sa < 0));
}

void im_unbind(Config *c, int player, int input)
{
    store(c, player, input, SDLK_UNKNOWN, 0);
}

void im_clear_map(Config *c, int player)
{
    for (int i = 0; i < IM_INPUTS; i++) {
        char k[48];
        map_key(player, i, k, sizeof(k));
        cfg_unset(c, k);
    }
}

void im_copy_map(Config *to, const Config *from, int player)
{
    for (int i = 0; i < IM_INPUTS; i++) {
        SDL_Keycode k;
        int p;
        parse(from, player, i, &k, &p);
        store(to, player, i, k, p);
    }
}

static int device_of(const Config *c, int player)
{
    char k[16];
    snprintf(k, sizeof(k), "pad.p%d", player + 1);
    const char *v = cfg_get(c, k);
    if (!v || !strcmp(v, "auto")) {
        return 0;
    }
    if (!strcmp(v, "none")) {
        return -2;
    }
    if (!strcmp(v, "keyboard")) {
        return -1;
    }
    if (!strcmp(v, "all")) {
        return -3;
    }
    return !strncmp(v, "pad", 3) ? atoi(v + 3) : 0;
}

void im_device_name(const Config *c, int player, char *out, size_t n)
{
    const int d = device_of(c, player);
    if (d == -2) {
        snprintf(out, n, "%s", T(S_NONE));
    } else if (d == -3) {
        snprintf(out, n, "%s", T(S_DEV_ALL));
    } else if (d == -1) {
        snprintf(out, n, "%s", T(S_KEYBOARD));
    } else if (d == 0) {
        snprintf(out, n, "%s", T(S_AUTO));
    } else {
        int cnt = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&cnt);
        const char *nm = ids && d <= cnt ? SDL_GetGamepadNameForID(ids[d - 1]) : NULL;
        snprintf(out, n, "%d: %s", d, nm ? nm : "-");
        SDL_free(ids);
    }
}

void im_cycle_device(Config *c, int player, int delta)
{
    int cnt = 0;
    SDL_free(SDL_GetGamepads(&cnt));
    /* auto, keyboard, keyboard + mouse + controllers, pad1..padN, none */
    const int slots = cnt + 4;
    const int d = device_of(c, player);
    int pos = d == 0 ? 0 : d == -1 ? 1 : d == -3 ? 2 : d == -2 ? slots - 1 : 2 + d;
    pos = (pos + delta + slots) % slots;
    char k[16], v[16];
    snprintf(k, sizeof(k), "pad.p%d", player + 1);
    if (pos == 0) {
        snprintf(v, sizeof(v), "auto");
    } else if (pos == 1) {
        snprintf(v, sizeof(v), "keyboard");
    } else if (pos == 2) {
        snprintf(v, sizeof(v), "all");
    } else if (pos == slots - 1) {
        snprintf(v, sizeof(v), "none");
    } else {
        snprintf(v, sizeof(v), "pad%d", pos - 2);
    }
    cfg_set(c, k, v);
}

void im_hotkey_name(const Config *c, int h, char *out, size_t n)
{
    char k[24];
    snprintf(k, sizeof(k), "hk.%s", k_hk_name[h]);
    const char *v = cfg_get(c, k);
    snprintf(out, n, "%s", v && *v ? v : SDL_GetKeyName(k_hk_def[h]));
}

void im_set_hotkey(Config *c, int h, SDL_Keycode key, SDL_Keymod mod)
{
    char k[24], v[96];
    snprintf(k, sizeof(k), "hk.%s", k_hk_name[h]);
    snprintf(v, sizeof(v), "%s%s%s%s", (mod & SDL_KMOD_CTRL) ? "Ctrl+" : "", (mod & SDL_KMOD_ALT) ? "Alt+" : "",
             (mod & SDL_KMOD_SHIFT) ? "Shift+" : "", SDL_GetKeyName(key));
    cfg_set(c, k, v);
}

/* "Ctrl+Alt+Shift+F5": the key, and the modifiers (1 Ctrl, 2 Alt, 4 Shift) */
static SDL_Keycode hotkey_parse(const char *v, uint8_t *mod)
{
    static const struct { const char *p; uint8_t bit; } k_mods[] = {{"Ctrl+", 1}, {"Alt+", 2}, {"Shift+", 4}};
    *mod = 0;
    for (bool more = true; more;) {
        more = false;
        for (unsigned i = 0; i < 3; i++) {
            const size_t n = strlen(k_mods[i].p);
            if (!SDL_strncasecmp(v, k_mods[i].p, n) && v[n]) {
                *mod |= k_mods[i].bit;
                v += n;
                more = true;
            }
        }
    }
    return SDL_GetKeyFromName(v);
}

/* the controller's choices for a shortcut */
static const struct { const char *v, *label; int code; } k_hkp[] = {
    {"none", NULL, IM_HKP_NONE},
    {"guide", "Guide", IM_HKP_GUIDE},
    {"touchpad", "Touchpad", IM_HKP_TOUCHPAD},
    {"share", "Share", IM_HKP_SHARE},
    {"guide+a", "Guide + A", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_SOUTH},
    {"guide+b", "Guide + B", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_EAST},
    {"guide+x", "Guide + X", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_WEST},
    {"guide+y", "Guide + Y", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_NORTH},
    {"guide+lb", "Guide + LB", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"guide+rb", "Guide + RB", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"guide+back", "Guide + Back", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_BACK},
    {"guide+start", "Guide + Start", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_START},
    {"guide+ls", "Guide + LS", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"guide+rs", "Guide + RS", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"guide+up", "Guide + D-pad up", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"guide+down", "Guide + D-pad down", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"guide+left", "Guide + D-pad left", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"guide+right", "Guide + D-pad right", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"back+a", "Back + A", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_SOUTH},
    {"back+b", "Back + B", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_EAST},
    {"back+x", "Back + X", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_WEST},
    {"back+y", "Back + Y", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_NORTH},
    {"back+lb", "Back + LB", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"back+rb", "Back + RB", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"back+ls", "Back + LS", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"back+rs", "Back + RS", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"back+up", "Back + D-pad up", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"back+down", "Back + D-pad down", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"back+left", "Back + D-pad left", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"back+right", "Back + D-pad right", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};
/* the menu keeps the Guide button's tap (it always opened it), a
 * screenshot the Share button's; the rest none until set */
static const char *const k_hkp_def[IM_HOTKEYS] = {"guide", "none", "none", "none", "none", "share", "none"};

int im_hkpad_count(void)
{
    return (int)(sizeof(k_hkp) / sizeof(k_hkp[0]));
}

const char *im_hkpad_value(int i)
{
    return k_hkp[i].v;
}

const char *im_hkpad_label(int i)
{
    return k_hkp[i].label ? k_hkp[i].label : T(S_NONE);
}

const char *im_hkpad_default(int h)
{
    return k_hkp_def[h];
}

static int hkpad_code(const char *v)
{
    for (int i = 0; v && i < im_hkpad_count(); i++) {
        if (!SDL_strcasecmp(v, k_hkp[i].v)) {
            return k_hkp[i].code;
        }
    }
    return IM_HKP_NONE;
}

void im_resolve(ImMap *m, const Config *global, const Config *game)
{
    memset(m, 0, sizeof(*m));
    const bool custom = game && cfg_bool(game, "map.custom", false);
    for (int p = 0; p < 4; p++) {
        m->device[p] = device_of(global, p);
        for (int i = 0; i < IM_INPUTS; i++) {
            parse(custom ? game : global, p, i, &m->key[p][i], &m->pad[p][i]);
        }
    }
    for (int h = 0; h < IM_HOTKEYS; h++) {
        char k[24];
        snprintf(k, sizeof(k), "hk.%s", k_hk_name[h]);
        const char *v = cfg_get(global, k);
        m->hk_mod[h] = 0;
        m->hotkey[h] = v && *v ? hotkey_parse(v, &m->hk_mod[h]) : k_hk_def[h];
        snprintf(k, sizeof(k), "hkpad.%s", k_hk_name[h]);
        const char *pv = cfg_get(global, k);
        m->hk_pad[h] = hkpad_code(pv ? pv : k_hkp_def[h]);
    }
    const Config *dz = game && cfg_get(game, "deadzone") ? game : global;
    m->deadzone = cfg_int(dz, "deadzone", 10) / 100.0f;
    const Config *ad = game && cfg_get(game, "stick.antidz") ? game : global;
    m->stick_antidz = cfg_int(ad, "stick.antidz", 0) / 100.0f;
    const Config *sr = game && cfg_get(game, "stick.range") ? game : global;
    m->stick_range = cfg_int(sr, "stick.range", 100) / 100.0f;
    const Config *ss = game && cfg_get(game, "stick.shape") ? game : global;
    const char *shape = cfg_get(ss, "stick.shape");
    m->stick_round = shape && !strcmp(shape, "round");
    const Config *td = game && cfg_get(game, "trigger.deadzone") ? game : global;
    m->trigger_dz = cfg_int(td, "trigger.deadzone", 10) / 100.0f;
    const Config *tf = game && cfg_get(game, "trigger.full") ? game : global;
    m->trigger_full = cfg_int(tf, "trigger.full", 100) / 100.0f;
    m->rumble = cfg_bool(global, "rumble", true);
    m->rumble_scale = cfg_int(global, "rumble.str", 100) / 100.0f;
    m->quick = cfg_bool(global, "quick.btn", true);
    {
        const char *q = cfg_get(global, "quick.combo");
        m->quick_combo = !q ? 0 : !strcmp(q, "triggers") ? 1 : !strcmp(q, "sticks") ? 2 : !strcmp(q, "dpad") ? 3 : 0;
    }
    /* the mouse: the game's own settings where its controls are its own, the global ones otherwise */
    const Config *mc[2] = {custom ? game : global, global};
#define MOUSE_CFG(key) (cfg_get(mc[0], key) ? mc[0] : mc[1])
    m->mouse = cfg_bool(MOUSE_CFG("mouse"), "mouse", false);
    {
        const char *s = cfg_get(MOUSE_CFG("mouse.stick"), "mouse.stick");
        m->mouse_stick = s && !strcmp(s, "left");
    }
    m->mouse_sens = cfg_int(MOUSE_CFG("mouse.sens"), "mouse.sens", 100) / 100.0f;
    m->mouse_sens = m->mouse_sens < 0.25f ? 0.25f : m->mouse_sens > 3.0f ? 3.0f : m->mouse_sens;
    m->mouse_invert = cfg_bool(MOUSE_CFG("mouse.invert"), "mouse.invert", false);
    for (int i = 0; i < IM_MOUSE_BTNS; i++) {
        char k[16];
        snprintf(k, sizeof(k), "mouse.b%d", i);
        const int t = cfg_int(MOUSE_CFG(k), k, im_mouse_btn_default(i));
        m->mouse_btn[i] = t < 0 || t >= IM_MOUSE_TARGETS ? 0 : t;
    }
#undef MOUSE_CFG
}

static float pad_value(SDL_Gamepad *g, int pad)
{
    if (!g || !pad) {
        return 0;
    }
    if (pad < 1000) {
        return SDL_GetGamepadButton(g, (SDL_GamepadButton)(pad - 1)) ? 1.0f : 0.0f;
    }
    const float v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)((pad - 1000) / 2)) / 32767.0f;
    return (pad & 1) ? (v < 0 ? -v : 0) : (v > 0 ? v : 0);
}

/* A stick as the Duke gave it. Today's sticks move in a circle: pushed fully
 * diagonal they read about 0.7 on each axis, where the Duke's read nearly
 * full on both, and its games were tuned to that (Tony Hawk's Pro Skater
 * 2x's tricks, a direction and a button, took a diagonal for a straight
 * one, or none). So the dead zone is a circle around the centre, not a band
 * along each axis (that cut diagonals further), and the circle is stretched
 * onto the square: full deflection is full on both axes at any angle (round:
 * kept a circle). The range scales the result, for a stick that doesn't
 * reach its edge (above 100%) or games too quick to it (below). Keys
 * already give the square's corners and stay as they are. */
void im_stick(float x, float y, float dz, float antidz, float range, bool round, float *ox, float *oy)
{
    const float r = sqrtf(x * x + y * y);
    *ox = *oy = 0;
    if (r <= dz || r <= 0) {
        return;
    }
    float len = (r - dz) / (1 - dz);
    len = len > 1 ? 1 : len;
    /* the anti dead zone: past its own dead zone the stick starts at antidz,
     * not at 0, so a game's dead zone of that size is crossed at once (many
     * Xbox games ignore the first quarter or more of the travel themselves,
     * made for the Duke's loose sticks) */
    len = (antidz + len * (1 - antidz)) * range;
    const float ux = x / r, uy = y / r;
    const float m = round ? 1 : fabsf(ux) > fabsf(uy) ? fabsf(ux) : fabsf(uy);
    const float sx = ux / m * len, sy = uy / m * len;
    *ox = sx > 1 ? 1 : sx < -1 ? -1 : sx;
    *oy = sy > 1 ? 1 : sy < -1 ? -1 : sy;
}

/* A worn trigger that stops short of its end still reaches 255 (full), and
 * the travel past the dead zone is spread over the whole range, not cut off
 * (the old fixed 10% jumped from 0 to 26). Keys give 1, a full press. */
float im_trigger(float a, float dz, float full)
{
    if (a <= dz) {
        return 0;
    }
    if (full <= dz || a >= full) {
        return 1;
    }
    return (a - dz) / (full - dz);
}

static void stick(const ImMap *m, float xpos, float xneg, float ypos, float yneg, int16_t *ox, int16_t *oy)
{
    float x, y;
    im_stick(xpos - xneg, ypos - yneg, m->deadzone, m->stick_antidz, m->stick_range, m->stick_round, &x, &y);
    *ox = (int16_t)(x * 32767);
    *oy = (int16_t)(y * 32767);
}

void im_read(const ImMap *m, int p, const bool *keys, SDL_Gamepad *pad, ImPad *out)
{
    float v[IM_INPUTS];
    for (int i = 0; i < IM_INPUTS; i++) {
        const SDL_Keycode k = m->key[p][i];
        float kv = 0;
        if (keys && k) {
            const SDL_Scancode sc = SDL_GetScancodeFromKey(k, NULL);
            kv = keys[sc] ? 1.0f : 0.0f;
        }
        const float pv = pad_value(pad, m->pad[p][i]);
        v[i] = kv > pv ? kv : pv;
    }
    memset(out, 0, sizeof(*out));
    static const int k_bits[] = {IM_DUP, IM_DDOWN, IM_DLEFT, IM_DRIGHT, IM_START, IM_BACK, IM_LS, IM_RS};
    for (int b = 0; b < 8; b++) {
        if (v[k_bits[b]] > 0.5f) {
            out->buttons |= (uint16_t)(1u << b);
        }
    }
    for (int i = 0; i < 8; i++) {
        float a = v[IM_A + i];
        if (IM_A + i == IM_LT || IM_A + i == IM_RT) {
            a = im_trigger(a, m->trigger_dz, m->trigger_full);
            out->analog[i] = (uint8_t)(a >= 1 ? 255 : a * 255);
            continue;
        }
        out->analog[i] = (uint8_t)(a >= 1 ? 255 : a <= 0.1f ? 0 : a * 255);
    }
    stick(m, v[IM_LRIGHT], v[IM_LLEFT], v[IM_LUP], v[IM_LDOWN], &out->lx, &out->ly);
    stick(m, v[IM_RRIGHT], v[IM_RLEFT], v[IM_RUP], v[IM_RDOWN], &out->rx, &out->ry);
}

/* ------------------------------------------------------------ the mouse and the layouts */

const char *im_mouse_button_label(int i)
{
    static const char *const n[IM_MOUSE_BTNS] = {"Left click", "Right click", "Middle click", "Side button 1",
                                                 "Side button 2", "Wheel up", "Wheel down"};
    return n[i < 0 ? 0 : i >= IM_MOUSE_BTNS ? IM_MOUSE_BTNS - 1 : i];
}

const char *im_mouse_target_label(int t)
{
    static const char *const n[IM_MOUSE_TARGETS] = {"Nothing", "A", "B", "X", "Y", "Back", "Start", "Left stick (click)",
                                                    "Right stick (click)", "White", "Black", "D-pad up", "D-pad down",
                                                    "D-pad left", "D-pad right", "Left trigger", "Right trigger"};
    return n[t < 0 ? 0 : t >= IM_MOUSE_TARGETS ? 0 : t];
}

/* Left click is the right trigger, right click the left, middle the right stick's click (the shooters') */
int im_mouse_btn_default(int i)
{
    static const int d[IM_MOUSE_BTNS] = {16, 15, 8, 0, 0, 0, 0};
    return d[i < 0 || i >= IM_MOUSE_BTNS ? 0 : i];
}

const char *im_preset_label(int preset)
{
    switch (preset) {
    case IM_PRESET_SHOOTER: return "Shooter or action (WASD + mouse)";
    case IM_PRESET_RACING: return "Racing";
    case IM_PRESET_FIGHTING: return "Fighting";
    case IM_PRESET_KEYBOARD: return "Sports and platformers (keyboard only)";
    default: return "xbased's own keys";
    }
}

/* The layouts as keys by input (SDL scancodes: A=4 .. Z=29, 1-4 = 30-33, Return 40, Backspace 42, Tab 43,
 * Space 44, arrows right 79 left 80 down 81 up 82, Left Shift 225). */
static void preset_keys(int preset, int sc[IM_INPUTS], bool *mouse)
{
    memset(sc, 0, IM_INPUTS * sizeof(int));
    *mouse = false;
    /* the keyboard's defaults to start from, as scancodes */
    for (int i = 0; i < IM_INPUTS; i++) {
        sc[i] = k_kb_def[i] ? (int)SDL_GetScancodeFromKey(k_kb_def[i], NULL) : 0;
    }
    int *k = sc;
    switch (preset) {
    case IM_PRESET_SHOOTER:
        k[IM_A] = 44; k[IM_B] = 8; k[IM_X] = 21; k[IM_Y] = 20;      /* Space E R Q */
        k[IM_BACK] = 42; k[IM_START] = 40;                          /* Backspace Return (Tab is the turbo hotkey) */
        k[IM_LS] = 225; k[IM_RS] = 25;                              /* Left Shift, V */
        k[IM_WHITE] = 10; k[IM_BLACK] = 9;                          /* G F */
        k[IM_DUP] = 30; k[IM_DDOWN] = 31; k[IM_DLEFT] = 32; k[IM_DRIGHT] = 33;   /* 1-4 */
        k[IM_LUP] = 26; k[IM_LLEFT] = 4; k[IM_LRIGHT] = 7; k[IM_LDOWN] = 22;     /* W A D S */
        k[IM_LT] = 29; k[IM_RT] = 27;                               /* Z X */
        k[IM_RUP] = 82; k[IM_RLEFT] = 80; k[IM_RRIGHT] = 79; k[IM_RDOWN] = 81;   /* arrows */
        *mouse = true;
        break;
    case IM_PRESET_RACING:      /* A / D steer, W gas, S brake */
        k[IM_LUP] = 0; k[IM_LDOWN] = 0; k[IM_LLEFT] = 4; k[IM_LRIGHT] = 7;
        k[IM_RT] = 26; k[IM_LT] = 22;
        k[IM_A] = 44; k[IM_B] = 5; k[IM_X] = 27; k[IM_Y] = 28;
        k[IM_WHITE] = 20; k[IM_BLACK] = 8;                          /* Q E: shift down, up */
        k[IM_START] = 40; k[IM_BACK] = 42;
        k[IM_RUP] = 82; k[IM_RLEFT] = 80; k[IM_RRIGHT] = 79; k[IM_RDOWN] = 81;
        k[IM_DUP] = 30; k[IM_DDOWN] = 31; k[IM_DLEFT] = 32; k[IM_DRIGHT] = 33;
        k[IM_LS] = 225; k[IM_RS] = 25;
        break;
    case IM_PRESET_FIGHTING:    /* the d-pad on WASD, the buttons on the right hand */
        k[IM_DUP] = 26; k[IM_DLEFT] = 4; k[IM_DRIGHT] = 7; k[IM_DDOWN] = 22;
        k[IM_A] = 13; k[IM_B] = 14; k[IM_X] = 24; k[IM_Y] = 12;     /* J K U I */
        k[IM_WHITE] = 18; k[IM_BLACK] = 15;                         /* O L */
        k[IM_LT] = 20; k[IM_RT] = 8;                                /* Q E */
        k[IM_LUP] = 82; k[IM_LLEFT] = 80; k[IM_LRIGHT] = 79; k[IM_LDOWN] = 81;
        k[IM_RUP] = k[IM_RLEFT] = k[IM_RRIGHT] = k[IM_RDOWN] = 0;
        k[IM_START] = 40; k[IM_BACK] = 42;
        break;
    case IM_PRESET_KEYBOARD:    /* arrows and Z X C V */
        k[IM_LUP] = 82; k[IM_LLEFT] = 80; k[IM_LRIGHT] = 79; k[IM_LDOWN] = 81;
        k[IM_A] = 29; k[IM_B] = 27; k[IM_X] = 6; k[IM_Y] = 25;
        k[IM_WHITE] = 4; k[IM_BLACK] = 22;                          /* A S */
        k[IM_LT] = 20; k[IM_RT] = 8;                                /* Q E */
        k[IM_DUP] = 30; k[IM_DDOWN] = 31; k[IM_DLEFT] = 32; k[IM_DRIGHT] = 33;
        k[IM_LS] = 225; k[IM_RS] = 0;
        k[IM_START] = 40; k[IM_BACK] = 42;
        k[IM_RUP] = 12; k[IM_RLEFT] = 13; k[IM_RRIGHT] = 15; k[IM_RDOWN] = 14;   /* I J L K */
        break;
    default:
        break;
    }
}

void im_apply_preset(Config *c, int player, int preset)
{
    if (preset == IM_PRESET_NONE) {
        im_clear_map(c, player);
        return;
    }
    int sc[IM_INPUTS];
    bool mouse;
    preset_keys(preset, sc, &mouse);
    for (int i = 0; i < IM_INPUTS; i++) {
        SDL_Keycode k;
        int p;
        parse(c, player, i, &k, &p);
        store(c, player, i, sc[i] > 0 ? SDL_GetKeyFromScancode((SDL_Scancode)sc[i], SDL_KMOD_NONE, false) : SDLK_UNKNOWN, p);
    }
    if (player == 0) {
        cfg_set(c, "mouse", mouse ? "1" : "0");
        if (mouse) {
            cfg_set(c, "mouse.stick", "right");
        }
    }
}

void im_apply_wasd_mouse(Config *c)
{
    im_apply_preset(c, 0, IM_PRESET_SHOOTER);
    /* the keyboard has to be in play for player 1: on auto and keyboard it is already */
    const int d = device_of(c, 0);
    if (d > 0 || d == -2) {
        cfg_set(c, "pad.p1", "all");
    }
}

static bool has_any(const char *n, const char *const *w)
{
    for (; *w; w++) {
        if (strstr(n, *w)) {
            return true;
        }
    }
    return false;
}

int im_preset_suggest(const char *title)
{
    char n[160];
    size_t i = 0;
    for (; title && title[i] && i < sizeof(n) - 1; i++) {
        n[i] = (char)((title[i] >= 'A' && title[i] <= 'Z') ? title[i] + 32 : title[i]);
    }
    n[i] = 0;
    static const char *const racing[] = {"forza", "project gotham", "burnout", "need for speed", "midtown madness",
        "rallisport", "colin mcrae", "outrun", "out run", "toca ", "midnight club", "wreckless", "flatout", "juiced",
        "racing", "rally", "motogp", "moto gp", "mx unleashed", "mx vs", "atv ", "crash 'n' burn", "test drive",
        "sega gt", "rpm tuning", "pro race driver", NULL};
    static const char *const fighting[] = {"soulcalibur", "soul calibur", "dead or alive", "mortal kombat",
        "virtua fighter", "tekken", "street fighter", "ultimate fighting", "smackdown", "def jam", "fight night",
        "wrestle", "wwe", "tao feng", "kakuto", "capcom vs", "king of fighters", NULL};
    static const char *const shooter[] = {"halo", "doom", "quake", "unreal", "half-life", "far cry", "counter-strike",
        "brothers in arms", "call of duty", "medal of honor", "ghost recon", "rainbow six", "timesplitters",
        "red faction", "turok", "goldeneye", "battlefront", "republic commando", "perfect dark", "condemned",
        "splinter cell", "ninja gaiden", "chronicles of riddick", "mercenaries", "crimson skies", "armed and dangerous",
        "psi-ops", "conflict: ", "area 51", "blood rayne", "bloodrayne", "painkiller", "project snowblind",
        "wolfenstein", "tom clancy", NULL};
    static const char *const sports[] = {"madden", "nba ", "nhl ", "fifa", "mlb", "tennis", "golf", "tiger woods",
        "ssx", "tony hawk", "amped", "pro evolution", "pes ", "sonic", "crash ", "spyro", "jet set radio", "rayman",
        "sly cooper", "kung fu chaos", "lego", "shrek", "spongebob", NULL};
    return has_any(n, racing) ? IM_PRESET_RACING : has_any(n, fighting) ? IM_PRESET_FIGHTING
           : has_any(n, shooter) ? IM_PRESET_SHOOTER : has_any(n, sports) ? IM_PRESET_KEYBOARD : IM_PRESET_NONE;
}
