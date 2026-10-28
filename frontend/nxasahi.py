#!/usr/bin/env python3
# nxasahi: the game library, a console-style home screen (Qt 6 / QML: frontend/qml) over frontend/library.py (games, imports, builds, launching) through
# frontend/backend.py. Drop NSP / XCI / NSZ files on the window or press +: a base game is imported (scripts/build_ipa_from_nsp.py --platform linux, its progress on
# the game's tile and in a card); an update or DLC joins its game and the game is built again with it. A controller drives it as the keyboard does.
# Run it through frontend/nxasahi.sh (inside nix develop) or the installed nxasahi command. Files given on the command line are added.
# NXASAHI_SHOT=<png> (testing): render the home screen once, write it to that file and quit; NXASAHI_SHOT_PAGE=<page>[:<title ID>] opens a page first
import os, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from PySide6.QtCore import QTimer, QUrl   # noqa: E402
from PySide6.QtGui import QGuiApplication, QIcon   # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine   # noqa: E402
from PySide6.QtQuickControls2 import QQuickStyle   # noqa: E402

from backend import Backend   # noqa: E402


def main():
    QQuickStyle.setStyle('Basic')   # every control is drawn by the library's own QML; the Basic style is the one meant to be restyled
    app = QGuiApplication(sys.argv)
    app.setApplicationName('nxasahi'); app.setOrganizationName('nxasahi'); app.setDesktopFileName('org.nxasahi.Library')
    app.setWindowIcon(QIcon.fromTheme('input-gaming'))
    back = Backend()
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty('backend', back)
    engine.load(QUrl.fromLocalFile(str(HERE / 'qml/Main.qml')))
    if not engine.rootObjects(): return 1
    win = engine.rootObjects()[0]; back.window = win
    files = [a for a in sys.argv[1:] if not a.startswith('-') and Path(a).exists()]
    if files: QTimer.singleShot(0, lambda: back.add_paths(files))
    shot, page = os.environ.get('NXASAHI_SHOT'), os.environ.get('NXASAHI_SHOT_PAGE')
    if page: pg, _, tid = page.partition(':'); win.setProperty('pageTid', tid); win.setProperty('page', pg)
    if shot: QTimer.singleShot(int(os.environ.get('NXASAHI_SHOT_MS', '1500')), lambda: (win.grabWindow().save(shot), app.quit()))
    app.aboutToQuit.connect(back.shutdown)
    return app.exec()


if __name__ == '__main__':
    sys.exit(main())
