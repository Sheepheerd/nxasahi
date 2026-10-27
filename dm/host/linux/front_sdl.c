// Linux build: the front end on SDL3, in place of front_mac.m. Without DM_WINDOW the game runs headless on this thread, as the Mac host. With DM_WINDOW=1 the main
// thread owns the window and its event loop and the game runs on a thread of its own (64 MiB stack, signal stack), as on the Mac. Lifecycle as front_mac.m: closing
// the window quits; focus lost → 2, regained → 1; minimized → 3 with the GPU suspended, restored → resumed. Input: keyboard and SDL gamepads, mapped as on the Mac unless DM_INPUT_MAP remaps them (front_input.c)
#include "dm.h"
#include "hle.h"
#include "front.h"
#include "vk_window.h"
#include "vk_dev.h"
#include "dm_input.h"
#include <SDL3/SDL.h>
#include <spawn.h>
#include <sys/wait.h>

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];   // the signal alternate stack is per thread; crash reports must work on the guest thread too
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("guest thread finished"); exit(0);
}

// ---- input: front_input.c (the bindings, DM_INPUT_MAP) ----
void input_init(void); void input_key(const SDL_KeyboardEvent *k); void input_pad_added(SDL_JoystickID id); void input_pad_removed(SDL_JoystickID id);
void input_pad_event(SDL_JoystickID id); void input_release(void);

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
    input_init();
    static guest_arg ga; ga.fn = guest;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
    for (SDL_Event e; SDL_WaitEvent(&e);) switch (e.type) {
        case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: LOG("front end: window closed"); exit(0);
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: vkw_resized(); break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: input_release(); dm_focus_set(2); break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: dm_focus_set(1); break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: input_key(&e.key); break;
        case SDL_EVENT_GAMEPAD_ADDED: input_pad_added(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: input_pad_removed(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: input_pad_event(e.gbutton.which); break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: input_pad_event(e.gaxis.which); break;
        case SDL_EVENT_WINDOW_MINIMIZED: input_release(); dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); vkw_suspend(1); break;
        case SDL_EVENT_WINDOW_RESTORED: vkw_suspend(0); vkw_resized(); dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); break;
        default: break;
    }
    die("SDL_WaitEvent: %s", SDL_GetError());
}
int front_screen_max_fps(void) { return vkw_display_hz(); }

// ---- the software keyboard (front.h front_text_input, hle_swkbd.c) ----
// An input box in a process of its own, run from the game thread that opened the keyboard (it blocks there until the box closes; the window's event loop goes on):
// DM_TEXT_INPUT names the program, the import's manifest sets it to frontend/text_input.py (GTK 4), else zenity when it is installed. The program gets the title,
// subtitle, hint, initial text and maximum length as arguments and prints the text: exit 0 = confirmed, 1 = cancelled. Without a window, nothing is shown (-1:
// hle_swkbd.c answers from DM_KBD or the initial text). Pokémon: Let's Go asks for the player's name with it
extern char **environ;
static int run_box(char *const argv[], char *out, int out_cap) {   // 1 confirmed, 0 cancelled, -1 the program did not run
    int fd[2]; if (pipe(fd)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1); posix_spawn_file_actions_addclose(&fa, fd[0]);
    pid_t pid; int e = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa); close(fd[1]);
    if (e) { close(fd[0]); return -1; }
    int n = 0; ssize_t r;
    while ((r = read(fd[0], out + n, (size_t)(out_cap - 1 - n))) > 0 && n < out_cap - 1) n += (int)r;
    close(fd[0]); out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;   // zenity ends the text with a newline
    int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st) == 0 ? 1 : WEXITSTATUS(st) == 1 ? 0 : -1;   // 127: not found
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    const char *box = getenv("DM_TEXT_INPUT");
    if (out_cap < 1 || (!vkw_active() && !(box && *box))) return -1;   // headless: only a program named outright (scripted runs)
    char maxs[16]; snprintf(maxs, sizeof maxs, "%d", max_chars > 0 ? max_chars : 0);
    if (box && *box) {   // "python3 /path/text_input.py": split on spaces (the paths it is made from have none to spare)
        char cmd[1024]; snprintf(cmd, sizeof cmd, "%s", box);
        char *argv[16]; int ac = 0;
        for (char *t = strtok(cmd, " "); t && ac < 10; t = strtok(NULL, " ")) argv[ac++] = t;
        argv[ac++] = (char *)(title ? title : ""); argv[ac++] = (char *)(sub ? sub : ""); argv[ac++] = (char *)(guide ? guide : "");
        argv[ac++] = (char *)(initial ? initial : ""); argv[ac++] = maxs; argv[ac] = NULL;
        int r = run_box(argv, out, out_cap);
        if (r >= 0) { LOG("software keyboard: %s", r ? "confirmed" : "cancelled"); return r; }
        LOG("[RED] software keyboard: %s did not run; trying zenity", box);
    }
    char text[600]; snprintf(text, sizeof text, "%s%s%s", sub && *sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : "");
    char *zargv[] = {"zenity", "--entry", "--title", (char *)(title && *title ? title : "Enter text"), "--text", text, "--entry-text", (char *)(initial ? initial : ""), NULL};
    int r = run_box(zargv, out, out_cap);
    if (r < 0) LOG("[RED] software keyboard: no input box (DM_TEXT_INPUT not set and no zenity): the game gets its initial text");
    else LOG("software keyboard: %s", r ? "confirmed" : "cancelled");
    return r;
}
