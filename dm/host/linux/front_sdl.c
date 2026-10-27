// Linux build: the front end on SDL3, in place of front_mac.m. Without DM_WINDOW the game runs headless on this thread, as the Mac host. With DM_WINDOW=1 the main
// thread owns the window and its event loop and the game runs on a thread of its own (64 MiB stack, signal stack), as on the Mac. Lifecycle as front_mac.m: closing
// the window quits; focus lost → 2, regained → 1; minimized → 3 with the GPU suspended, restored → resumed. Input: keyboard and SDL gamepads, mapped as on the Mac (below)
#include "dm.h"
#include "hle.h"
#include "front.h"
#include "vk_window.h"
#include "vk_dev.h"
#include "dm_input.h"
#include <SDL3/SDL.h>

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];   // the signal alternate stack is per thread; crash reports must work on the guest thread too
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("guest thread finished"); exit(0);
}

// ---- input, as the Mac front end maps it (front_mac.m) ----
// Keyboard (dm_input_set): W A S D the left stick, J K U I → A B X Y, F G press the sticks, Q E Z C → L R ZL ZR, Return Tab → + −, the arrows the d-pad, Space shakes.
// Controllers (SDL gamepads, dm_pad_set_player): one player each in the order they connect, buttons by position as on the Mac and the iPhone (right A, bottom B, top X,
// left Y), triggers as ZL / ZR. Everything is let go when the window loses focus, so nothing stays held
static uint64_t g_kbtn; static int g_kw, g_ka, g_ks, g_kd;
static SDL_Gamepad *g_pad[DM_PLAYERS];
static void kb_push(void) { dm_input_set(g_kbtn, (g_kd - g_ka) * DM_STICK_MAX, (g_kw - g_ks) * DM_STICK_MAX, 0, 0); }
static uint64_t key_bit(SDL_Scancode s) {
    switch (s) {
    case SDL_SCANCODE_J: return DM_BTN_A; case SDL_SCANCODE_K: return DM_BTN_B; case SDL_SCANCODE_U: return DM_BTN_X; case SDL_SCANCODE_I: return DM_BTN_Y;
    case SDL_SCANCODE_F: return DM_BTN_LS; case SDL_SCANCODE_G: return DM_BTN_RS;
    case SDL_SCANCODE_Q: return DM_BTN_L; case SDL_SCANCODE_E: return DM_BTN_R; case SDL_SCANCODE_Z: return DM_BTN_ZL; case SDL_SCANCODE_C: return DM_BTN_ZR;
    case SDL_SCANCODE_RETURN: return DM_BTN_PLUS; case SDL_SCANCODE_TAB: return DM_BTN_MINUS;
    case SDL_SCANCODE_LEFT: return DM_BTN_LEFT; case SDL_SCANCODE_UP: return DM_BTN_UP; case SDL_SCANCODE_RIGHT: return DM_BTN_RIGHT; case SDL_SCANCODE_DOWN: return DM_BTN_DOWN;
    case SDL_SCANCODE_SPACE: return DM_BTN_SHAKE;
    default: return 0;
    }
}
static void on_key(const SDL_KeyboardEvent *k) {
    if (k->repeat) return;
    int down = k->down;
    switch (k->scancode) { case SDL_SCANCODE_W: g_kw = down; break; case SDL_SCANCODE_A: g_ka = down; break; case SDL_SCANCODE_S: g_ks = down; break; case SDL_SCANCODE_D: g_kd = down; break; default: break; }
    uint64_t m = key_bit(k->scancode);
    g_kbtn = down ? g_kbtn | m : g_kbtn & ~m;
    kb_push();
}
static int32_t axis(Sint16 v, int invert) { int32_t a = invert ? -(int32_t)v : v; if (a > DM_STICK_MAX) a = DM_STICK_MAX; if (a < -DM_STICK_MAX) a = -DM_STICK_MAX; return a; }
static void pad_push(int p) {
    SDL_Gamepad *g = g_pad[p]; if (!g) return;
    uint64_t m = 0;
    #define B(sdl, dm) if (SDL_GetGamepadButton(g, sdl)) m |= dm
    B(SDL_GAMEPAD_BUTTON_EAST, DM_BTN_A); B(SDL_GAMEPAD_BUTTON_SOUTH, DM_BTN_B); B(SDL_GAMEPAD_BUTTON_NORTH, DM_BTN_X); B(SDL_GAMEPAD_BUTTON_WEST, DM_BTN_Y);
    B(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, DM_BTN_L); B(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, DM_BTN_R);
    B(SDL_GAMEPAD_BUTTON_START, DM_BTN_PLUS); B(SDL_GAMEPAD_BUTTON_BACK, DM_BTN_MINUS); B(SDL_GAMEPAD_BUTTON_LEFT_STICK, DM_BTN_LS); B(SDL_GAMEPAD_BUTTON_RIGHT_STICK, DM_BTN_RS);
    B(SDL_GAMEPAD_BUTTON_DPAD_LEFT, DM_BTN_LEFT); B(SDL_GAMEPAD_BUTTON_DPAD_UP, DM_BTN_UP); B(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, DM_BTN_RIGHT); B(SDL_GAMEPAD_BUTTON_DPAD_DOWN, DM_BTN_DOWN);
    #undef B
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) m |= DM_BTN_ZL;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) m |= DM_BTN_ZR;
    // SDL's sticks grow down on y, the Switch's up
    dm_pad_set_player(p, m, axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), 0), axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY), 1),
                      axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX), 0), axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY), 1));
}
static void pads_changed(void) {
    uint32_t mask = 1; for (int p = 0; p < DM_PLAYERS; p++) if (g_pad[p]) mask |= 1u << p;
    dm_pad_players(mask);
}
static void pad_added(SDL_JoystickID id) {
    for (int p = 0; p < DM_PLAYERS; p++) if (g_pad[p] && SDL_GetGamepadID(g_pad[p]) == id) return;
    for (int p = 0; p < DM_PLAYERS; p++) if (!g_pad[p]) {
        g_pad[p] = SDL_OpenGamepad(id);
        if (g_pad[p]) { LOG("controller '%s' connected: player %d (npad %u)", SDL_GetGamepadName(g_pad[p]), p + 1, dm_player_npad(p)); pads_changed(); pad_push(p); }
        return;
    }
}
static void pad_removed(SDL_JoystickID id) {
    for (int p = 0; p < DM_PLAYERS; p++) if (g_pad[p] && SDL_GetGamepadID(g_pad[p]) == id) {
        LOG("controller '%s' disconnected: player %d", SDL_GetGamepadName(g_pad[p]), p + 1);
        SDL_CloseGamepad(g_pad[p]); g_pad[p] = NULL; dm_pad_set_player(p, 0, 0, 0, 0, 0); pads_changed();
    }
}
static void pad_event(SDL_JoystickID id) { for (int p = 0; p < DM_PLAYERS; p++) if (g_pad[p] && SDL_GetGamepadID(g_pad[p]) == id) pad_push(p); }
static void input_release(void) {
    g_kbtn = 0; g_kw = g_ka = g_ks = g_kd = 0; kb_push();
    for (int p = 0; p < DM_PLAYERS; p++) if (g_pad[p]) dm_pad_set_player(p, 0, 0, 0, 0, 0);
}

void front_run(void (*guest)(void)) {
    // The Vulkan device exists before the game runs: guest memory is backed with device memory as the game commits it (vk_mem.c). DM_VK=0: none (headless only)
    if (!getenv("DM_WINDOW") || !atoi(getenv("DM_WINDOW"))) {
        if (!getenv("DM_VK") || atoi(getenv("DM_VK"))) vkd_init(0);
        guest(); return;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) die("SDL_Init: %s", SDL_GetError());
    if (!vkd_init(1)) die("no Vulkan device for the window (see the vulkan lines above)");
    SDL_Window *w = SDL_CreateWindow(DM_GAME_NAME, 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!w) die("SDL_CreateWindow: %s", SDL_GetError());
    if (getenv("DM_FULLSCREEN") && atoi(getenv("DM_FULLSCREEN"))) SDL_SetWindowFullscreen(w, true);
    if (!vkw_init(w)) die("no Vulkan presentation for the window (see the vulkan lines above)");
    LOG("front end: SDL %d.%d.%d window on %s, display %d Hz", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
        SDL_GetCurrentVideoDriver(), vkw_display_hz());
    static guest_arg ga; ga.fn = guest;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
    for (SDL_Event e; SDL_WaitEvent(&e);) switch (e.type) {
        case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: LOG("front end: window closed"); exit(0);
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: vkw_resized(); break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: input_release(); dm_focus_set(2); break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: dm_focus_set(1); break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: on_key(&e.key); break;
        case SDL_EVENT_GAMEPAD_ADDED: pad_added(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: pad_removed(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: pad_event(e.gbutton.which); break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: pad_event(e.gaxis.which); break;
        case SDL_EVENT_WINDOW_MINIMIZED: input_release(); dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); vkw_suspend(1); break;
        case SDL_EVENT_WINDOW_RESTORED: vkw_suspend(0); vkw_resized(); dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); break;
        default: break;
    }
    die("SDL_WaitEvent: %s", SDL_GetError());
}
int front_screen_max_fps(void) { return vkw_display_hz(); }
