#!/usr/bin/env python3
# nxasahi: the game library (GTK 4 + libadwaita). Drop NSP / XCI / NSZ files on the window (or Add Game): a base game is imported (scripts/build_ipa_from_nsp.py
# --platform linux, its JSON progress shown as it builds); an update or DLC joins its game and the game is built again with it. Games are tiles: double-click to play,
# the menu (or a right-click) for rebuild, properties, the save and log folders, removal. Controllers (frontend/controllers.py) sets up and remaps each player's input.
# Where things are: ~/.local/share/nxasahi/games/<title ID>/ holds links to the game's files and the build (<id>_build/, linux_game.json); saves in
# ~/.local/share/nxasahi/saves/<title ID>/ (kept when a game is rebuilt or removed); settings in ~/.config/nxasahi/settings.json, a game's own in games/<title ID>/nxasahi.json.
# Run it through frontend/nxasahi.sh (inside nix develop: Python with PyGObject, the build tools)
import importlib.util, json, os, re, shlex, shutil, signal, sys, time
from pathlib import Path

import gi
gi.require_version('Gtk', '4.0'); gi.require_version('Adw', '1')
from gi.repository import Adw, Gdk, GdkPixbuf, Gio, GLib, Gtk, Pango

REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / 'scripts/build_ipa_from_nsp.py'
DATA = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'nxasahi'
GAMES, SAVES = DATA / 'games', DATA / 'saves'
CONFIG = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'nxasahi' / 'settings.json'
GAME_EXT = ('.nsp', '.xci', '.nsz', '.xcz')
ART = 180   # tile art size
DEFAULTS = {'keys': str(Path.home() / '.switch/prod.keys'), 'language': '', 'fullscreen': False, 'audio': True, 'big_cores': True, 'precompile': 2, 'extra_env': ''}

sys.path.insert(0, str(REPO / 'dm/tools'))
import nacp   # noqa: E402  (the game languages, the name and version in control data)
sys.path.insert(0, str(REPO / 'frontend'))
import controllers   # noqa: E402  (the controller settings window, the host's input map)
LANGS = nacp.LANGUAGES

_pipeline = None
def pipeline():   # the import script as a module: its title / version readers (no keys needed: they read the container's file names)
    global _pipeline
    if _pipeline is None:
        spec = importlib.util.spec_from_file_location('nxbuild', SCRIPT); _pipeline = importlib.util.module_from_spec(spec); spec.loader.exec_module(_pipeline)
    return _pipeline


def load_json(p, default):
    try: return {**default, **json.loads(Path(p).read_text())}
    except (OSError, ValueError): return dict(default)


def save_json(p, d):
    p = Path(p); p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix('.tmp'); tmp.write_text(json.dumps(d, indent=1) + '\n'); tmp.replace(p)


def env_pairs(text):   # "A=1 B='x y'" or one per line
    out = {}
    for tok in shlex.split(text or '', comments=True):
        if '=' in tok: k, v = tok.split('=', 1); out[k.strip()] = v
    return out


def identify(path):
    """(application title ID, kind, version) of a game file: kind 'base', 'update' or 'dlc'. Raises ValueError when the title ID cannot be read"""
    p = Path(path)
    tid = None
    try: tid = pipeline().title_of(p)
    except Exception: pass
    if tid is None:
        m = re.search(r'\[([0-9A-Fa-f]{16})\]', p.name)
        if m: tid = int(m.group(1), 16)
    if tid is None: raise ValueError(f'cannot read the title ID of {p.name} (no ticket inside; a [0100…] tag in the file name also works)')
    low = tid & 0xFFF
    app = tid if low == 0 else tid & ~0xFFF if low == 0x800 else (tid & ~0xFFF) - 0x1000
    m = re.search(r'\[v(\d+)\]', p.name)
    return app, 'base' if low == 0 else 'update' if low == 0x800 else 'dlc', int(m.group(1)) if m else None


class Game:
    def __init__(self, folder):
        self.folder = Path(folder); self.tid = self.folder.name
        self.reload()

    def reload(self):
        self.manifest = None
        for m in sorted(self.folder.glob('*_build/linux_game.json')):
            try: self.manifest = json.loads(m.read_text())
            except (OSError, ValueError): pass
        self.conf = load_json(self.folder / 'nxasahi.json', {'language': '', 'extra_env': '', 'last_played': 0, 'error': ''})
        files = self.files()
        base = next((f for f in files if f[1] == 'base'), None)
        self.name = (self.manifest or {}).get('name') or (re.sub(r'\s*\[.*$', '', base[0].stem).strip() if base else self.tid)
        self.version = (self.manifest or {}).get('version', '')
        icon = (self.manifest or {}).get('icon') or next((str(p) for p in self.folder.glob('*_build/extract/*/icon.jpg')), '')
        self.icon = icon if icon and Path(icon).is_file() else ''
        self.updates = sum(1 for f in files if f[1] == 'update'); self.dlc = sum(1 for f in files if f[1] == 'dlc')

    def files(self):
        out = []
        for p in sorted(self.folder.iterdir()):
            if p.suffix.lower() in GAME_EXT:
                try: out.append((p, identify(p)[1]))
                except ValueError: out.append((p, '?'))
        return out

    @property
    def ready(self): return bool(self.manifest and Path(self.manifest.get('host', '')).is_file())

    def work(self): return next(iter(sorted(self.folder.glob('*_build'))), self.folder / f'{self.tid}_build')

    def save_conf(self): save_json(self.folder / 'nxasahi.json', self.conf)


class Build:
    """One import / rebuild: the script with --progress json, its events into the card and the game's tile"""
    def __init__(self, app, game):
        self.app, self.game = app, game
        self.stage, self.frac, self.step, self.lines, self.proc, self.ok = 'waiting', None, '', [], None, None
        self.log = game.work() / 'logs' / 'nxasahi_build.log'

    def start(self):
        s = self.app.settings
        if not Path(os.path.expanduser(s['keys'])).is_file():
            self.finish(False, f'no prod.keys at {s["keys"]}: set it in Preferences'); return
        cmd = [sys.executable, str(SCRIPT), str(self.game.folder), '-k', os.path.expanduser(s['keys']), '--platform', 'linux', '--progress', 'json']
        self.log.parent.mkdir(parents=True, exist_ok=True)
        self.logf = open(self.log, 'w')
        self.stage = 'starting'
        self.proc = Gio.Subprocess.new(cmd, Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        self.stream = Gio.DataInputStream.new(self.proc.get_stdout_pipe())
        self.stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self._line)
        self.proc.wait_async(None, self._exited)
        self.app.build_changed(self)

    def _line(self, stream, res):
        try: line, _ = stream.read_line_finish_utf8(res)
        except GLib.Error: line = None
        if line is None: return
        self.logf.write(line + '\n'); self.logf.flush()
        ev = None
        if line.startswith('{'):
            try: ev = json.loads(line)
            except ValueError: ev = None
        if ev:
            e = ev.get('ev')
            if e == 'stage': self.stage = f'[{ev.get("k")}/{ev.get("n")}] {ev.get("name")}'; self.frac = None; self.step = ev.get('step') or ''
            elif e == 'step': self.step = ev.get('step') or ''
            elif e == 'progress': self.frac = ev.get('frac'); self.step = ' '.join(x for x in (ev.get('step') or '', ev.get('note') or '') if x)
            elif e == 'stage_end': self.frac = 1.0 if ev.get('ok') else self.frac
            elif e == 'say': self.lines.append(ev.get('msg', ''))
        else: self.lines.append(line)
        del self.lines[:-400]
        self.app.build_changed(self)
        stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self._line)

    def _exited(self, proc, res):
        try: proc.wait_finish(res)
        except GLib.Error: pass
        code = proc.get_exit_status() if proc.get_if_exited() else -1
        last = next((l for l in reversed(self.lines) if '[RED]' in l or 'Error' in l), '') if code else ''
        GLib.timeout_add(200, lambda: (self.finish(code == 0, re.sub(r'^\[[\d:]+\]\s*', '', last).replace('[RED] ', '') or (f'the build stopped (exit code {code})' if code else '')), False)[1])

    def finish(self, ok, msg=''):
        self.ok = ok; self.stage = 'done' if ok else 'failed'; self.error = msg
        try: self.logf.close()
        except Exception: pass
        self.game.conf['error'] = '' if ok else msg; self.game.save_conf()
        self.app.build_done(self)

    def cancel(self):
        if self.proc: self.proc.send_signal(signal.SIGINT)


class GameTile(Gtk.Box):
    def __init__(self, app, game):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=6, hexpand=False, halign=Gtk.Align.CENTER)
        self.app, self.game = app, game
        self.set_size_request(196, -1); self.add_css_class('game-tile')
        ov = Gtk.Overlay()
        self.pic = Gtk.Picture(content_fit=Gtk.ContentFit.COVER, width_request=ART, height_request=ART)   # the icon is scaled to ART when loaded: a picture takes its paintable's own size
        holder = Gtk.Box(halign=Gtk.Align.CENTER, valign=Gtk.Align.START, overflow=Gtk.Overflow.HIDDEN, css_classes=['game-art']); holder.append(self.pic)
        ov.set_child(holder)
        self.badge = Gtk.Label(css_classes=['game-badge'], halign=Gtk.Align.START, valign=Gtk.Align.START, margin_start=14, margin_top=8); ov.add_overlay(self.badge)
        self.spin = Gtk.Spinner(halign=Gtk.Align.CENTER, valign=Gtk.Align.CENTER, width_request=48, height_request=48); ov.add_overlay(self.spin)
        self.append(ov)
        row = Gtk.Box(spacing=4, margin_start=8, margin_end=4)
        col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, hexpand=True)
        self.title = Gtk.Label(xalign=0, ellipsize=Pango.EllipsizeMode.END, max_width_chars=18, wrap=True, wrap_mode=Pango.WrapMode.WORD_CHAR, lines=2, css_classes=['heading'])
        self.sub = Gtk.Label(xalign=0, ellipsize=Pango.EllipsizeMode.END, max_width_chars=22, css_classes=['dim-label', 'caption'])
        col.append(self.title); col.append(self.sub); row.append(col)
        self.menu = Gtk.MenuButton(icon_name='view-more-symbolic', valign=Gtk.Align.CENTER, css_classes=['flat', 'circular'], menu_model=app.game_menu(game.tid))
        row.append(self.menu); self.append(row)
        rc = Gtk.GestureClick(button=3); rc.connect('pressed', lambda *a: self.menu.popup()); self.add_controller(rc)
        self.update()

    def update(self):
        g, b = self.game, self.app.builds.get(self.game.tid)
        self.title.set_label(g.name); self.set_tooltip_text(f'{g.name}\n{g.tid}')
        if g.icon != getattr(self, 'shown_icon', None):
            self.shown_icon = g.icon
            try: self.pic.set_paintable(Gdk.Texture.new_from_bytes(GLib.Bytes.new(GdkPixbuf.Pixbuf.new_from_file_at_scale(g.icon, ART, ART, True).save_to_bufferv('png', [], [])[1])) if g.icon else None)
            except GLib.Error: self.pic.set_paintable(None)
        extra = (f' · {g.updates} update{"s" if g.updates != 1 else ""}' if g.updates else '') + (f' · {g.dlc} DLC' if g.dlc else '')
        if b and b.ok is None:
            self.spin.set_visible(True); self.spin.start()
            self.sub.set_label(b.stage if b.stage != 'waiting' else 'waiting to build'); self.badge.set_visible(False)
        else:
            self.spin.stop(); self.spin.set_visible(False)
            if g.ready: self.sub.set_label((f'v{g.version}' if g.version else 'ready') + extra); self.badge.set_visible(False)
            else:
                self.sub.set_label(('build failed' if g.conf.get('error') else 'not built') + extra)
                self.badge.set_label('!' if g.conf.get('error') else 'new'); self.badge.set_visible(True)
        if self.app.running.get(g.tid): self.sub.set_label('playing'); self.badge.set_label('▶'); self.badge.set_visible(True)


class BuildCard(Gtk.Box):
    def __init__(self, app, build):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=4, css_classes=['card', 'build-card'], margin_start=12, margin_end=12, margin_bottom=6)
        self.build = build
        top = Gtk.Box(spacing=8, margin_top=8, margin_start=12, margin_end=8)
        self.title = Gtk.Label(xalign=0, hexpand=True, css_classes=['heading'], ellipsize=Pango.EllipsizeMode.END)
        self.cancel = Gtk.Button(icon_name='process-stop-symbolic', css_classes=['flat', 'circular'], tooltip_text='Stop this build'); self.cancel.connect('clicked', lambda *a: build.cancel())
        self.logbtn = Gtk.ToggleButton(icon_name='utilities-terminal-symbolic', css_classes=['flat', 'circular'], tooltip_text='Show the build output')
        top.append(self.title); top.append(self.logbtn); top.append(self.cancel); self.append(top)
        self.stage = Gtk.Label(xalign=0, css_classes=['caption'], margin_start=12, ellipsize=Pango.EllipsizeMode.END); self.append(self.stage)
        self.bar = Gtk.ProgressBar(margin_start=12, margin_end=12, margin_bottom=8); self.append(self.bar)
        self.view = Gtk.TextView(editable=False, monospace=True, cursor_visible=False, wrap_mode=Gtk.WrapMode.WORD_CHAR, css_classes=['build-log'])
        sc = Gtk.ScrolledWindow(child=self.view, min_content_height=160, max_content_height=240, propagate_natural_height=True, margin_start=12, margin_end=12, margin_bottom=8)
        self.rev = Gtk.Revealer(child=sc); self.append(self.rev)
        self.logbtn.bind_property('active', self.rev, 'reveal-child', 0)
        self.update()

    def update(self):
        b = self.build
        self.title.set_label(f'Building {b.game.name}' if b.ok is None else (f'{b.game.name} is ready' if b.ok else f'{b.game.name}: build failed'))
        self.stage.set_label(b.stage + (f' — {b.step}' if b.step else '') if b.ok is None else (b.error if not b.ok else 'Double-click its tile to play'))
        if b.ok is not None: self.bar.set_fraction(1.0 if b.ok else self.bar.get_fraction()); self.cancel.set_visible(False)
        elif b.frac is None: self.bar.pulse()
        else: self.bar.set_fraction(b.frac)
        if self.rev.get_reveal_child():
            buf = self.view.get_buffer(); buf.set_text('\n'.join(b.lines[-200:]))
            GLib.idle_add(lambda: (self.view.scroll_to_iter(buf.get_end_iter(), 0, False, 0, 0), False)[1])


class App(Adw.Application):
    def __init__(self):
        super().__init__(application_id='org.nxasahi.Library', flags=Gio.ApplicationFlags.HANDLES_OPEN)
        self.settings = load_json(CONFIG, DEFAULTS)
        self.games, self.tiles, self.builds, self.cards, self.queue, self.running = {}, {}, {}, {}, [], {}
        self.connect('activate', self.on_activate); self.connect('open', self.on_open)

    # ---- window ----
    def on_activate(self, *a):
        if getattr(self, 'win', None): self.win.present(); return
        css = Gtk.CssProvider()
        css.load_from_string('''
            .game-tile { padding: 8px; border-radius: 14px; }
            flowboxchild:hover .game-tile { background: alpha(currentColor, 0.06); }
            flowboxchild:selected .game-tile { background: alpha(@accent_bg_color, 0.18); }
            .game-art { border-radius: 12px; background: alpha(currentColor, 0.08); }
            .game-badge { background: @accent_bg_color; color: @accent_fg_color; border-radius: 99px; padding: 1px 8px; font-weight: bold; font-size: 0.8em; }
            .drop-hint { background: alpha(@accent_bg_color, 0.15); border: 3px dashed @accent_bg_color; border-radius: 18px; margin: 18px; }
            .build-log { font-size: 0.85em; padding: 6px; }''')
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        for name, fn, param in (('play', self.play, 's'), ('rebuild', lambda t: self.enqueue(self.games[t]), 's'), ('properties', self.properties, 's'),
                                ('open-saves', lambda t: self.open_dir(SAVES / t), 's'), ('open-logs', lambda t: self.open_dir(self.games[t].work() / 'logs'), 's'),
                                ('open-folder', lambda t: self.open_dir(self.games[t].folder), 's'), ('remove', self.remove, 's'), ('stop', self.stop, 's'),
                                ('add', lambda: self.add_dialog(), None), ('preferences', lambda: self.preferences(), None), ('controllers', lambda: self.controllers(), None), ('library', lambda: self.open_dir(GAMES), None),
                                ('desktop', lambda: self.install_desktop(), None), ('about', lambda: self.about(), None)):
            act = Gio.SimpleAction.new(name, GLib.VariantType.new(param) if param else None)
            act.connect('activate', (lambda f: lambda a, p: f(p.get_string()))(fn) if param else (lambda f: lambda a, p: f())(fn))
            self.add_action(act)
        self.set_accels_for_action('app.add', ['<Control>o']); self.set_accels_for_action('app.preferences', ['<Control>comma']); self.set_accels_for_action('app.controllers', ['<Control>k'])
        self.win = Adw.ApplicationWindow(application=self, default_width=1120, default_height=740, title='nxasahi')
        tv = Adw.ToolbarView()
        hb = Adw.HeaderBar(); self.wtitle = Adw.WindowTitle(title='nxasahi'); hb.set_title_widget(self.wtitle)
        add = Gtk.Button(icon_name='list-add-symbolic', tooltip_text='Add a game (Ctrl+O)', action_name='app.add'); hb.pack_start(add)
        menu = Gio.Menu(); menu.append('Preferences', 'app.preferences'); menu.append('Controllers', 'app.controllers'); menu.append('Open Library Folder', 'app.library'); menu.append('Add to App Launcher', 'app.desktop'); menu.append('About nxasahi', 'app.about')
        hb.pack_end(Gtk.MenuButton(icon_name='open-menu-symbolic', menu_model=menu, primary=True))
        hb.pack_end(Gtk.Button(icon_name='input-gaming-symbolic', tooltip_text='Controllers (Ctrl+K)', action_name='app.controllers'))
        self.search = Gtk.SearchEntry(placeholder_text='Search games', width_chars=24); self.search.connect('search-changed', lambda *a: self.flow.invalidate_filter()); hb.pack_end(self.search)
        tv.add_top_bar(hb)
        self.toasts = Adw.ToastOverlay()
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.stack = Gtk.Stack(vexpand=True, transition_type=Gtk.StackTransitionType.CROSSFADE)
        empty = Adw.StatusPage(icon_name='input-gaming-symbolic', title='Drop a Game Here', description='Drag NSP, XCI or NSZ files onto this window to add them, or choose them with the button below.\nUpdates and DLC join their game when dropped too.')
        eb = Gtk.Button(label='Add Game…', halign=Gtk.Align.CENTER, css_classes=['pill', 'suggested-action'], action_name='app.add'); empty.set_child(eb)
        self.stack.add_named(empty, 'empty')
        self.flow = Gtk.FlowBox(valign=Gtk.Align.START, halign=Gtk.Align.START, selection_mode=Gtk.SelectionMode.SINGLE, homogeneous=True, max_children_per_line=12, min_children_per_line=1,
                                column_spacing=8, row_spacing=8, margin_top=18, margin_bottom=18, margin_start=18, margin_end=18, activate_on_single_click=False)
        self.flow.connect('child-activated', lambda f, c: self.play(c.get_child().game.tid))
        self.flow.set_filter_func(lambda c: self.search.get_text().lower() in c.get_child().game.name.lower() or self.search.get_text().upper() in c.get_child().game.tid)
        self.flow.set_sort_func(lambda a, b: (b.get_child().game.conf.get('last_played', 0) > a.get_child().game.conf.get('last_played', 0)) - (b.get_child().game.conf.get('last_played', 0) < a.get_child().game.conf.get('last_played', 0)) or (a.get_child().game.name.lower() > b.get_child().game.name.lower()) - (a.get_child().game.name.lower() < b.get_child().game.name.lower()))
        self.stack.add_named(Gtk.ScrolledWindow(child=self.flow, hscrollbar_policy=Gtk.PolicyType.NEVER), 'library')
        over = Gtk.Overlay(child=self.stack, vexpand=True)
        self.hint = Adw.StatusPage(icon_name='document-save-symbolic', title='Drop to Add', css_classes=['drop-hint'], visible=False, can_target=False); over.add_overlay(self.hint)
        body.append(over)
        self.cardbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL); body.append(self.cardbox)
        self.toasts.set_child(body); tv.set_content(self.toasts); self.win.set_content(tv)
        dt = Gtk.DropTarget.new(Gdk.FileList, Gdk.DragAction.COPY)
        dt.connect('enter', lambda *a: (self.hint.set_visible(True), Gdk.DragAction.COPY)[1]); dt.connect('leave', lambda *a: self.hint.set_visible(False))
        dt.connect('drop', self.on_drop); self.win.add_controller(dt)
        self.scan(); self.win.present()
        GLib.timeout_add(250, self.tick)

    def tick(self):
        for t, b in self.builds.items():
            if b.ok is None and b.frac is None and t in self.cards: self.cards[t].bar.pulse()
        return True

    def toast(self, msg, timeout=4): self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(msg), timeout=timeout))

    def game_menu(self, tid):
        m = Gio.Menu()
        s1 = Gio.Menu(); s1.append('Play', f'app.play::{tid}'); s1.append('Stop', f'app.stop::{tid}'); m.append_section(None, s1)
        s2 = Gio.Menu(); s2.append('Properties', f'app.properties::{tid}'); s2.append('Rebuild', f'app.rebuild::{tid}'); m.append_section(None, s2)
        s3 = Gio.Menu(); s3.append('Open Save Folder', f'app.open-saves::{tid}'); s3.append('Open Build Logs', f'app.open-logs::{tid}'); s3.append('Open Game Folder', f'app.open-folder::{tid}'); m.append_section(None, s3)
        s4 = Gio.Menu(); s4.append('Remove from Library…', f'app.remove::{tid}'); m.append_section(None, s4)
        return m

    def scan(self):
        GAMES.mkdir(parents=True, exist_ok=True)
        for d in sorted(GAMES.iterdir()):
            if d.is_dir() and re.fullmatch(r'[0-9A-F]{16}', d.name) and d.name not in self.games and any(p.suffix.lower() in GAME_EXT for p in d.iterdir()):
                self.add_tile(Game(d))
        self.refresh()

    def add_tile(self, g):
        self.games[g.tid] = g; t = GameTile(self, g); self.tiles[g.tid] = t; self.flow.append(t)

    def refresh(self):
        n = len(self.games)
        self.stack.set_visible_child_name('library' if n else 'empty')
        self.wtitle.set_subtitle(f'{n} game{"s" if n != 1 else ""}' if n else '')
        for t in self.tiles.values(): t.update()
        self.flow.invalidate_sort()

    # ---- adding games ----
    def on_drop(self, target, value, x, y):
        self.hint.set_visible(False)
        self.add_paths([f.get_path() for f in value.get_files() if f.get_path()])
        return True

    def on_open(self, app, files, n, hint):   # files given on the command line / opened with the app
        self.activate(); self.add_paths([f.get_path() for f in files if f.get_path()])

    def add_dialog(self):
        d = Gtk.FileDialog(title='Add Games')
        flt = Gtk.FileFilter(name='Switch games (NSP, XCI, NSZ, XCZ)')
        for e in GAME_EXT: flt.add_suffix(e[1:])
        fl = Gio.ListStore.new(Gtk.FileFilter); fl.append(flt); d.set_filters(fl)
        def done(dlg, res):
            try: files = dlg.open_multiple_finish(res)
            except GLib.Error: return
            self.add_paths([files.get_item(i).get_path() for i in range(files.get_n_items())])
        d.open_multiple(self.win, None, done)

    def add_paths(self, paths):
        touched, bad = {}, []
        for p in paths:
            p = Path(p)
            if p.is_dir():   # a folder: the game files in it
                paths += [str(q) for q in sorted(p.iterdir()) if q.suffix.lower() in GAME_EXT]; continue
            if p.suffix.lower() not in GAME_EXT: bad.append(f'{p.name}: not a game file'); continue
            try: app, kind, ver = identify(p)
            except ValueError as e: bad.append(str(e)); continue
            tid = f'{app:016X}'; folder = GAMES / tid
            if kind != 'base' and not (folder.is_dir() and any(identify_kind(q) == 'base' for q in folder.iterdir() if q.suffix.lower() in GAME_EXT)):
                bad.append(f'{p.name}: an {kind} of {tid}; add the base game first'); continue
            folder.mkdir(parents=True, exist_ok=True)
            link = folder / p.name
            if kind == 'update':   # one update at a time: the newest is the one built
                for q in folder.iterdir():
                    if q.suffix.lower() in GAME_EXT and q != link and identify_kind(q) == 'update': q.unlink()
            if not link.exists():
                try: os.link(p, link)   # a hard link when on the same file system (the game keeps working if the original moves), else a symbolic link
                except OSError: link.symlink_to(p.resolve())
            touched[tid] = kind
        for tid, kind in touched.items():
            if tid not in self.games: self.add_tile(Game(GAMES / tid))
            else: self.games[tid].reload()
            self.toast(f'{self.games[tid].name}: ' + ('added, building…' if kind == 'base' else f'{kind} added, rebuilding…'))
            self.enqueue(self.games[tid])
        for b in bad: self.toast(b, 6)
        self.refresh()

    # ---- builds ----
    def enqueue(self, g):
        if g.tid in self.builds and self.builds[g.tid].ok is None: self.toast(f'{g.name} is already building'); return
        if g.tid in self.cards: self.cardbox.remove(self.cards.pop(g.tid))
        b = Build(self, g); self.builds[g.tid] = b
        card = BuildCard(self, b); self.cards[g.tid] = card; self.cardbox.append(card)
        self.queue.append(b); self.pump(); self.refresh()

    def pump(self):
        if any(b.proc and b.ok is None for b in self.builds.values()): return
        while self.queue:
            b = self.queue.pop(0)
            if b.ok is None: b.start(); return

    def build_changed(self, b):
        if b.game.tid in self.cards: self.cards[b.game.tid].update()
        if b.game.tid in self.tiles: self.tiles[b.game.tid].update()

    def build_done(self, b):
        b.game.reload(); self.build_changed(b); self.refresh()
        self.toast(f'{b.game.name} is ready to play' if b.ok else f'{b.game.name}: build failed — {b.error}', 5 if b.ok else 8)
        if b.ok: GLib.timeout_add_seconds(8, lambda: (self.drop_card(b), False)[1])
        self.pump()

    def drop_card(self, b):
        c = self.cards.get(b.game.tid)
        if c and c.build is b and b.ok: self.cardbox.remove(c); del self.cards[b.game.tid]

    # ---- playing ----
    def play(self, tid):
        g = self.games[tid]
        if self.running.get(tid): self.toast(f'{g.name} is already running'); return
        if not g.ready:
            b = self.builds.get(tid)
            self.toast(f'{g.name} is still building' if b and b.ok is None else f'{g.name} is not built yet: building it');
            if not (b and b.ok is None): self.enqueue(g)
            return
        m, s = g.manifest, self.settings
        env = dict(os.environ); env.update(m['env'])
        save = SAVES / tid; save.mkdir(parents=True, exist_ok=True)
        env['DM_SAVE'] = str(save)   # saves outside the build: kept when the game is rebuilt or removed
        env['DM_WINDOW'] = '1'
        env.setdefault('DM_TEXT_INPUT', f'{sys.executable} {REPO / "frontend/text_input.py"}')   # the software keyboard's input box (games built before the manifest carried it)
        env['DM_AUDIO'] = '1' if s['audio'] else '0'
        if s['fullscreen']: env['DM_FULLSCREEN'] = '1'
        if not s['big_cores']: env['DM_CPUS'] = 'all'
        env['DM_VK_PRECOMPILE'] = str(int(s['precompile']))
        imap = controllers.map_for_launch()   # the controller bindings (dm/host/linux/front_input.c); none set: the host's built-in map
        if imap: env['DM_INPUT_MAP'] = str(imap)
        lang = g.conf.get('language') or s['language']
        if lang: env['DM_LANG'] = lang
        env.update(env_pairs(s['extra_env'])); env.update(env_pairs(g.conf.get('extra_env')))
        logp = Path(m['cwd']) / 'last_run.log'; Path(m['cwd']).mkdir(parents=True, exist_ok=True)
        launcher = Gio.SubprocessLauncher.new(Gio.SubprocessFlags.NONE)
        launcher.set_environ([f'{k}={v}' for k, v in env.items()]); launcher.set_cwd(m['cwd'])
        launcher.set_stdout_file_path(str(logp)); launcher.set_stderr_file_path(str(logp))   # same path: GLib opens it once per stream (appending)
        try: proc = launcher.spawnv([m['host'], m['modules']])
        except GLib.Error as e: self.toast(f'{g.name} did not start: {e.message}', 8); return
        self.running[tid] = (proc, time.time()); g.conf['last_played'] = int(time.time()); g.save_conf()
        self.toast(f'Starting {g.name}…', 3); self.refresh()
        proc.wait_async(None, lambda p, r: self.exited(tid, p, r, logp))

    def exited(self, tid, proc, res, logp):
        try: proc.wait_finish(res)
        except GLib.Error: pass
        _, t0 = self.running.pop(tid, (None, 0)); g = self.games.get(tid)
        code = proc.get_exit_status() if proc.get_if_exited() else -proc.get_term_sig() if proc.get_if_signaled() else 0
        if g and code not in (0, -signal.SIGTERM, -signal.SIGINT) and time.time() - t0 < 30:
            tail = ''
            try: tail = next((l for l in reversed(logp.read_text(errors='replace').splitlines()) if '[RED]' in l or 'die' in l.lower() or 'error' in l.lower()), '')
            except OSError: pass
            self.toast(f'{g.name} stopped (code {code}). {tail[:140]}  Log: {logp}', 10)
        self.refresh()

    def stop(self, tid):
        r = self.running.get(tid)
        if r: r[0].send_signal(signal.SIGTERM)

    # ---- game properties ----
    def properties(self, tid):
        g = self.games[tid]; m = g.manifest or {}
        d = Adw.PreferencesDialog(title=g.name, search_enabled=False)
        page = Adw.PreferencesPage(title='Game', icon_name='input-gaming-symbolic')
        info = Adw.PreferencesGroup(title='Information')
        def row(t, v):
            r = Adw.ActionRow(title=t, subtitle=v or '—', subtitle_selectable=True); r.add_css_class('property'); info.add(r)
        row('Title ID', g.tid); row('Version', g.version); row('Build', m.get('package', '')); row('Profile', m.get('profile', ''))
        row('Updates / DLC', f'{g.updates} / {g.dlc}'); row('Built', m.get('built', '').replace('T', ' ') or 'not built')
        if g.conf.get('error'): row('Last build error', g.conf['error'])
        page.add(info)
        opt = Adw.PreferencesGroup(title='Options', description='Override the global preferences for this game')
        names = ['Use the global setting'] + [f'{n} ({c})' for c, n in LANGS]
        lr = Adw.ComboRow(title='Language', model=Gtk.StringList.new(names))
        codes = [''] + [c for c, _ in LANGS]; lr.set_selected(codes.index(g.conf.get('language')) if g.conf.get('language') in codes else 0)
        lr.connect('notify::selected', lambda r, p: (g.conf.__setitem__('language', codes[r.get_selected()]), g.save_conf()))
        opt.add(lr)
        er = Adw.EntryRow(title='Extra environment (KEY=value …)', text=g.conf.get('extra_env', ''), show_apply_button=True)
        er.connect('apply', lambda r: (g.conf.__setitem__('extra_env', r.get_text()), g.save_conf(), self.toast('Saved')))
        opt.add(er); page.add(opt)
        act = Adw.PreferencesGroup(title='Actions')
        for title, sub, action in (('Rebuild', 'Run the import again (after a tool or profile update)', f'app.rebuild::{tid}'), ('Open Save Folder', str(SAVES / tid), f'app.open-saves::{tid}'),
                                   ('Open Build Logs', str(g.work() / 'logs'), f'app.open-logs::{tid}')):
            r = Adw.ActionRow(title=title, subtitle=sub, activatable=True, action_name=action.split('::')[0], action_target=GLib.Variant.new_string(tid))
            r.add_suffix(Gtk.Image(icon_name='go-next-symbolic')); act.add(r)
        page.add(act)
        d.add(page); d.present(self.win)

    def remove(self, tid):
        g = self.games[tid]
        d = Adw.AlertDialog(heading=f'Remove {g.name}?', body=f'This deletes its build files ({g.folder}). Your saves stay in {SAVES / tid}; the original game files are not touched.')
        d.add_response('cancel', 'Cancel'); d.add_response('remove', 'Remove'); d.set_response_appearance('remove', Adw.ResponseAppearance.DESTRUCTIVE)
        def resp(dlg, r):
            if r != 'remove': return
            b = self.builds.get(tid)
            if b and b.ok is None: self.toast('Stop its build first'); return
            if self.running.get(tid): self.toast('Quit the game first'); return
            shutil.rmtree(g.folder, ignore_errors=True)
            self.flow.remove(self.tiles.pop(tid).get_parent()); del self.games[tid]
            if tid in self.cards: self.cardbox.remove(self.cards.pop(tid))
            self.toast(f'Removed {g.name}'); self.refresh()
        d.connect('response', resp); d.present(self.win)

    # ---- preferences ----
    def preferences(self):
        s = self.settings
        def put(k, v): s[k] = v; save_json(CONFIG, s)
        d = Adw.PreferencesDialog(title='Preferences')
        page = Adw.PreferencesPage(title='General', icon_name='preferences-system-symbolic')
        g1 = Adw.PreferencesGroup(title='Files')
        kr = Adw.ActionRow(title='Keys file (prod.keys)', subtitle=s['keys'], subtitle_selectable=True)
        kb = Gtk.Button(label='Choose…', valign=Gtk.Align.CENTER)
        def choose(*a):
            fd = Gtk.FileDialog(title='Choose prod.keys')
            def done(dlg, res):
                try: f = dlg.open_finish(res)
                except GLib.Error: return
                put('keys', f.get_path()); kr.set_subtitle(f.get_path())
            fd.open(self.win, None, done)
        kb.connect('clicked', choose); kr.add_suffix(kb)
        if not Path(os.path.expanduser(s['keys'])).is_file(): kr.add_suffix(Gtk.Image(icon_name='dialog-warning-symbolic', tooltip_text='not found'))
        g1.add(kr)
        lib = Adw.ActionRow(title='Library folder', subtitle=str(DATA), activatable=True, action_name='app.library'); lib.add_suffix(Gtk.Image(icon_name='folder-open-symbolic')); g1.add(lib)
        page.add(g1)
        g2 = Adw.PreferencesGroup(title='Games')
        names = ['English (the default)'] + [f'{n} ({c})' for c, n in LANGS]; codes = [''] + [c for c, _ in LANGS]
        lr = Adw.ComboRow(title='Language', subtitle='A game that lacks it falls back to English', model=Gtk.StringList.new(names)); lr.set_selected(codes.index(s['language']) if s['language'] in codes else 0)
        lr.connect('notify::selected', lambda r, p: put('language', codes[r.get_selected()])); g2.add(lr)
        for k, t, sub in (('fullscreen', 'Fullscreen', 'Start games in fullscreen'), ('audio', 'Sound', 'Play sound through the default output (SDL)')):
            sw = Adw.SwitchRow(title=t, subtitle=sub, active=bool(s[k])); sw.connect('notify::active', (lambda k: lambda r, p: put(k, r.get_active()))(k)); g2.add(sw)
        page.add(g2)
        g3 = Adw.PreferencesGroup(title='Performance')
        bc = Adw.SwitchRow(title='Fast cores only', subtitle='Keep games on the performance cores (much faster on Apple silicon)', active=bool(s['big_cores']))
        bc.connect('notify::active', lambda r, p: put('big_cores', r.get_active())); g3.add(bc)
        pc = Adw.SpinRow.new_with_range(0, 8, 1); pc.set_title('Shader precompile threads'); pc.set_subtitle('Background threads that fill the shader cache while you play (0 = off)'); pc.set_value(int(s['precompile']))
        pc.connect('notify::value', lambda r, p: put('precompile', int(r.get_value()))); g3.add(pc)
        page.add(g3)
        g4 = Adw.PreferencesGroup(title='Advanced')
        er = Adw.EntryRow(title='Extra environment for every game (KEY=value …)', text=s['extra_env'], show_apply_button=True)
        er.connect('apply', lambda r: (put('extra_env', r.get_text()), self.toast('Saved'))); g4.add(er)
        page.add(g4)
        d.add(page); d.present(self.win)

    # ---- controllers ----
    def controllers(self): controllers.ControllersDialog(self.toast).present(self.win)

    # ---- misc ----
    def open_dir(self, p):
        p = Path(p); p.mkdir(parents=True, exist_ok=True)
        Gtk.FileLauncher.new(Gio.File.new_for_path(str(p))).launch(self.win, None, None)

    def install_desktop(self):
        apps = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'applications'; apps.mkdir(parents=True, exist_ok=True)
        (apps / 'org.nxasahi.Library.desktop').write_text(
            '[Desktop Entry]\nType=Application\nName=nxasahi\nComment=Switch game library\nIcon=input-gaming\nCategories=Game;\n'
            f'Exec={REPO / "frontend/nxasahi.sh"} %F\nMimeType=application/x-nx-nsp;application/x-nx-xci;\nTerminal=false\n')
        self.toast('Added nxasahi to the app launcher')

    def about(self):
        Adw.AboutDialog(application_name='nxasahi', application_icon='input-gaming', version='0.1', comments='Switch games running natively on Asahi Linux',
                        website='https://github.com/Sheepheerd/nxasahi', license_type=Gtk.License.CUSTOM).present(self.win)


def identify_kind(p):
    try: return identify(p)[1]
    except ValueError: return '?'


if __name__ == '__main__':
    sys.exit(App().run(sys.argv))
