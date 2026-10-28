# nxasahi: the controller settings (as Ryujinx's input window), without their screen (frontend/qml/Controllers.qml, through frontend/backend.py). Each of the eight
# players has a keyboard (on or off) and a controller (none, any, or one model), each with its own bindings. Saved in ~/.config/nxasahi/input.json and written for the
# host as input.map (DM_INPUT_MAP, read by dm/host/linux/front_input.c, which documents the format). The controllers are seen through SDL3 (ctypes, the library the host
# links): the library also navigates with them
import ctypes, ctypes.util, json, os
from pathlib import Path

CONFIG_DIR = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'nxasahi'
INPUT_JSON, INPUT_MAP = CONFIG_DIR / 'input.json', CONFIG_DIR / 'input.map'
PLAYERS = 8

# Switch targets (front_input.c TARGETS) and how they are shown
LABEL = {'A': 'A', 'B': 'B', 'X': 'X', 'Y': 'Y', 'LS': 'Press', 'RS': 'Press', 'L': 'L', 'R': 'R', 'ZL': 'ZL', 'ZR': 'ZR', 'PLUS': '+', 'MINUS': '−',
         'LEFT': 'Left', 'UP': 'Up', 'RIGHT': 'Right', 'DOWN': 'Down', 'SHAKE': 'Shake (motion)',
         'LSTICK_UP': 'Up', 'LSTICK_DOWN': 'Down', 'LSTICK_LEFT': 'Left', 'LSTICK_RIGHT': 'Right',
         'RSTICK_UP': 'Up', 'RSTICK_DOWN': 'Down', 'RSTICK_LEFT': 'Left', 'RSTICK_RIGHT': 'Right'}
GROUPS = (   # columns, as the controller is laid out
    (('Left Stick', ('LSTICK_UP', 'LSTICK_DOWN', 'LSTICK_LEFT', 'LSTICK_RIGHT', 'LS')), ('Triggers', ('L', 'ZL')), ('Buttons', ('MINUS',))),
    (('D-Pad', ('UP', 'DOWN', 'LEFT', 'RIGHT')), ('Other', ('SHAKE',))),
    (('Face Buttons', ('A', 'B', 'X', 'Y')), ('Triggers', ('R', 'ZR')), ('Buttons', ('PLUS',))),
    (('Right Stick', ('RSTICK_UP', 'RSTICK_DOWN', 'RSTICK_LEFT', 'RSTICK_RIGHT', 'RS')),),
)
TARGETS = [t for col in GROUPS for _, ts in col for t in ts]

# Controller inputs: SDL_GamepadButton / SDL_GamepadAxis order, the names the map file uses, and how they are shown (positions, with the Xbox letter)
PAD_BUTTONS = ['south', 'east', 'west', 'north', 'back', 'guide', 'start', 'leftstick', 'rightstick', 'leftshoulder', 'rightshoulder',
               'dpup', 'dpdown', 'dpleft', 'dpright', 'misc1', 'rightpaddle1', 'leftpaddle1', 'rightpaddle2', 'leftpaddle2', 'touchpad']
PAD_BUTTON_LABEL = {'south': 'Bottom (A)', 'east': 'Right (B)', 'west': 'Left (X)', 'north': 'Top (Y)', 'back': 'Back / Select', 'guide': 'Home / Guide',
                    'start': 'Start', 'leftstick': 'L3', 'rightstick': 'R3', 'leftshoulder': 'LB', 'rightshoulder': 'RB', 'dpup': 'D-Pad Up',
                    'dpdown': 'D-Pad Down', 'dpleft': 'D-Pad Left', 'dpright': 'D-Pad Right', 'misc1': 'Share / Capture', 'rightpaddle1': 'Paddle R1',
                    'leftpaddle1': 'Paddle L1', 'rightpaddle2': 'Paddle R2', 'leftpaddle2': 'Paddle L2', 'touchpad': 'Touchpad'}
PAD_AXES = ['leftx', 'lefty', 'rightx', 'righty', 'lefttrigger', 'righttrigger']
PAD_AXIS_LABEL = {'+leftx': 'Left Stick →', '-leftx': 'Left Stick ←', '+lefty': 'Left Stick ↓', '-lefty': 'Left Stick ↑',
                  '+rightx': 'Right Stick →', '-rightx': 'Right Stick ←', '+righty': 'Right Stick ↓', '-righty': 'Right Stick ↑',
                  '+lefttrigger': 'LT', '+righttrigger': 'RT'}

# Defaults: the same as front_input.c's built-in map (the Mac front end's)
DEFAULT_KEYS = {'A': 'J', 'B': 'K', 'X': 'U', 'Y': 'I', 'LS': 'F', 'RS': 'G', 'L': 'Q', 'R': 'E', 'ZL': 'Z', 'ZR': 'C', 'PLUS': 'Return', 'MINUS': 'Tab',
                'LEFT': 'Left', 'UP': 'Up', 'RIGHT': 'Right', 'DOWN': 'Down', 'SHAKE': 'Space',
                'LSTICK_UP': 'W', 'LSTICK_DOWN': 'S', 'LSTICK_LEFT': 'A', 'LSTICK_RIGHT': 'D'}
DEFAULT_BUTTONS = {'A': 'east', 'B': 'south', 'X': 'north', 'Y': 'west', 'LS': 'leftstick', 'RS': 'rightstick', 'L': 'leftshoulder', 'R': 'rightshoulder',
                   'ZL': '+lefttrigger', 'ZR': '+righttrigger', 'PLUS': 'start', 'MINUS': 'back', 'LEFT': 'dpleft', 'UP': 'dpup', 'RIGHT': 'dpright', 'DOWN': 'dpdown',
                   'LSTICK_UP': '-lefty', 'LSTICK_DOWN': '+lefty', 'LSTICK_LEFT': '-leftx', 'LSTICK_RIGHT': '+leftx',
                   'RSTICK_UP': '-righty', 'RSTICK_DOWN': '+righty', 'RSTICK_LEFT': '-rightx', 'RSTICK_RIGHT': '+rightx'}

# Linux evdev key codes (the native scan code Qt reports on Wayland and X11, less 8) → SDL scancode names: the host matches keys by position, as SDL does
EVDEV = {1: 'Escape', 14: 'Backspace', 15: 'Tab', 28: 'Return', 29: 'Left Ctrl', 42: 'Left Shift', 54: 'Right Shift', 56: 'Left Alt', 57: 'Space', 58: 'CapsLock',
         12: '-', 13: '=', 26: '[', 27: ']', 39: ';', 40: "'", 41: '`', 43: '\\', 51: ',', 52: '.', 53: '/', 86: 'NonUSBackslash',
         97: 'Right Ctrl', 100: 'Right Alt', 125: 'Left GUI', 126: 'Right GUI', 127: 'Menu', 102: 'Home', 103: 'Up', 104: 'PageUp', 105: 'Left', 106: 'Right',
         107: 'End', 108: 'Down', 109: 'PageDown', 110: 'Insert', 111: 'Delete', 69: 'Numlock', 70: 'ScrollLock', 99: 'PrintScreen', 119: 'Pause',
         55: 'Keypad *', 74: 'Keypad -', 78: 'Keypad +', 83: 'Keypad .', 96: 'Keypad Enter', 98: 'Keypad /', 87: 'F11', 88: 'F12',
         71: 'Keypad 7', 72: 'Keypad 8', 73: 'Keypad 9', 75: 'Keypad 4', 76: 'Keypad 5', 77: 'Keypad 6', 79: 'Keypad 1', 80: 'Keypad 2', 81: 'Keypad 3', 82: 'Keypad 0'}
EVDEV.update({2 + i: '1234567890'[i] for i in range(10)})
EVDEV.update({16 + i: c for i, c in enumerate('QWERTYUIOP')}); EVDEV.update({30 + i: c for i, c in enumerate('ASDFGHJKL')}); EVDEV.update({44 + i: c for i, c in enumerate('ZXCVBNM')})
EVDEV.update({59 + i: f'F{i + 1}' for i in range(10)})


def default_player(p):
    return {'keyboard': p == 0, 'controller': 'any', 'controller_name': '', 'deadzone': 0.1, 'keys': dict(DEFAULT_KEYS) if p == 0 else {}, 'buttons': dict(DEFAULT_BUTTONS)}


def load():
    try: players = json.loads(INPUT_JSON.read_text()).get('players', [])
    except (OSError, ValueError): players = []
    return [{**default_player(p), **(players[p] if p < len(players) and isinstance(players[p], dict) else {})} for p in range(PLAYERS)]


def save(players):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    tmp = INPUT_JSON.with_suffix('.tmp'); tmp.write_text(json.dumps({'players': players}, indent=1) + '\n'); tmp.replace(INPUT_JSON)
    write_map(players)


def write_map(players, path=INPUT_MAP):
    """The host's map file (front_input.c). Returns its path"""
    out = ['# written by the nxasahi library (frontend/controllers.py); edit the controllers there']
    for p, pl in enumerate(players):
        n = p + 1
        out.append(f'player {n} keyboard {"on" if pl.get("keyboard") else "off"}')
        out.append(f'player {n} controller {pl.get("controller") or "none"}')
        out.append(f'player {n} deadzone {float(pl.get("deadzone", 0)):.2f}')
        out += [f'player {n} key {t} {pl["keys"][t]}' for t in TARGETS if pl.get('keys', {}).get(t)]
        out += [f'player {n} button {t} {pl["buttons"][t]}' for t in TARGETS if pl.get('buttons', {}).get(t)]
    Path(path).parent.mkdir(parents=True, exist_ok=True); Path(path).write_text('\n'.join(out) + '\n')
    return Path(path)


def map_for_launch():
    """The map file for a game about to start, written fresh from the settings; None when the controllers were never set (the host's built-in map)"""
    return write_map(load()) if INPUT_JSON.is_file() else None


def pad_label(src):
    if not src: return 'Unbound'
    return PAD_AXIS_LABEL.get(src) or PAD_BUTTON_LABEL.get(src, src)


# ---- SDL3 through ctypes: the controllers that are connected, and what is pressed on them ----
class _GUID(ctypes.Structure): _fields_ = [('data', ctypes.c_uint8 * 16)]


class Pads:
    SDL_INIT_GAMEPAD = 0x2000

    def __init__(self):
        self.lib, self.open = None, {}   # instance id → SDL_Gamepad*
        name = ctypes.util.find_library('SDL3') or 'libSDL3.so.0'
        try: lib = ctypes.CDLL(name)
        except OSError: return
        for fn, res, args in (('SDL_SetHint', ctypes.c_bool, [ctypes.c_char_p, ctypes.c_char_p]), ('SDL_Init', ctypes.c_bool, [ctypes.c_uint32]),
                              ('SDL_QuitSubSystem', None, [ctypes.c_uint32]), ('SDL_GetError', ctypes.c_char_p, []),
                              ('SDL_GetGamepads', ctypes.POINTER(ctypes.c_uint32), [ctypes.POINTER(ctypes.c_int)]), ('SDL_free', None, [ctypes.c_void_p]),
                              ('SDL_GetGamepadNameForID', ctypes.c_char_p, [ctypes.c_uint32]), ('SDL_GetGamepadGUIDForID', _GUID, [ctypes.c_uint32]),
                              ('SDL_OpenGamepad', ctypes.c_void_p, [ctypes.c_uint32]), ('SDL_CloseGamepad', None, [ctypes.c_void_p]),
                              ('SDL_UpdateGamepads', None, []), ('SDL_GetGamepadButton', ctypes.c_bool, [ctypes.c_void_p, ctypes.c_int]),
                              ('SDL_GetGamepadAxis', ctypes.c_int16, [ctypes.c_void_p, ctypes.c_int])):
            f = getattr(lib, fn); f.restype = res; f.argtypes = args
        lib.SDL_SetHint(b'SDL_NO_SIGNAL_HANDLERS', b'1'); lib.SDL_SetHint(b'SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS', b'1')
        if not lib.SDL_Init(self.SDL_INIT_GAMEPAD):
            print(f'controllers: SDL_Init: {lib.SDL_GetError().decode(errors="replace")}'); return
        self.lib = lib

    def close(self):
        if not self.lib: return
        for g in self.open.values(): self.lib.SDL_CloseGamepad(g)
        self.open = {}; self.lib.SDL_QuitSubSystem(self.SDL_INIT_GAMEPAD); self.lib = None

    def poll(self):
        """[(guid, name, gamepad handle)] of the connected controllers, opened"""
        if not self.lib: return []
        self.lib.SDL_UpdateGamepads()
        n = ctypes.c_int(0); ids = self.lib.SDL_GetGamepads(ctypes.byref(n)); live = [ids[i] for i in range(n.value)] if ids else []
        if ids: self.lib.SDL_free(ids)
        for i in [i for i in self.open if i not in live]: self.lib.SDL_CloseGamepad(self.open.pop(i))
        out = []
        for i in live:
            if i not in self.open:
                g = self.lib.SDL_OpenGamepad(i)
                if not g: continue
                self.open[i] = g
            out.append((bytes(self.lib.SDL_GetGamepadGUIDForID(i).data).hex(), (self.lib.SDL_GetGamepadNameForID(i) or b'Controller').decode(errors='replace'), self.open[i]))
        return out

    def state(self, g):
        """(the buttons held, the axes) of one gamepad"""
        return ({b for i, b in enumerate(PAD_BUTTONS) if self.lib.SDL_GetGamepadButton(g, i)}, [self.lib.SDL_GetGamepadAxis(g, i) for i in range(len(PAD_AXES))])


def pressed_sources(btns, axes, thresh=16000):
    s = set(btns)
    for i, v in enumerate(axes):
        if v > thresh: s.add('+' + PAD_AXES[i])
        elif v < -thresh and 'trigger' not in PAD_AXES[i]: s.add('-' + PAD_AXES[i])
    return s
