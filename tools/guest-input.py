#!/usr/bin/env python3
"""Send non-overlapping QMP key presses for reliable OS 9 guest diagnostics.

QMP send-key defaults to a 100ms hold, which the guest can drop (Escape in
particular), while the original fast text helper sends again after 40ms.
QMP.type_text in utm_qmp.py holds each key long enough and keeps a complete
release interval between keys. Control clicks use a small move and 250ms hold, as required by the guest.
Uses the vendored UTM socket helper (tools/utm_qmp.py); it does not change
VM configuration.
"""
import argparse
import time
from utm_qmp import QMP

parser = argparse.ArgumentParser()
parser.add_argument('text', nargs='?', default='')
parser.add_argument('--click', action='store_true', help='small move, then 250ms hold at current pointer')
args = parser.parse_args()
q = QMP()
if args.click:
    q.move(1, 0); time.sleep(.1); q.move(-1, 0); time.sleep(.15)
    q.button(True); time.sleep(.25); q.button(False); time.sleep(.3)
q.type_text(args.text)
