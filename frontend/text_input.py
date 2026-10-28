#!/usr/bin/env python3
# The software keyboard's input box for the Linux host (dm/host/linux/front_sdl.c front_text_input, DM_TEXT_INPUT): text_input.py <title> <subtitle> <hint> <initial text> <max characters>
# Prints the text (UTF-8, no newline) and exits 0 when confirmed; exits 1 when cancelled. Qt Widgets, in the library's colours
import sys

from PySide6.QtWidgets import QApplication, QDialog, QDialogButtonBox, QLabel, QLineEdit, QVBoxLayout

title, sub, hint, initial, maxc = (sys.argv[1:] + [''] * 5)[:5]
maxc = int(maxc) if maxc.isdigit() else 0

app = QApplication([sys.argv[0]])
app.setStyleSheet('QDialog { background: #171b27; } QLabel { color: #eef1f8; font-size: 15px; } QLabel#dim { color: #8b93a9; font-size: 13px; }'
                  'QLineEdit { background: #10141d; color: #eef1f8; border: 2px solid #4fa3ff; border-radius: 8px; padding: 8px; font-size: 16px; }'
                  'QPushButton { background: #262c3e; color: #eef1f8; border: 1px solid #323a52; border-radius: 8px; padding: 8px 18px; font-size: 14px; }'
                  'QPushButton:default { background: #4fa3ff; color: #0b0e14; font-weight: bold; }')
d = QDialog(); d.setWindowTitle(title or 'Enter Text'); d.setMinimumWidth(520)
lay = QVBoxLayout(d); lay.setContentsMargins(22, 20, 22, 18); lay.setSpacing(10)
if sub: lay.addWidget(QLabel(sub))
entry = QLineEdit(initial); entry.setPlaceholderText(hint)
if maxc: entry.setMaxLength(maxc)
lay.addWidget(entry)
if maxc: n = QLabel(f'Up to {maxc} characters'); n.setObjectName('dim'); lay.addWidget(n)
bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel); bb.accepted.connect(d.accept); bb.rejected.connect(d.reject); lay.addWidget(bb)
entry.selectAll(); entry.setFocus()
ok = d.exec() == QDialog.Accepted
if not ok: sys.exit(1)
sys.stdout.write(entry.text()); sys.stdout.flush()
