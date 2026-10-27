# nxasahi: the controller settings (as Ryujinx's input window). Each of the eight players has a keyboard (on or off) and a controller (none, any, or one model), each with
# its own bindings; click a binding, then press the key or the controller button / stick to give it. Saved in ~/.config/nxasahi/input.json and written for the host as
# input.map (DM_INPUT_MAP, read by dm/host/linux/front_input.c, which documents the format). The controllers are seen through SDL3 (ctypes, the library the host links),
# opened only while this window is open
import ctypes, ctypes.util, json, os
from pathlib import Path

from gi.repository import Adw, Gdk, GLib, Gtk, Pango

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

# Linux evdev key codes (the hardware keycode GTK reports, less 8) → SDL scancode names: the host matches keys by position, as SDL does
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


# ---- the window ----
class ControllersDialog(Adw.Dialog):
    def __init__(self, toast=None):
        super().__init__(title='Controllers', content_width=900, content_height=760)
        self.players, self.toast = load(), toast
        self.p, self.listen, self.pads, self.connected, self.keys_down = 0, None, Pads(), [], set()
        self.baseline = set()
        tv = Adw.ToolbarView(); hb = Adw.HeaderBar(); tv.add_top_bar(hb)
        reset = Gtk.Button(label='Reset Player', tooltip_text='Give this player the default bindings'); reset.connect('clicked', self.reset_player); hb.pack_start(reset)
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, margin_top=12, margin_bottom=18, margin_start=18, margin_end=18)

        # player, keyboard, controller
        top = Adw.PreferencesGroup()
        self.prow = Adw.ComboRow(title='Player', model=Gtk.StringList.new([f'Player {i + 1}' for i in range(PLAYERS)]))
        self.prow.connect('notify::selected', lambda *a: self.select_player(self.prow.get_selected())); top.add(self.prow)
        self.kbrow = Adw.SwitchRow(title='Keyboard', subtitle='This player uses the keyboard (bindings in the Keyboard tab)')
        self.kbrow.connect('notify::active', self.on_keyboard); top.add(self.kbrow)
        self.crow = Adw.ComboRow(title='Controller', subtitle='Any: the next controller that connects. A named model is kept for this player')
        self.cmodel = Gtk.StringList(); self.crow.set_model(self.cmodel); self.cids = []
        self.crow_handler = self.crow.connect('notify::selected', self.on_controller); top.add(self.crow)
        self.dzrow = Adw.ActionRow(title='Stick deadzone', subtitle='Stick travel ignored around the centre')
        self.dz = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 0.5, 0.01); self.dz.set_size_request(240, -1); self.dz.set_valign(Gtk.Align.CENTER)
        self.dz.set_draw_value(True); self.dz.set_format_value_func(lambda s, v: f'{round(v * 100)} %'); self.dz.connect('value-changed', self.on_deadzone)
        self.dzrow.add_suffix(self.dz); top.add(self.dzrow)
        body.append(top)

        # bindings: a Keyboard tab and a Controller tab, each laid out as the controller is
        self.stack = Adw.ViewStack(); self.btn = {'keys': {}, 'buttons': {}}
        for kind, title, icon in (('keys', 'Keyboard', 'input-keyboard-symbolic'), ('buttons', 'Controller', 'input-gaming-symbolic')):
            grid = Gtk.FlowBox(selection_mode=Gtk.SelectionMode.NONE, homogeneous=True, min_children_per_line=1, max_children_per_line=4, column_spacing=12, row_spacing=12,
                               valign=Gtk.Align.START)
            for col in GROUPS:
                cbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
                for gname, targets in col:
                    g = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4, css_classes=['card', 'bind-group'])
                    g.append(Gtk.Label(label=gname, xalign=0, css_classes=['heading'], margin_bottom=2))
                    for t in targets:
                        row = Gtk.Box(spacing=8)
                        row.append(Gtk.Label(label=LABEL[t], xalign=0, hexpand=True))
                        b = Gtk.Button(width_request=118, css_classes=['bind'], child=Gtk.Label(ellipsize=Pango.EllipsizeMode.END, max_width_chars=12)); b.connect('clicked', self.start_listen, kind, t)
                        rc = Gtk.GestureClick(button=3); rc.connect('pressed', (lambda k, tt: lambda *a: self.set_bind(k, tt, ''))(kind, t)); b.add_controller(rc)
                        b.set_tooltip_text('Click, then press a key or a controller input. Right-click to unbind. Escape cancels')
                        row.append(b); g.append(row); self.btn[kind][t] = b
                    cbox.append(g)
                grid.append(cbox)
            self.stack.add_titled_with_icon(grid, kind, title, icon)
        sw = Adw.ViewSwitcher(stack=self.stack, policy=Adw.ViewSwitcherPolicy.WIDE, halign=Gtk.Align.CENTER)
        body.append(sw); body.append(self.stack)
        self.status = Gtk.Label(xalign=0, css_classes=['dim-label'], wrap=True); body.append(self.status)
        tv.set_content(Gtk.ScrolledWindow(child=body, hscrollbar_policy=Gtk.PolicyType.NEVER, propagate_natural_height=True)); self.set_child(tv)

        kc = Gtk.EventControllerKey(propagation_phase=Gtk.PropagationPhase.CAPTURE)
        kc.connect('key-pressed', self.on_key); kc.connect('key-released', self.on_key_up); self.add_controller(kc)
        self.connect('closed', lambda *a: self.shutdown())
        css = Gtk.CssProvider()
        css.load_from_string('.bind-group { padding: 8px 10px; } button.bind { padding-left: 6px; padding-right: 6px; } button.bind.listening { background: @accent_bg_color; color: @accent_fg_color; } '
                             'button.bind.live { box-shadow: inset 0 0 0 2px @success_color; }')
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        if not self.pads.lib: self.status.set_label('SDL3 could not be loaded (run the library inside nix develop): controllers are not detected here, so their bindings cannot be captured.')
        self.select_player(0)
        self.timer = GLib.timeout_add(16, self.tick)

    # ---- the player shown ----
    @property
    def pl(self): return self.players[self.p]

    def select_player(self, p):
        self.p = p; self.stop_listen()
        self.kbrow.set_active(bool(self.pl['keyboard'])); self.dz.set_value(float(self.pl.get('deadzone', 0)))
        self.fill_controllers(force=True); self.refresh()

    def fill_controllers(self, force=False):
        found = [(guid, name) for guid, name, _ in self.connected]
        cur = self.pl.get('controller') or 'none'
        ids = ['none', 'any'] + [g for g, _ in found]
        names = ['None', 'Any controller'] + [f'{n}' for _, n in found]
        if cur not in ids: ids.append(cur); names.append(f'{self.pl.get("controller_name") or "Controller"} (not connected)')
        if not force and ids == self.cids and names == [self.cmodel.get_string(i) for i in range(self.cmodel.get_n_items())]: return
        self.crow.handler_block(self.crow_handler)
        self.cmodel.splice(0, self.cmodel.get_n_items(), names); self.cids = ids; self.crow.set_selected(ids.index(cur))
        self.crow.handler_unblock(self.crow_handler)

    def refresh(self):
        for kind, btns in self.btn.items():
            for t, b in btns.items():
                v = self.pl[kind].get(t, '')
                b.get_child().set_label('Press…' if self.listen == (kind, t) else (v or 'Unbound') if kind == 'keys' else pad_label(v))
                if self.listen == (kind, t): b.add_css_class('listening')
                else: b.remove_css_class('listening')
        self.stack.get_child_by_name('keys').set_sensitive(bool(self.pl['keyboard']))
        self.stack.get_child_by_name('buttons').set_sensitive(self.pl.get('controller', 'none') != 'none')
        self.dzrow.set_sensitive(self.pl.get('controller', 'none') != 'none')

    def changed(self):
        try: save(self.players)
        except OSError as e: self.status.set_label(f'Could not save: {e}')
        self.refresh()

    def on_keyboard(self, row, _):
        if bool(self.pl['keyboard']) == row.get_active(): return
        self.pl['keyboard'] = row.get_active()
        if self.pl['keyboard'] and not self.pl['keys']: self.pl['keys'] = dict(DEFAULT_KEYS)
        others = [i + 1 for i, q in enumerate(self.players) if q['keyboard'] and i != self.p]
        if self.pl['keyboard'] and others: self.status.set_label(f'The keyboard also drives player {", ".join(map(str, others))}: a key bound for both presses both.')
        self.changed()

    def on_controller(self, row, _):
        i = row.get_selected()
        if i >= len(self.cids): return
        self.pl['controller'] = self.cids[i]
        self.pl['controller_name'] = next((n for g, n, _ in self.connected if g == self.cids[i]), self.pl.get('controller_name', '')) if i >= 2 else ''
        self.changed()

    def on_deadzone(self, s):
        if abs(float(self.pl.get('deadzone', 0)) - s.get_value()) < 1e-3: return
        self.pl['deadzone'] = round(s.get_value(), 2); self.changed()

    def reset_player(self, *a):
        self.players[self.p] = default_player(self.p); self.select_player(self.p); self.changed()

    def set_bind(self, kind, t, v):
        if v: self.pl[kind][t] = v
        else: self.pl[kind].pop(t, None)
        self.listen = None; self.changed()

    # ---- capturing a binding ----
    def start_listen(self, b, kind, t):
        self.listen = (kind, t); self.baseline = self.pad_pressed(); self.refresh()
        self.status.set_label('Press a key (Escape cancels, Backspace unbinds)' if kind == 'keys' else 'Press a button, pull a trigger or push a stick (Escape cancels)')

    def stop_listen(self):
        if self.listen: self.listen = None; self.status.set_label(''); self.refresh()

    def on_key(self, ctl, keyval, keycode, state):
        name = EVDEV.get(keycode - 8)
        if name: self.keys_down.add(name)
        if not self.listen: return False
        kind, t = self.listen
        if keyval == Gdk.KEY_Escape: self.stop_listen(); return True
        if kind != 'keys': return True
        if keyval == Gdk.KEY_BackSpace: self.set_bind(kind, t, ''); self.status.set_label(''); return True
        if not name: self.status.set_label(f'That key ({Gdk.keyval_name(keyval)}) cannot be bound'); return True
        self.set_bind(kind, t, name); self.status.set_label(''); return True

    def on_key_up(self, ctl, keyval, keycode, state): self.keys_down.discard(EVDEV.get(keycode - 8))

    def player_pad(self):
        """The gamepad handles this player's controller setting matches"""
        c = self.pl.get('controller', 'none')
        return [g for guid, _, g in self.connected if c == 'any' or c == guid]

    def pad_pressed(self, thresh=16000):
        s = set()
        if self.pads.lib:
            for g in (self.player_pad() or [g for _, _, g in self.connected]):
                s |= pressed_sources(*self.pads.state(g), thresh)
        return s

    def tick(self):
        try: self.connected = self.pads.poll()
        except Exception: self.connected = []
        self.fill_controllers()
        live = self.pad_pressed()
        if self.listen and self.listen[0] == 'buttons':
            new = self.pad_pressed(24000) - self.baseline
            if new:   # a stick: the axis direction; else the first button pressed
                self.set_bind('buttons', self.listen[1], sorted(new, key=lambda s: s[0] not in '+-')[0]); self.status.set_label('')
            self.baseline &= live
        for t, b in self.btn['buttons'].items():
            on = self.pl['buttons'].get(t) in live
            if on: b.add_css_class('live')
            else: b.remove_css_class('live')
        for t, b in self.btn['keys'].items():
            if self.pl['keys'].get(t) in self.keys_down: b.add_css_class('live')
            else: b.remove_css_class('live')
        return True

    def shutdown(self):
        if self.timer: GLib.source_remove(self.timer); self.timer = 0
        self.pads.close()
