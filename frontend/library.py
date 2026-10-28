# nxasahi's game library, without its screens: where games, saves and settings live, what a game folder holds, the imports and shader-cache builds (the import script
# with --progress json, and the host's precompiler), and launching a game. frontend/backend.py puts it in front of the QML screens (frontend/qml).
# Where things are: ~/.local/share/nxasahi/games/<title ID>/ holds links to the game's files and the build (<id>_build/, linux_game.json); saves in
# ~/.local/share/nxasahi/saves/<title ID>/ (kept when a game is rebuilt or removed); settings in ~/.config/nxasahi/settings.json, a game's own in games/<title ID>/nxasahi.json.
import importlib.util, json, os, re, shlex, shutil, signal, sys, time
from pathlib import Path

from PySide6.QtCore import QObject, QProcess, QProcessEnvironment, QTimer, Signal

REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / 'scripts/build_ipa_from_nsp.py'
DATA = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'nxasahi'
GAMES, SAVES = DATA / 'games', DATA / 'saves'
CONFIG = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'nxasahi' / 'settings.json'
GAME_EXT = ('.nsp', '.xci', '.nsz', '.xcz')
DEFAULTS = {'keys': str(Path.home() / '.switch/prod.keys'), 'language': '', 'fullscreen': False, 'audio': True, 'big_cores': True, 'precompile': 2, 'full_cache': True,
            'extra_env': '', 'library_fullscreen': False}

sys.path.insert(0, str(REPO / 'dm/tools'))
import nacp   # noqa: E402  (the game languages, the name and version in control data)
sys.path.insert(0, str(REPO / 'frontend'))
import controllers   # noqa: E402  (the controller settings and the host's input map)
LANGS = nacp.LANGUAGES


def launcher():   # what starts the library: the installed app (flake.nix sets NXASAHI_LAUNCHER; the command on PATH when it is the same app), else the source checkout's script
    inst = os.environ.get('NXASAHI_LAUNCHER')
    if inst:
        on_path = shutil.which('nxasahi')
        return 'nxasahi' if on_path and Path(on_path).resolve() == Path(inst).resolve() else inst
    return str(REPO / 'frontend/nxasahi.sh')


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


def identify_kind(p):
    try: return identify(p)[1]
    except ValueError: return '?'


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


class Build(QObject):
    """One import / rebuild: the script with --progress json; its events update stage / frac / step / lines and emit changed, then finished(ok, message)"""
    changed, finished = Signal(), Signal(bool, str)
    kind, what, done_title, done_text = 'build', 'Building', 'is ready', 'Press A to play'

    def __init__(self, settings, game):
        super().__init__()
        self.settings, self.game = settings, game
        self.stage, self.frac, self.step, self.lines, self.proc, self.ok, self.error = 'waiting', None, '', [], None, None, ''
        self.log = game.work() / 'logs' / 'nxasahi_build.log'
        self._buf = b''

    def command(self):
        s = self.settings
        return [sys.executable, str(SCRIPT), str(self.game.folder), '-k', os.path.expanduser(s['keys']), '--platform', 'linux', '--progress', 'json',
                '--shader-cache' if s['full_cache'] else '--no-shader-cache'], None, None

    def start(self):
        if self.kind == 'build' and not Path(os.path.expanduser(self.settings['keys'])).is_file():
            self.finish(False, f'no prod.keys at {self.settings["keys"]}: set it in Settings'); return
        cmd, env, cwd = self.command()
        self.log.parent.mkdir(parents=True, exist_ok=True)
        self.logf = open(self.log, 'w')
        self.stage = 'starting'
        self.proc = QProcess(); self.proc.setProcessChannelMode(QProcess.MergedChannels)
        if env is not None:
            pe = QProcessEnvironment()
            for k, v in env.items(): pe.insert(k, v)
            self.proc.setProcessEnvironment(pe)
        if cwd: self.proc.setWorkingDirectory(cwd)
        self.proc.readyReadStandardOutput.connect(self._read)
        self.proc.finished.connect(self._exited)
        self.proc.errorOccurred.connect(lambda e: self.finish(False, f'it did not start ({e.name})') if e == QProcess.FailedToStart else None)
        self.proc.start(cmd[0], cmd[1:])
        self.changed.emit()

    def _read(self):
        self._buf += bytes(self.proc.readAllStandardOutput())
        *lines, self._buf = self._buf.split(b'\n')
        for raw in lines: self._line(raw.decode('utf-8', 'replace'))
        self.changed.emit()

    def _line(self, line):
        self.logf.write(line + '\n'); self.logf.flush()
        ev = None
        if line.startswith('{'):
            try: ev = json.loads(line)
            except ValueError: ev = None
        pc = None if ev else re.fullmatch(r'precompile: (\d+) / (\d+)', line)   # the host's precompiler alone (CacheBuild): its count, not a line of the output
        if pc:
            d, t = int(pc[1]), int(pc[2]); self.frac = min(d / t, 0.99) if t else None; self.step = f'{d:,} / {t:,} shaders'
        elif ev:
            e = ev.get('ev')
            if e == 'stage': self.stage = f'[{ev.get("k")}/{ev.get("n")}] {ev.get("name")}'; self.frac = None; self.step = ev.get('step') or ''
            elif e == 'step': self.step = ev.get('step') or ''
            elif e == 'progress': self.frac = ev.get('frac'); self.step = ' '.join(x for x in (ev.get('step') or '', ev.get('note') or '') if x)
            elif e == 'stage_end': self.frac = 1.0 if ev.get('ok') else self.frac
            elif e == 'say': self.lines.append(ev.get('msg', ''))
        else: self.lines.append(line)
        del self.lines[:-400]

    def _exited(self, code, status):
        if self._buf: self._line(self._buf.decode('utf-8', 'replace')); self._buf = b''
        if status != QProcess.NormalExit: code = -1
        last = next((l for l in reversed(self.lines) if '[RED]' in l or 'Error' in l), '') if code else ''
        msg = re.sub(r'^\[[\d:]+\]\s*', '', last).replace('[RED] ', '') or (f'the build stopped (exit code {code})' if code else '')
        QTimer.singleShot(200, lambda: self.finish(code == 0, msg))

    def finish(self, ok, msg=''):
        if self.ok is not None: return
        self.ok = ok; self.stage = 'done' if ok else 'failed'; self.error = msg
        try: self.logf.close()
        except Exception: pass
        if self.kind == 'build': self.game.conf['error'] = '' if ok else msg; self.game.save_conf()
        self.changed.emit(); self.finished.emit(ok, msg)

    def cancel(self):
        if self.proc and self.proc.state() != QProcess.NotRunning: os.kill(self.proc.processId(), signal.SIGINT)


class CacheBuild(Build):
    """Build Shader Cache: the game's host with DM_VK_PRECOMPILE_ONLY=1 (dm/host/linux/vk_draw.c vkx_precompile_tool), every shader compiled for this GPU into
    ~/.cache/nxasahi/vkshaders on every core, so nothing is compiled while the game runs. What is cached already is skipped. A failure is only reported"""
    kind, what, done_title, done_text = 'cache', 'Building the shader cache of', 'shader cache is full', 'Every shader is compiled: nothing is left to compile in play'

    def __init__(self, settings, game):
        super().__init__(settings, game); self.log = game.work() / 'logs' / 'shader_cache.log'

    def command(self):
        m = self.game.manifest
        env = dict(os.environ); env.update(m['env']); env.pop('DM_WINDOW', None)
        env['DM_VK_PRECOMPILE_ONLY'] = '1'; env['DM_AUDIO'] = '0'
        self.stage = 'compiling the shaders for this GPU'
        return [m['host']], env, m['cwd']

    def cancel(self):   # SIGTERM: what is compiled so far stays cached (each shader is written whole, then renamed)
        if self.proc and self.proc.state() != QProcess.NotRunning: self.proc.terminate()


def shader_cache_supported(g):   # a host built before the precompiler could run alone cannot build the cache ahead
    try: return b'DM_VK_PRECOMPILE_ONLY' in Path(g.manifest['host']).read_bytes()
    except (OSError, TypeError, KeyError): return False


def launch_env(g, s):
    """The environment, command and working folder that start game g with settings s"""
    m = g.manifest
    env = dict(os.environ); env.update(m['env'])
    save = SAVES / g.tid; save.mkdir(parents=True, exist_ok=True)
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
    return env, [m['host'], m['modules']], m['cwd']


def install_desktop():
    apps = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'applications'; apps.mkdir(parents=True, exist_ok=True)
    (apps / 'org.nxasahi.Library.desktop').write_text(
        '[Desktop Entry]\nType=Application\nName=nxasahi\nComment=Switch game library\nIcon=input-gaming\nCategories=Game;\n'
        f'Exec={launcher()} %F\nMimeType=application/x-nx-nsp;application/x-nx-xci;\nTerminal=false\n')
