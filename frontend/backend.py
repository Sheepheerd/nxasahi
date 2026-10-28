# The library's screens (frontend/qml) talk to this object: the games as a list model, the builds as another, the settings, a game's options and actions, and the
# controller settings. Controllers also drive the screens: their buttons become key presses on the window (D-pad and left stick: arrows, A: Return, B: Escape, X / Y:
# the X / Y keys, + / −: the Plus / Minus keys), as a Switch's home menu reads them. The game's own window reads its controllers itself, so navigation stops while one runs.
import os, re, shutil, signal, time
from pathlib import Path

from PySide6.QtCore import (QAbstractListModel, QByteArray, QCoreApplication, QEvent, QModelIndex, QObject, QProcess, QProcessEnvironment, Qt, QTimer, QUrl,
                            Property, Signal, Slot)
from PySide6.QtGui import QDesktopServices, QGuiApplication, QKeyEvent

import controllers
import library as L


def _local(u):   # a QML url (a QUrl or file:///… text) or a path → a path string
    if isinstance(u, QUrl): return u.toLocalFile() if u.isLocalFile() else u.toString()
    u = str(u)
    return QUrl(u).toLocalFile() if u.startswith('file:') else u


class GameModel(QAbstractListModel):
    ROLES = ('tid', 'name', 'icon', 'subtitle', 'status', 'progress', 'stage', 'version', 'error', 'ready', 'running', 'building')

    def __init__(self, back):
        super().__init__(); self.back, self.order = back, []

    def roleNames(self): return {Qt.UserRole + i: QByteArray(r.encode()) for i, r in enumerate(self.ROLES)}
    def rowCount(self, parent=QModelIndex()): return 0 if parent.isValid() else len(self.order)

    def data(self, idx, role):
        if not idx.isValid() or idx.row() >= len(self.order): return None
        tid = self.order[idx.row()]; g = self.back.games[tid]; b = self.back.builds.get(tid)
        r = self.ROLES[role - Qt.UserRole] if Qt.UserRole <= role < Qt.UserRole + len(self.ROLES) else None
        building = bool(b and b.ok is None)
        running = tid in self.back.running
        if r == 'tid': return tid
        if r == 'name': return g.name
        if r == 'icon': return QUrl.fromLocalFile(g.icon).toString() if g.icon else ''
        if r == 'version': return g.version
        if r == 'error': return g.conf.get('error', '')
        if r == 'ready': return g.ready
        if r == 'running': return running
        if r == 'building': return building
        if r == 'status': return 'playing' if running else 'building' if building else 'ready' if g.ready else 'failed' if g.conf.get('error') else 'new'
        if r == 'progress': return -1.0 if not building or b.frac is None else float(b.frac)
        if r == 'stage': return (b.stage if b.stage != 'waiting' else 'waiting to build') + (f' — {b.step}' if b.step else '') if building else ''
        if r == 'subtitle':
            extra = (f' · {g.updates} update{"s" if g.updates != 1 else ""}' if g.updates else '') + (f' · {g.dlc} DLC' if g.dlc else '')
            if running: return 'Playing' + extra
            if building: return (b.stage if b.stage != 'waiting' else 'Waiting to build') + extra
            if g.ready: return (f'Version {g.version}' if g.version else 'Ready') + extra
            return ('Build failed' if g.conf.get('error') else 'Not built yet') + extra
        return None

    def sort_key(self, tid):
        g = self.back.games[tid]; return (-g.conf.get('last_played', 0), g.name.lower())

    def reset(self):
        self.beginResetModel(); self.order = sorted(self.back.games, key=self.sort_key); self.endResetModel()

    def insert(self, tid):
        keys = [self.sort_key(t) for t in self.order]; k = self.sort_key(tid)
        i = next((j for j, kk in enumerate(keys) if k < kk), len(self.order))
        self.beginInsertRows(QModelIndex(), i, i); self.order.insert(i, tid); self.endInsertRows()

    def remove(self, tid):
        if tid not in self.order: return
        i = self.order.index(tid); self.beginRemoveRows(QModelIndex(), i, i); self.order.pop(i); self.endRemoveRows()

    def touch(self, tid):
        if tid in self.order:
            i = self.index(self.order.index(tid)); self.dataChanged.emit(i, i)

    def resort(self):   # the order changes after a game is played: moved one row at a time, so the view keeps its items
        want = sorted(self.order, key=self.sort_key)
        for dst, tid in enumerate(want):
            src = self.order.index(tid)
            if src != dst:
                self.beginMoveRows(QModelIndex(), src, src, QModelIndex(), dst); self.order.insert(dst, self.order.pop(src)); self.endMoveRows()


class BuildModel(QAbstractListModel):
    ROLES = ('tid', 'title', 'detail', 'progress', 'state', 'log')

    def __init__(self, back):
        super().__init__(); self.back, self.order = back, []

    def roleNames(self): return {Qt.UserRole + i: QByteArray(r.encode()) for i, r in enumerate(self.ROLES)}
    def rowCount(self, parent=QModelIndex()): return 0 if parent.isValid() else len(self.order)

    def data(self, idx, role):
        if not idx.isValid() or idx.row() >= len(self.order): return None
        b = self.order[idx.row()]; r = self.ROLES[role - Qt.UserRole]
        if r == 'tid': return b.game.tid
        if r == 'title': return f'{b.what} {b.game.name}' if b.ok is None else (f'{b.game.name}: {b.done_title}' if b.ok else f'{b.game.name}: build failed')
        if r == 'detail': return b.stage + (f' — {b.step}' if b.step else '') if b.ok is None else (b.error if not b.ok else b.done_text)
        if r == 'progress': return 1.0 if b.ok else -1.0 if b.frac is None else float(b.frac)
        if r == 'state': return 'running' if b.ok is None else 'done' if b.ok else 'failed'
        if r == 'log': return '\n'.join(b.lines[-200:])
        return None

    def add(self, b):
        old = next((x for x in self.order if x.game.tid == b.game.tid), None)
        if old: self.drop(old)
        self.beginInsertRows(QModelIndex(), len(self.order), len(self.order)); self.order.append(b); self.endInsertRows()

    def drop(self, b):
        if b in self.order:
            i = self.order.index(b); self.beginRemoveRows(QModelIndex(), i, i); self.order.pop(i); self.endRemoveRows()

    def touch(self, b):
        if b in self.order:
            i = self.index(self.order.index(b)); self.dataChanged.emit(i, i)


# Controller buttons → the keys the screens read (SDL_GamepadButton names; positions as on a Switch: A is the right face button)
NAV_BUTTONS = {'east': Qt.Key_Return, 'south': Qt.Key_Escape, 'north': Qt.Key_X, 'west': Qt.Key_Y, 'start': Qt.Key_Plus, 'back': Qt.Key_Minus,
               'leftshoulder': Qt.Key_PageUp, 'rightshoulder': Qt.Key_PageDown}
NAV_DIRS = {'dpup': Qt.Key_Up, 'dpdown': Qt.Key_Down, 'dpleft': Qt.Key_Left, 'dpright': Qt.Key_Right,
            '-lefty': Qt.Key_Up, '+lefty': Qt.Key_Down, '-leftx': Qt.Key_Left, '+leftx': Qt.Key_Right}


class Backend(QObject):
    toast = Signal(str, int, arguments=['text', 'seconds'])
    gamesChanged, settingsChanged, padsChanged, playersChanged, liveChanged, reordered = Signal(), Signal(), Signal(), Signal(), Signal(), Signal()
    padCaptured = Signal(str, arguments=['source'])

    def __init__(self):
        super().__init__()
        self.settings = L.load_json(L.CONFIG, L.DEFAULTS)
        self.games, self.builds, self.queue, self.running = {}, {}, [], {}
        self._games, self._builds = GameModel(self), BuildModel(self)
        self.window = None
        self.players = controllers.load()
        self.pads, self.connected, self.capture, self.baseline, self.live = controllers.Pads(), [], None, set(), set()
        self.held, self.next_repeat = {}, {}
        self.scan()
        self.timer = QTimer(self); self.timer.timeout.connect(self.tick); self.timer.start(16)
        self.ptimer = QTimer(self); self.ptimer.timeout.connect(self.pulse); self.ptimer.start(250)

    # ---- models and settings ----
    @Property(QObject, constant=True)
    def gameModel(self): return self._games
    @Property(QObject, constant=True)
    def buildModel(self): return self._builds
    @Property(int, notify=gamesChanged)
    def gameCount(self): return len(self.games)
    @Property('QVariantMap', notify=settingsChanged)
    def config(self): return dict(self.settings, keys_found=Path(os.path.expanduser(self.settings['keys'])).is_file(), library=str(L.DATA))
    @Property('QVariantList', constant=True)
    def languages(self): return [{'code': '', 'name': 'English (the default)'}] + [{'code': c, 'name': f'{n} ({c})'} for c, n in L.LANGS]

    @Slot(str, 'QVariant')
    def setSetting(self, k, v):
        if k == 'keys': v = _local(v)
        if k == 'precompile': v = int(v)
        self.settings[k] = v; L.save_json(L.CONFIG, self.settings); self.settingsChanged.emit()

    @Slot(str, result=int)
    def indexOf(self, tid): return self._games.order.index(tid) if tid in self._games.order else -1

    def scan(self):
        L.GAMES.mkdir(parents=True, exist_ok=True)
        for d in sorted(L.GAMES.iterdir()):
            if d.is_dir() and re.fullmatch(r'[0-9A-F]{16}', d.name) and d.name not in self.games and any(p.suffix.lower() in L.GAME_EXT for p in d.iterdir()):
                self.games[d.name] = L.Game(d)
        self._games.reset(); self.gamesChanged.emit()

    def refresh(self, tid=None):
        for t in ([tid] if tid else list(self.games)): self._games.touch(t)

    # ---- a game's information and options ----
    @Slot(str, result='QVariantMap')
    def gameInfo(self, tid):
        g = self.games.get(tid)
        if not g: return {}
        m = g.manifest or {}
        return {'tid': tid, 'name': g.name, 'version': g.version or '—', 'build': m.get('package', '') or '—', 'profile': m.get('profile', '') or '—',
                'updates': g.updates, 'dlc': g.dlc, 'built': m.get('built', '').replace('T', ' ') or 'not built', 'error': g.conf.get('error', ''),
                'language': g.conf.get('language', ''), 'extra_env': g.conf.get('extra_env', ''), 'ready': g.ready, 'running': tid in self.running,
                'building': bool(self.builds.get(tid) and self.builds[tid].ok is None), 'saves': str(L.SAVES / tid), 'logs': str(g.work() / 'logs'),
                'icon': QUrl.fromLocalFile(g.icon).toString() if g.icon else ''}

    @Slot(str, str, str)
    def setGameOption(self, tid, k, v):
        g = self.games.get(tid)
        if g: g.conf[k] = v; g.save_conf()

    # ---- adding games ----
    @Slot('QVariantList')
    def addFiles(self, urls): self.add_paths([_local(u) for u in urls])

    @Slot(str, 'QVariantList')
    def addContent(self, tid, urls):   # an update or DLC chosen for one game: files of another title are refused
        g = self.games[tid]; keep = []
        for p in (_local(u) for u in urls):
            try: app, kind, _ = L.identify(p)
            except ValueError as e: self.toast.emit(str(e), 6); continue
            if f'{app:016X}' != tid: self.toast.emit(f'{Path(p).name}: belongs to {app:016X}, not {g.name}', 6)
            elif kind == 'base': self.toast.emit(f'{Path(p).name}: the base game, not an update or DLC', 6)
            else: keep.append(p)
        if keep: self.add_paths(keep)

    def add_paths(self, paths):
        touched, bad = {}, []
        paths = list(paths)
        for p in paths:
            p = Path(p)
            if p.is_dir():   # a folder: the game files in it
                paths += [str(q) for q in sorted(p.iterdir()) if q.suffix.lower() in L.GAME_EXT]; continue
            if p.suffix.lower() not in L.GAME_EXT: bad.append(f'{p.name}: not a game file'); continue
            try: app, kind, ver = L.identify(p)
            except ValueError as e: bad.append(str(e)); continue
            tid = f'{app:016X}'; folder = L.GAMES / tid
            if kind != 'base' and not (folder.is_dir() and any(L.identify_kind(q) == 'base' for q in folder.iterdir() if q.suffix.lower() in L.GAME_EXT)):
                bad.append(f'{p.name}: an {kind} of {tid}; add the base game first'); continue
            folder.mkdir(parents=True, exist_ok=True)
            link = folder / p.name
            if kind == 'update':   # one update at a time: the newest is the one built
                for q in folder.iterdir():
                    if q.suffix.lower() in L.GAME_EXT and q != link and L.identify_kind(q) == 'update': q.unlink()
            if not link.exists():
                try: os.link(p, link)   # a hard link when on the same file system (the game keeps working if the original moves), else a symbolic link
                except OSError: link.symlink_to(p.resolve())
            touched[tid] = kind
        for tid, kind in touched.items():
            if tid not in self.games: self.games[tid] = L.Game(L.GAMES / tid); self._games.insert(tid); self.gamesChanged.emit()
            else: self.games[tid].reload(); self.refresh(tid)
            self.toast.emit(f'{self.games[tid].name}: ' + ('added, building…' if kind == 'base' else f'{kind} added, rebuilding…'), 4)
            self.enqueue(self.games[tid])
        for b in bad: self.toast.emit(b, 6)

    # ---- builds ----
    def enqueue(self, g, kind=L.Build):
        if g.tid in self.builds and self.builds[g.tid].ok is None: self.toast.emit(f'{g.name} is already building', 4); return
        b = kind(self.settings, g); self.builds[g.tid] = b
        b.changed.connect(lambda b=b: self._build_changed(b)); b.finished.connect(lambda ok, msg, b=b: self._build_done(b))
        self._builds.add(b); self.queue.append(b); self.pump(); self.refresh(g.tid)

    def pump(self):
        if any(b.proc and b.ok is None for b in self.builds.values()): return
        while self.queue:
            b = self.queue.pop(0)
            if b.ok is None: b.start(); return

    def _build_changed(self, b): self._builds.touch(b); self._games.touch(b.game.tid)

    def _build_done(self, b):
        b.game.reload(); self._build_changed(b); self.refresh(b.game.tid)
        self.toast.emit(f'{b.game.name}: {b.done_title}' if b.ok else f'{b.game.name}: build failed — {b.error}', 5 if b.ok else 8)
        if b.ok: QTimer.singleShot(8000, lambda: self._builds.drop(b) if b.ok else None)
        self.pump()

    def pulse(self):   # an indeterminate build's bar moves in the screen; nothing to do here but keep its card fresh
        for b in self.builds.values():
            if b.ok is None: self._builds.touch(b)

    @Slot(str)
    def rebuild(self, tid):
        if tid in self.games: self.enqueue(self.games[tid])

    @Slot(str)
    def buildCache(self, tid):
        g = self.games[tid]
        if not g.ready: self.toast.emit(f'{g.name} is not built yet', 4); return
        if not L.shader_cache_supported(g): self.toast.emit(f'{g.name} was built before shader caches could be built ahead: Rebuild it (that builds the cache too)', 8); return
        self.enqueue(g, L.CacheBuild)

    @Slot(str)
    def cancelBuild(self, tid):
        b = self.builds.get(tid)
        if b: b.cancel()

    @Slot(str)
    def dismissBuild(self, tid):
        b = self.builds.get(tid)
        if b and b.ok is not None: self._builds.drop(b)

    # ---- playing ----
    @Slot(str)
    def play(self, tid):
        g = self.games[tid]
        if tid in self.running: self.toast.emit(f'{g.name} is already running', 4); return
        if not g.ready:
            b = self.builds.get(tid)
            self.toast.emit(f'{g.name} is still building' if b and b.ok is None else f'{g.name} is not built yet: building it', 4)
            if not (b and b.ok is None): self.enqueue(g)
            return
        env, cmd, cwd = L.launch_env(g, self.settings)
        logp = Path(cwd) / 'last_run.log'; Path(cwd).mkdir(parents=True, exist_ok=True)
        proc = QProcess(self); pe = QProcessEnvironment()
        for k, v in env.items(): pe.insert(k, v)
        proc.setProcessEnvironment(pe); proc.setWorkingDirectory(cwd)
        proc.setProcessChannelMode(QProcess.MergedChannels); proc.setStandardOutputFile(str(logp))
        proc.finished.connect(lambda code, st, tid=tid: self._exited(tid, code, st, logp))
        proc.start(cmd[0], cmd[1:])
        if not proc.waitForStarted(5000): self.toast.emit(f'{g.name} did not start: {proc.errorString()}', 8); return
        self.running[tid] = (proc, time.time()); g.conf['last_played'] = int(time.time()); g.save_conf()
        self.toast.emit(f'Starting {g.name}…', 3); self.refresh(tid); self.gamesChanged.emit()
        self._games.resort(); self.reordered.emit()

    def _exited(self, tid, code, status, logp):
        _, t0 = self.running.pop(tid, (None, 0)); g = self.games.get(tid)
        crashed = status != QProcess.NormalExit
        if g and (crashed or code) and time.time() - t0 < 30:
            tail = ''
            try: tail = next((l for l in reversed(logp.read_text(errors='replace').splitlines()) if '[RED]' in l or 'die' in l.lower() or 'error' in l.lower()), '')
            except OSError: pass
            self.toast.emit(f'{g.name} stopped ({"crashed" if crashed else f"code {code}"}). {tail[:140]}  Log: {logp}', 10)
        self.refresh(tid); self.gamesChanged.emit()

    @Slot(str)
    def stop(self, tid):
        r = self.running.get(tid)
        if r: r[0].terminate()

    @Property(bool, notify=gamesChanged)
    def anyRunning(self): return bool(self.running)

    # ---- removing ----
    @Slot(str)
    def remove(self, tid):
        g = self.games.get(tid)
        if not g: return
        b = self.builds.get(tid)
        if b and b.ok is None: self.toast.emit('Stop its build first', 4); return
        if tid in self.running: self.toast.emit('Quit the game first', 4); return
        shutil.rmtree(g.folder, ignore_errors=True)
        self._games.remove(tid); del self.games[tid]
        if b: self._builds.drop(b); self.builds.pop(tid, None)
        self.toast.emit(f'Removed {g.name}', 4); self.gamesChanged.emit()

    # ---- folders and the desktop ----
    def _open(self, p):
        p = Path(p); p.mkdir(parents=True, exist_ok=True); QDesktopServices.openUrl(QUrl.fromLocalFile(str(p)))
    @Slot(str)
    def openSaves(self, tid): self._open(L.SAVES / tid)
    @Slot(str)
    def openLogs(self, tid): self._open(self.games[tid].work() / 'logs')
    @Slot(str)
    def openFolder(self, tid): self._open(self.games[tid].folder)
    @Slot()
    def openLibrary(self): self._open(L.GAMES)
    @Slot()
    def installDesktop(self): L.install_desktop(); self.toast.emit('Added nxasahi to the app launcher', 4)

    # ---- controllers: settings ----
    @Property(int, constant=True)
    def playerCount(self): return controllers.PLAYERS
    @Property('QVariantList', constant=True)
    def bindGroups(self):   # the binding columns, laid out as the controller is
        return [[{'title': name, 'targets': [{'id': t, 'label': controllers.LABEL[t]} for t in targets]} for name, targets in col] for col in controllers.GROUPS]

    @Slot(int, result='QVariantMap')
    def player(self, p):
        pl = self.players[p]
        return {'keyboard': bool(pl['keyboard']), 'controller': pl.get('controller') or 'none', 'deadzone': float(pl.get('deadzone', 0)),
                'keys': {t: pl['keys'].get(t, '') for t in controllers.TARGETS}, 'buttons': {t: controllers.pad_label(pl['buttons'].get(t, '')) for t in controllers.TARGETS},
                'raw': {t: pl['buttons'].get(t, '') for t in controllers.TARGETS},
                'others': [i + 1 for i, q in enumerate(self.players) if q['keyboard'] and i != p]}

    @Slot(int, result='QVariantList')
    def controllerChoices(self, p):
        pl = self.players[p]; cur = pl.get('controller') or 'none'
        out = [{'id': 'none', 'name': 'None'}, {'id': 'any', 'name': 'Any controller'}] + [{'id': g, 'name': n} for g, n, _ in self.connected]
        if cur not in [o['id'] for o in out]: out.append({'id': cur, 'name': f'{pl.get("controller_name") or "Controller"} (not connected)'})
        return out

    def _save_players(self):
        try: controllers.save(self.players)
        except OSError as e: self.toast.emit(f'Could not save the controllers: {e}', 6)
        self.playersChanged.emit()

    @Slot(int, str, 'QVariant')
    def setPlayer(self, p, k, v):
        pl = self.players[p]
        if k == 'keyboard':
            pl['keyboard'] = bool(v)
            if pl['keyboard'] and not pl['keys']: pl['keys'] = dict(controllers.DEFAULT_KEYS)
        elif k == 'controller':
            pl['controller'] = str(v)
            pl['controller_name'] = next((n for g, n, _ in self.connected if g == v), pl.get('controller_name', '')) if v not in ('none', 'any') else ''
        elif k == 'deadzone': pl['deadzone'] = round(float(v), 2)
        self._save_players()

    @Slot(int, str, str, str)
    def setBind(self, p, kind, target, value):
        d = self.players[p][kind]
        if value: d[target] = value
        else: d.pop(target, None)
        self._save_players()

    @Slot(int)
    def resetPlayer(self, p): self.players[p] = controllers.default_player(p); self._save_players()

    @Slot(int, result=str)
    def keyName(self, native_scan_code): return controllers.EVDEV.get(native_scan_code - 8, '')   # Wayland / X11: evdev code + 8

    @Slot(int)
    def startPadCapture(self, p): self.capture = p; self.baseline = self._pressed(p)
    @Slot()
    def stopPadCapture(self): self.capture = None

    @Property('QVariantList', notify=liveChanged)
    def liveSources(self): return sorted(self.live)   # what is pressed on the shown player's controller, for the bindings screen
    @Property(int, notify=padsChanged)
    def padCount(self): return len(self.connected)

    def _pressed(self, p=None, thresh=16000):
        s = set()
        if not self.pads.lib: return s
        pads = [g for _, _, g in self.connected]
        if p is not None:
            c = self.players[p].get('controller', 'none')
            pads = [g for guid, _, g in self.connected if c == 'any' or c == guid] or pads
        for g in pads: s |= controllers.pressed_sources(*self.pads.state(g), thresh)
        return s

    # ---- controllers: polling, capture, navigation ----
    def tick(self):
        try: conn = self.pads.poll()
        except Exception: conn = []
        if [c[0] for c in conn] != [c[0] for c in self.connected]: self.connected = conn; self.padsChanged.emit()
        else: self.connected = conn
        if not self.connected: self.held.clear(); return
        if self.capture is not None:
            live = self._pressed(self.capture)
            new = self._pressed(self.capture, 24000) - self.baseline
            if new:   # a stick: the axis direction; else the first button pressed
                src = sorted(new, key=lambda s: s[0] not in '+-')[0]; self.capture = None; self.padCaptured.emit(src)
            self.baseline &= live
            if live != self.live: self.live = live; self.liveChanged.emit()
            return
        live = self._pressed()
        if live != self.live: self.live = live; self.liveChanged.emit()
        if self.running or not self.window or not self.window.isActive(): self.held.clear(); return
        now = time.monotonic()
        for src, key in list(NAV_BUTTONS.items()) + list(NAV_DIRS.items()):
            down = src in live
            if down and src not in self.held:
                self.held[src] = now; self.next_repeat[src] = now + 0.4; self._key(key)
            elif down and src in NAV_DIRS and now >= self.next_repeat.get(src, now + 1):
                self.next_repeat[src] = now + 0.11; self._key(key)
            elif not down: self.held.pop(src, None)

    def _key(self, key):
        for t in (QEvent.KeyPress, QEvent.KeyRelease):
            QGuiApplication.postEvent(self.window, QKeyEvent(t, key, Qt.NoModifier))

    def shutdown(self):
        self.timer.stop(); self.pads.close()
