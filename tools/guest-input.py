#!/usr/bin/env python3
"""Send non-overlapping QMP key presses to the UTM guest for diagnostics.

A thin consumer of the harness's public command line (harness/guest.py --utm
input): keys are held long enough for the guest and released fully between
presses, and --click makes a small move and a 250ms hold at the current
pointer first, as the guest requires. It does not change VM configuration.
"""
import os
import sys

HARNESS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness", "guest.py")
os.execv(sys.executable, [sys.executable, HARNESS, "--utm", "input", *sys.argv[1:]])
