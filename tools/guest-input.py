#!/usr/bin/env python3
"""Send non-overlapping QMP key presses for reliable OS 9 guest diagnostics.

QMP send-key defaults to a 100ms hold, while the original fast text helper
sends again after 40ms. Keep a complete hold/release interval between keys.
Control clicks use a small move and 250ms hold, as required by the guest.
Uses the existing UTM socket helper; it does not change VM configuration.
"""
import argparse
import sys
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'macos922/tools'))
from utm_qmp import QMP

parser = argparse.ArgumentParser()
parser.add_argument('text', nargs='?', default='')
parser.add_argument('--click', action='store_true', help='small move, then 250ms hold at current pointer')
args = parser.parse_args()
q = QMP()
if args.click:
    q.move(1, 0); time.sleep(.1); q.move(-1, 0); time.sleep(.15)
    q.button(True); time.sleep(.25); q.button(False); time.sleep(.3)
keymap = {' ': 'spc', '.': 'dot', '-': 'minus', '/': 'slash', '\n': 'ret'}
for ch in args.text:
    if ch.isascii() and ch.isalpha():
        keys = ['shift', ch.lower()] if ch.isupper() else [ch]
    elif ch.isdigit() and ch.isascii():
        keys = [ch]
    elif ch in keymap:
        keys = [keymap[ch]]
    else:
        raise SystemExit('Unsupported diagnostic character')
    q.cmd('send-key', keys=[{'type': 'qcode', 'data': key} for key in keys], **{'hold-time': 200})
    time.sleep(.3)
