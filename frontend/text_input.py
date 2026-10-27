#!/usr/bin/env python3
# The software keyboard's input box for the Linux host (dm/host/linux/front_sdl.c front_text_input, DM_TEXT_INPUT): text_input.py <title> <subtitle> <hint> <initial text> <max characters>
# Prints the text (UTF-8, no newline) and exits 0 when confirmed; exits 1 when cancelled. GTK 4 + libadwaita, as the game library
import sys

import gi
gi.require_version('Gtk', '4.0'); gi.require_version('Adw', '1')
from gi.repository import Adw, Gtk

title, sub, hint, initial, maxc = (sys.argv[1:] + [''] * 5)[:5]
maxc = int(maxc) if maxc.isdigit() else 0
result = {'text': None}


def activate(app):
    win = Adw.ApplicationWindow(application=app, title=title or 'Enter Text', default_width=520, modal=True, resizable=False)
    tv = Adw.ToolbarView(); hb = Adw.HeaderBar(show_end_title_buttons=False, show_start_title_buttons=False)
    cancel = Gtk.Button(label='Cancel'); ok = Gtk.Button(label='OK', css_classes=['suggested-action'])
    hb.pack_start(cancel); hb.pack_end(ok); hb.set_title_widget(Adw.WindowTitle(title=title or 'Enter Text', subtitle=sub)); tv.add_top_bar(hb)
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin_top=18, margin_bottom=18, margin_start=18, margin_end=18)
    entry = Gtk.Entry(text=initial, placeholder_text=hint, activates_default=True, hexpand=True)
    if maxc: entry.set_max_length(maxc)
    box.append(entry)
    if maxc: box.append(Gtk.Label(label=f'Up to {maxc} characters', xalign=0, css_classes=['dim-label', 'caption']))
    tv.set_content(box); win.set_content(tv); win.set_default_widget(ok)
    def done(text):
        result['text'] = text; win.close()
    ok.connect('clicked', lambda *a: done(entry.get_text()))
    entry.connect('activate', lambda *a: done(entry.get_text()))
    cancel.connect('clicked', lambda *a: done(None))
    keys = Gtk.EventControllerKey(); keys.connect('key-pressed', lambda c, kv, kc, st: (done(None), True)[1] if kv == 0xff1b else False); win.add_controller(keys)   # Escape cancels
    win.present(); entry.grab_focus(); entry.select_region(0, -1)


app = Adw.Application(application_id='org.nxasahi.TextInput')
app.connect('activate', activate)
app.run([sys.argv[0]])
if result['text'] is None: sys.exit(1)
sys.stdout.write(result['text']); sys.stdout.flush()
