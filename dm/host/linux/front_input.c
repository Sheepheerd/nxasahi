// Linux build: keyboard and SDL gamepads → the players' controllers, with the bindings remappable (front_sdl.c feeds it the window's events).
// Each player has a keyboard (on or off) and a controller (none, the next free one, or one model by its SDL GUID), each with its own bindings, and a stick deadzone.
// DM_INPUT_MAP names a map file (the library front end writes it: frontend/controllers.py); without one the built-in map below is the one the Mac front end uses.
// Map file, one line each, '#' starts a comment:
//   player <1-8> keyboard on|off
//   player <1-8> controller none|any|<GUID, 32 hex digits>
//   player <1-8> deadzone <0-0.9>                               radial, on each stick
//   player <1-8> key <TARGET> <SDL scancode name>               e.g. "key A J", "key ZL Left Shift"
//   player <1-8> button <TARGET> <pad button>|<+|-><pad axis>    e.g. "button A east", "button ZR +righttrigger", "button LSTICK_UP -lefty"
// TARGET: A B X Y LS RS L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN SHAKE and LSTICK_UP / _DOWN / _LEFT / _RIGHT, the same for RSTICK. Pad buttons by position: south east
// west north back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright misc1 rightpaddle1 leftpaddle1 rightpaddle2 leftpaddle2 touchpad;
// pad axes leftx lefty rightx righty (+ right / down) lefttrigger righttrigger (+ only). A target bound to an axis is pressed past half travel; a stick direction bound to
// a key or button is a full tilt. The file replaces the built-in map as a whole.
// Player 1's keyboard goes through dm_input_set and its controller through dm_pad_set_player(0), which the host ORs; any other player's keyboard and controller are
// merged here (buttons ORed, each stick axis the larger deflection). Controllers are taken in the order they connect: a player that names a model gets it first, then
// the first player set to any
#include "dm.h"
#include "dm_input.h"
#include <SDL3/SDL.h>
#include <math.h>

enum { T_BUTTONS = 17, T_LUP = T_BUTTONS, T_LDOWN, T_LLEFT, T_LRIGHT, T_RUP, T_RDOWN, T_RLEFT, T_RRIGHT, T_COUNT };   // 0..15 the NpadButton bits, 16 shake
static const char *const TARGETS[T_COUNT] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN", "SHAKE",
    "LSTICK_UP", "LSTICK_DOWN", "LSTICK_LEFT", "LSTICK_RIGHT", "RSTICK_UP", "RSTICK_DOWN", "RSTICK_LEFT", "RSTICK_RIGHT"};
// SDL_GamepadButton order (SDL3)
static const char *const PAD_BUTTONS[] = {"south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright", "misc1", "rightpaddle1", "leftpaddle1", "rightpaddle2", "leftpaddle2", "touchpad"};
static const char *const PAD_AXES[] = {"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};   // SDL_GamepadAxis order

typedef struct { int8_t kind, idx, sign; } pad_src;   // kind 0 unbound, 1 button, 2 axis (sign ±1)
typedef struct {
    int keyboard, pad_any, has_guid; SDL_GUID guid; float deadzone;
    SDL_Scancode key[T_COUNT]; pad_src pad[T_COUNT];
    SDL_Gamepad *gp;
} player_t;
static player_t g_pl[DM_PLAYERS];
static bool g_keys[SDL_SCANCODE_COUNT];

// The Mac front end's map (front_mac.m): W A S D the left stick, J K U I → A B X Y, F G press the sticks, Q E Z C → L R ZL ZR, Return Tab → + −, the arrows the
// d-pad, Space shakes; controllers one player each in the order they connect, buttons by position (right A, bottom B, top X, left Y), triggers as ZL / ZR
static const char DEFAULT_MAP[] =
    "player 1 keyboard on\n"
    "player 1 key A J\nplayer 1 key B K\nplayer 1 key X U\nplayer 1 key Y I\nplayer 1 key LS F\nplayer 1 key RS G\n"
    "player 1 key L Q\nplayer 1 key R E\nplayer 1 key ZL Z\nplayer 1 key ZR C\nplayer 1 key PLUS Return\nplayer 1 key MINUS Tab\n"
    "player 1 key LEFT Left\nplayer 1 key UP Up\nplayer 1 key RIGHT Right\nplayer 1 key DOWN Down\nplayer 1 key SHAKE Space\n"
    "player 1 key LSTICK_UP W\nplayer 1 key LSTICK_DOWN S\nplayer 1 key LSTICK_LEFT A\nplayer 1 key LSTICK_RIGHT D\n";
static const char DEFAULT_PAD[] =   // every player's controller, before "player N " is put in front
    "controller any\n"
    "button A east\nbutton B south\nbutton X north\nbutton Y west\nbutton LS leftstick\nbutton RS rightstick\n"
    "button L leftshoulder\nbutton R rightshoulder\nbutton ZL +lefttrigger\nbutton ZR +righttrigger\nbutton PLUS start\nbutton MINUS back\n"
    "button LEFT dpleft\nbutton UP dpup\nbutton RIGHT dpright\nbutton DOWN dpdown\n"
    "button LSTICK_UP -lefty\nbutton LSTICK_DOWN +lefty\nbutton LSTICK_LEFT -leftx\nbutton LSTICK_RIGHT +leftx\n"
    "button RSTICK_UP -righty\nbutton RSTICK_DOWN +righty\nbutton RSTICK_LEFT -rightx\nbutton RSTICK_RIGHT +rightx\n";

static int find(const char *const *tab, int n, const char *s) { for (int i = 0; i < n; i++) if (!SDL_strcasecmp(tab[i], s)) return i; return -1; }
#define FIND(tab, s) find(tab, (int)(sizeof tab / sizeof *tab), s)

static void parse_line(char *line, const char *where, int ln) {
    char *h = strchr(line, '#'); if (h) *h = 0;
    char *save, *w0 = strtok_r(line, " \t\r\n", &save);
    if (!w0) return;
    char *ps = strtok_r(NULL, " \t\r\n", &save), *what = strtok_r(NULL, " \t\r\n", &save), *rest = strtok_r(NULL, "\r\n", &save);
    int p = ps ? atoi(ps) - 1 : -1;
    if (strcmp(w0, "player") || p < 0 || p >= DM_PLAYERS || !what) { LOG("[RED] input map %s:%d: not a player line", where, ln); return; }
    while (rest && (*rest == ' ' || *rest == '\t')) rest++;
    char *e = rest ? rest + strlen(rest) : NULL; while (e && e > rest && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    player_t *pl = &g_pl[p];
    if (!strcmp(what, "keyboard")) { pl->keyboard = rest && !strcmp(rest, "on"); return; }
    if (!strcmp(what, "deadzone")) { float d = rest ? strtof(rest, NULL) : 0; pl->deadzone = d < 0 ? 0 : d > 0.9f ? 0.9f : d; return; }
    if (!strcmp(what, "controller")) {
        pl->pad_any = pl->has_guid = 0;
        if (!rest || !strcmp(rest, "none")) return;
        if (!strcmp(rest, "any")) { pl->pad_any = 1; return; }
        pl->guid = SDL_StringToGUID(rest); pl->has_guid = 1; return;
    }
    if (strcmp(what, "key") && strcmp(what, "button")) { LOG("[RED] input map %s:%d: unknown setting '%s'", where, ln, what); return; }
    // key / button <TARGET> <source>
    char *tsave, *tn = rest ? strtok_r(rest, " \t", &tsave) : NULL, *src = tn ? strtok_r(NULL, "", &tsave) : NULL;
    while (src && (*src == ' ' || *src == '\t')) src++;
    int t = tn ? FIND(TARGETS, tn) : -1;
    if (t < 0) { LOG("[RED] input map %s:%d: unknown target '%s'", where, ln, tn ? tn : ""); return; }
    if (!strcmp(what, "key")) {
        SDL_Scancode sc = src && *src ? SDL_GetScancodeFromName(src) : SDL_SCANCODE_UNKNOWN;
        if (src && *src && sc == SDL_SCANCODE_UNKNOWN) LOG("[RED] input map %s:%d: unknown key '%s'", where, ln, src);
        pl->key[t] = sc; return;
    }
    {   // button
        pad_src s = {0};
        if (src && (*src == '+' || *src == '-')) { int a = FIND(PAD_AXES, src + 1); if (a >= 0) s = (pad_src){2, (int8_t)a, (int8_t)(*src == '-' ? -1 : 1)}; }
        else if (src && *src) { int b = FIND(PAD_BUTTONS, src); if (b >= 0) s = (pad_src){1, (int8_t)b, 0}; }
        if (src && *src && !s.kind) LOG("[RED] input map %s:%d: unknown controller input '%s'", where, ln, src);
        pl->pad[t] = s; return;
    }
}
static void parse(char *text, const char *where) {
    int ln = 0;
    for (char *save, *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) parse_line(l, where, ++ln);
}

static void players_changed(void);
void input_init(void) {
    memset(g_pl, 0, sizeof g_pl);
    const char *path = getenv("DM_INPUT_MAP");
    if (path && *path) {
        FILE *f = fopen(path, "r");
        if (f) {
            static char buf[1 << 16]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
            parse(buf, path); LOG("input: the map in %s", path); players_changed(); return;
        }
        LOG("[RED] input: DM_INPUT_MAP=%s cannot be read; the built-in map", path);
    }
    static char buf[sizeof DEFAULT_MAP + DM_PLAYERS * (sizeof DEFAULT_PAD + 256)];
    int n = snprintf(buf, sizeof buf, "%s", DEFAULT_MAP);
    for (int p = 0; p < DM_PLAYERS; p++)
        for (const char *l = DEFAULT_PAD; *l;) { const char *e = strchr(l, '\n'); n += snprintf(buf + n, sizeof buf - n, "player %d %.*s\n", p + 1, (int)(e - l), l); l = e + 1; }
    parse(buf, "built-in"); players_changed();
}

// 0..DM_STICK_MAX: how far a source is pressed
static int32_t src_key(const player_t *pl, int t) { return pl->keyboard && pl->key[t] && g_keys[pl->key[t]] ? DM_STICK_MAX : 0; }
static int32_t src_pad(const player_t *pl, int t) {
    pad_src s = pl->pad[t];
    if (!pl->gp || !s.kind) return 0;
    if (s.kind == 1) return SDL_GetGamepadButton(pl->gp, (SDL_GamepadButton)s.idx) ? DM_STICK_MAX : 0;
    int32_t v = SDL_GetGamepadAxis(pl->gp, (SDL_GamepadAxis)s.idx) * s.sign;
    return v < 0 ? 0 : v > DM_STICK_MAX ? DM_STICK_MAX : v;
}
static void stick(int32_t *x, int32_t *y, float dz) {
    if (dz <= 0) return;
    float fx = *x / (float)DM_STICK_MAX, fy = *y / (float)DM_STICK_MAX, r = sqrtf(fx * fx + fy * fy);
    if (r <= dz) { *x = *y = 0; return; }
    float k = ((r > 1 ? 1 : r) - dz) / (1 - dz) / r;   // radial: the deadzone's edge becomes the centre, full travel stays full
    *x = (int32_t)lrintf(fminf(fmaxf(fx * k, -1), 1) * DM_STICK_MAX); *y = (int32_t)lrintf(fminf(fmaxf(fy * k, -1), 1) * DM_STICK_MAX);
}
typedef struct { uint64_t b; int32_t lx, ly, rx, ry; } pad_state;
static pad_state eval(const player_t *pl, int32_t (*src)(const player_t *, int)) {
    pad_state s = {0};
    for (int t = 0; t < T_BUTTONS; t++) if (src(pl, t) > DM_STICK_MAX / 2) s.b |= t == 16 ? DM_BTN_SHAKE : 1ull << t;
    s.lx = src(pl, T_LRIGHT) - src(pl, T_LLEFT); s.ly = src(pl, T_LUP) - src(pl, T_LDOWN);
    s.rx = src(pl, T_RRIGHT) - src(pl, T_RLEFT); s.ry = src(pl, T_RUP) - src(pl, T_RDOWN);
    if (src == src_pad) { stick(&s.lx, &s.ly, pl->deadzone); stick(&s.rx, &s.ry, pl->deadzone); }
    return s;
}
static int32_t larger(int32_t a, int32_t b) { return abs(a) >= abs(b) ? a : b; }
static void push(int p) {
    player_t *pl = &g_pl[p];
    pad_state k = eval(pl, src_key), c = eval(pl, src_pad);
    if (p == 0) {   // the host ORs player 1's keyboard path and controller path
        if (pl->keyboard) dm_input_set(k.b, k.lx, k.ly, k.rx, k.ry);
        dm_pad_set_player(0, c.b, c.lx, c.ly, c.rx, c.ry);
        return;
    }
    dm_pad_set_player(p, k.b | c.b, larger(k.lx, c.lx), larger(k.ly, c.ly), larger(k.rx, c.rx), larger(k.ry, c.ry));
}
static void players_changed(void) {
    uint32_t mask = 1; for (int p = 0; p < DM_PLAYERS; p++) if (g_pl[p].gp || g_pl[p].keyboard) mask |= 1u << p;
    dm_pad_players(mask);
}

void input_key(const SDL_KeyboardEvent *k) {
    if (k->repeat || (unsigned)k->scancode >= SDL_SCANCODE_COUNT) return;
    g_keys[k->scancode] = k->down;
    for (int p = 0; p < DM_PLAYERS; p++) if (g_pl[p].keyboard) push(p);
}
void input_pad_added(SDL_JoystickID id) {
    for (int p = 0; p < DM_PLAYERS; p++) if (g_pl[p].gp && SDL_GetGamepadID(g_pl[p].gp) == id) return;
    SDL_GUID guid = SDL_GetGamepadGUIDForID(id);
    int want = -1;
    for (int p = 0; p < DM_PLAYERS && want < 0; p++) if (!g_pl[p].gp && g_pl[p].has_guid && !memcmp(&g_pl[p].guid, &guid, sizeof guid)) want = p;
    for (int p = 0; p < DM_PLAYERS && want < 0; p++) if (!g_pl[p].gp && g_pl[p].pad_any) want = p;
    char gs[33]; SDL_GUIDToString(guid, gs, sizeof gs);
    if (want < 0) { LOG("controller '%s' (%s) connected: no player is set to take it", SDL_GetGamepadNameForID(id), gs); return; }
    if (!(g_pl[want].gp = SDL_OpenGamepad(id))) { LOG("[RED] controller '%s': SDL_OpenGamepad: %s", SDL_GetGamepadNameForID(id), SDL_GetError()); return; }
    LOG("controller '%s' (%s) connected: player %d (npad %u)", SDL_GetGamepadName(g_pl[want].gp), gs, want + 1, dm_player_npad(want));
    players_changed(); push(want);
}
void input_pad_removed(SDL_JoystickID id) {
    for (int p = 0; p < DM_PLAYERS; p++) if (g_pl[p].gp && SDL_GetGamepadID(g_pl[p].gp) == id) {
        LOG("controller '%s' disconnected: player %d", SDL_GetGamepadName(g_pl[p].gp), p + 1);
        SDL_CloseGamepad(g_pl[p].gp); g_pl[p].gp = NULL; push(p); players_changed();
    }
}
void input_pad_event(SDL_JoystickID id) { for (int p = 0; p < DM_PLAYERS; p++) if (g_pl[p].gp && SDL_GetGamepadID(g_pl[p].gp) == id) push(p); }
void input_release(void) {   // the window lost focus: let everything go, so nothing stays held
    memset(g_keys, 0, sizeof g_keys);
    for (int p = 0; p < DM_PLAYERS; p++) {
        if (p == 0 && g_pl[0].keyboard) dm_input_set(0, 0, 0, 0, 0);
        if (g_pl[p].gp || g_pl[p].keyboard) dm_pad_set_player(p, 0, 0, 0, 0, 0);
    }
}
