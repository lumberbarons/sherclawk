#!/usr/bin/env python3
"""Drive an OS 9 guest over QMP: screenshots, keys, text and the pointer.

The public command line for the harness's QMP layer. Pick the guest with
--slot (a provisioned standalone slot), --socket (any QMP socket; default
$QMP_SOCK) or --utm (the UTM "Mac OS 9.2.2" VM). Stdlib only.

    guest.py --slot slot1 screendump shot.png
    guest.py --slot slot1 type "Sherclawk"
    guest.py --slot slot1 key meta_l o
    guest.py --slot slot1 pclick 320 240

Commands (the QMP socket takes one client at a time: do not run two at once):
    screendump <out>             PNG or PPM by extension
    hmp '<monitor command>'
    key <qcode>...               keys pressed together, held long enough
    type "text"                  refused whole if any character has no key
    input [--click] [text]       optional click where the pointer rests, then text
    mice                         list pointing devices
    reset                        system_reset, forgetting the pointer position

  Closed-loop pointer (re-parks the pointer, so it ends within a few pixels):
    pgoto|pclick|pdclick|phold <x> <y>
    tap                          click where the pointer already is
    nudge <dx> <dy>              raw relative move (menus, popups)
    release                      let the button up

  Template-tracked pointer:
    prepare | reanchor | locate
    goto|click|dclick <x> <y>

  Blind, from the remembered position (menus):
    moveto <x> <y> | clicknow | dclicknow
"""
import argparse
import hashlib
import os
import sys
import time

import pointer
import provision
import qmp
import screen

UTM_GROUP = os.path.expanduser("~/Library/Group Containers/WDNLXAD4W8.com.utmapp.UTM")
UTM_SOCK = os.path.join(UTM_GROUP, "utm-mac9-qmp.sock")

# Pointer commands need a relative device; typing and screenshots do not.
POINTER_COMMANDS = {"pgoto", "pclick", "pdclick", "phold", "tap", "nudge", "release", "input",
                    "prepare", "reanchor", "locate", "goto", "click", "dclick",
                    "moveto", "clicknow", "dclicknow"}


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.split("\n")[1:]))
    which = ap.add_mutually_exclusive_group()
    which.add_argument("--slot", help="a provisioned standalone slot (see provision.py)")
    which.add_argument("--socket", help="QMP unix socket (default $QMP_SOCK)")
    which.add_argument("--utm", action="store_true", help="the UTM Mac OS 9.2.2 VM")
    ap.add_argument("--home", help="harness state directory (default $OS9_HARNESS_HOME)")
    ap.add_argument("--state-dir", help="where pointer state and captures live")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("mice")
    sub.add_parser("reset")
    for name in ("tap", "release", "prepare", "reanchor", "locate", "clicknow", "dclicknow"):
        sub.add_parser(name)
    sub.add_parser("screendump").add_argument("out")
    sub.add_parser("hmp").add_argument("line")
    sub.add_parser("key").add_argument("qcodes", nargs="+")
    sub.add_parser("type").add_argument("text")
    p = sub.add_parser("input")
    p.add_argument("text", nargs="?", default="")
    p.add_argument("--click", action="store_true", help="click where the pointer rests first")
    for name in ("pgoto", "pclick", "pdclick", "phold", "goto", "click", "dclick", "moveto"):
        p = sub.add_parser(name)
        p.add_argument("x", type=int)
        p.add_argument("y", type=int)
    p = sub.add_parser("nudge")
    p.add_argument("dx", type=int)
    p.add_argument("dy", type=int)
    return ap


def resolve(args):
    """Return (socket path, Capture, state directory) for the chosen guest."""
    state_dir = args.state_dir or pointer.default_state_dir()
    if args.utm:
        path = os.environ.get("UTM_QMP_SOCK", UTM_SOCK)
        # UTM's sandboxed QEMU only writes inside its own group container.
        return path, qmp.Capture(UTM_GROUP, relative=True), state_dir
    if args.slot:
        path = provision.slot(provision.Home(args.home), args.slot).qmp
    else:
        path = args.socket or os.environ.get("QMP_SOCK")
        if not path:
            raise qmp.QMPError("choose a guest: --slot, --socket, --utm or $QMP_SOCK")
    return path, qmp.Capture(os.path.join(state_dir, "shots")), state_dir


def open_guest(args):
    path, capture, state_dir = resolve(args)
    q = qmp.QMP(path, capture=capture)
    key = hashlib.sha1(os.path.abspath(path).encode()).hexdigest()[:10]
    state = pointer.GuestState(os.path.join(state_dir, "guest-%s.json" % key),
                               qmp.socket_incarnation(path))
    return q, state


def prepared_cursor(q, state):
    c = pointer.Cursor(q, state, log=print)
    if c.verify_template():
        print("template OK, cursor at %s" % (c.pos,))
        return c
    print("template missing/stale; calibrating...")
    c.calibrate()
    print("calibrated, cursor at %s" % (c.pos,))
    return c


def run(args, q, state):
    cmd = args.cmd
    if cmd in POINTER_COMMANDS:
        q.use_relative_mouse()
    if cmd == "screendump":
        q.screendump(args.out)
    elif cmd == "hmp":
        print(q.hmp(args.line))
    elif cmd == "key":
        q.key(*args.qcodes)
    elif cmd == "type":
        q.type_text(args.text)
    elif cmd == "input":
        if args.click:
            pointer.Pointer(q).click()
        q.type_text(args.text)
    elif cmd == "mice":
        for m in q.mice():
            print("%s #%d: %s%s" % ("*" if m.get("current") else " ", m["index"], m["name"],
                                    " (absolute)" if m.get("absolute") else ""))
    elif cmd == "reset":
        q.system_reset()
        state.invalidate()
    elif cmd in ("pgoto", "pclick", "pdclick", "phold"):
        ptr = pointer.Pointer(q)
        print("pointer at", ptr.goto(args.x, args.y))
        if cmd == "pclick":
            ptr.click()
        elif cmd == "pdclick":
            ptr.click()
            ptr.click()
        elif cmd == "phold":
            ptr.press()
    elif cmd == "tap":
        pointer.Pointer(q).click()
    elif cmd == "nudge":
        q.move(args.dx, args.dy)
    elif cmd == "release":
        q.button(False)
    elif cmd == "prepare":
        prepared_cursor(q, state)
    elif cmd == "locate":
        c = pointer.Cursor(q, state)
        if not c.template:
            c.calibrate()
        print(c.recover())
    elif cmd == "reanchor":
        c = pointer.Cursor(q, state, log=print)
        if not c.template:
            c.calibrate()
        c.recover()
        state.set_pos(c.pos)
        print("anchored at", c.pos)
    elif cmd in ("goto", "click", "dclick"):
        c = prepared_cursor(q, state)
        if cmd == "goto":
            print(c.goto(args.x, args.y))
        elif cmd == "click":
            print(c.click_at(args.x, args.y))
        else:
            print(c.doubleclick_at(args.x, args.y))
    elif cmd in ("moveto", "clicknow", "dclicknow"):
        pos = state.pos()
        if pos is None:
            c = pointer.Cursor(q, state, log=print)
            if not c.template:
                c.calibrate()
            c.recover()
            pos = c.pos
            state.set_pos(pos)
        if cmd == "moveto":
            print("believed cursor at", pointer.blind_step(q, state, pos, args.x, args.y))
        elif cmd == "clicknow":
            q.click()
            time.sleep(0.2)
            print("clicked at", pos)
        else:
            q.click()
            time.sleep(0.08)
            q.click()
            time.sleep(0.1)
            print("double-clicked at", pos)


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        q, state = open_guest(args)
        try:
            run(args, q, state)
            if q.resets:  # the guest reset under us: what we knew of the pointer is stale
                state.invalidate()
        finally:
            q.close()
    except (qmp.QMPError, screen.ImageError, provision.ProvisionError) as e:
        print("guest: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
