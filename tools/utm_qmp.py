#!/usr/bin/env python3
"""Drive the UTM "Mac OS 9.2.2" VM through its extra QMP socket.

A thin consumer of the harness's public command line (harness/guest.py --utm);
the QMP layer, input timing and pointer tracking live under harness/ (ADR-0009).
The VM's config.plist carries an additional QEMU argument:

    -qmp unix:<UTM container>/utm-mac9-qmp.sock,server=on,wait=off

Usage (every command is passed through unchanged; python3 harness/guest.py
--help lists them with their arguments):
    utm_qmp.py screendump <out.png>
    utm_qmp.py type "text" | key <qcode>...
    utm_qmp.py pgoto|pclick|pdclick|phold <x> <y> | nudge <dx> <dy> | release
    utm_qmp.py prepare|reanchor|locate | goto|click|dclick <x> <y>
    utm_qmp.py moveto <x> <y> | clicknow | dclicknow

The QMP socket takes one client at a time; do not run two of these at once.
"""
import os
import sys

HARNESS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness", "guest.py")
os.execv(sys.executable, [sys.executable, HARNESS, "--utm", *sys.argv[1:]])
