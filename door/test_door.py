#!/usr/bin/env python3
"""
test_door.py -- runs the C64 door on a pseudo-terminal and pretends to be SyncTERM, the way the other doors' tests
pretend to be TERMinator. No BBS and no terminal needed.

It answers the door's questions (CTerm version, JPEG XL, sound files, key reports, screen size), agrees to the
disclaimer, picks a library title, plays it for a while with the joystick (key reports: right held, fire pulses),
opens the F12 menu and leaves, then quits the door. It answers every frame's cursor-position request, so the door's
pacing runs as on a fast link.

    python3 test_door.py                    # library title 1 (Hessian), 20 s
    python3 test_door.py --title 2 --seconds 60 --swap-at 25   # the demo, side 1a in at 25 s
    python3 test_door.py --title 2 --seconds 60 --reset-at 30   # F12, R at 30 s: back at BASIC, not reloading
    python3 test_door.py --kept --seconds 30   # kept disk 1 from players/<player>/ (put a .d64 there first)
    python3 test_door.py --basic            # no disk: switch on at the BASIC prompt and type a line
    python3 test_door.py --nokeys           # a terminal without key reports (typed keys only)
    python3 test_door.py --rows 24          # an 80x24 SyncTERM (640x384): the picture fills all but the last row

Run it in the folder the door is installed in (or give --door). Never in the live door folder on the BBS: the door
writes its log under logs/ beside itself.
"""

import argparse
import os
import pty
import re
import select
import sys
import time

p = argparse.ArgumentParser()
p.add_argument("--door", default="./c64door")
p.add_argument("--title", type=int, default=1)
p.add_argument("--seconds", type=float, default=20)
p.add_argument("--basic", action="store_true")
p.add_argument("--nokeys", action="store_true")
p.add_argument("--nosound", action="store_true")
p.add_argument("--swap-at", type=float, default=0, help="seconds in: F12, D (next disk), back to the C64")
p.add_argument("--kept", action="store_true", help="boot kept disk 1 (F12, M, 1, A) instead of a library title")
p.add_argument("--reset-at", type=float, default=0, help="seconds in: F12, R (reset to BASIC, no autostart)")
p.add_argument("--rows", type=int, default=25, help="the screen's text rows: 25 is 640x400, 24 is 640x384")
args = p.parse_args()

pid, fd = pty.fork()
if pid == 0:
    os.execv(args.door, [args.door])

out = bytearray()           # everything the door has sent
seen = 0                    # how much of it the responder has looked at
stats = {"frames": 0, "frame_bytes": 0, "sound_pieces": 0, "acks": 0, "asked_size": False}


def send(data):
    os.write(fd, data if isinstance(data, bytes) else data.encode())


def pump(timeout=0.05):
    """Reads what the door sent and answers its questions. False when the door has gone."""
    global seen
    r, _, _ = select.select([fd], [], [], timeout)
    if r:
        try:
            data = os.read(fd, 65536)
        except OSError:
            return False
        if not data:
            return False
        out.extend(data)
    new = bytes(out[max(0, seen - 16):])
    start = max(0, seen - 16)
    for m in re.finditer(rb"\x1b\[c|\x1b\[5n|\x1b\[6n|C;DrawJXLBlob;[^;]*;[^;]*;(?:[^;]*;[^;]*;)?|A;Queue;", new):
        if start + m.end() <= seen:
            continue
        tok = m.group(0)
        if tok == b"\x1b[c":
            send("\x1b[=67;84;101;114;109;1;332c")
        elif tok == b"\x1b[5n":
            answers = "\x1b[=1;1-n"
            if not args.nosound:
                answers += "\x1b[=7;100;1n\x1b[=7;101;1;5;1n"
            if not args.nokeys:
                answers += "\x1b[<0;8c"
            answers += f"\x1b[?2;0;640;{args.rows * 16}S\x1b[0n"
            send(answers)
            stats["asked_size"] = True
        elif tok == b"\x1b[6n" and not stats["asked_size"]:
            send(f"\x1b[{args.rows};80R")               # the screen-size question: the cursor stopped at the corner
        elif tok == b"\x1b[6n":
            send("\x1b[1;1R")
            stats["acks"] += 1
        elif tok.startswith(b"C;DrawJXLBlob"):
            stats["frames"] += 1
        elif tok == b"A;Queue;":
            stats["sound_pieces"] += 1
    seen = len(out)
    return True


def wait_for(text, timeout=15):
    mark = len(out)
    end = time.time() + timeout
    while time.time() < end:
        if not pump():
            return False
        if text.encode() in out[max(0, mark - 200):]:
            return True
    print(f"FAIL: never saw {text!r}")
    print(bytes(out[-600:]))
    sys.exit(1)


def play_for(seconds):
    start = time.time()
    t = 0
    while time.time() - start < seconds:
        pump(0.02)
        now = time.time() - start
        if now > t:
            t += 0.5
            if args.nokeys:
                send("\x1b[C")                       # a typed arrow
                if int(now * 2) % 4 == 0:
                    send(" ")                        # fire, typed
            else:
                send("\x1b[=106K")                   # right held
                if int(now * 2) % 4 == 0:
                    send("\x1b[=97K")                # fire (right Ctrl) down...
                elif int(now * 2) % 4 == 1:
                    send("\x1b[=97k")                # ...and up


def open_menu(ctrl_q=False):
    if args.nokeys:
        send("\x11" if ctrl_q else "\x1b[24~")
    elif ctrl_q:
        send("\x1b[=106k\x1b[=29K\x1b[=16K\x1b[=16k\x1b[=29k")     # let go of right; Ctrl-Q
    else:
        send("\x1b[=106k\x1b[=88K\x1b[=88k")                       # let go of right; F12
    wait_for("C64 paused")


wait_for("Press Y to agree")
send("y")
wait_for("Press any key to switch on")
send(" ")
play_for(3)                                          # the C64 at the BASIC prompt
frames_before = stats["frames"]
t0 = time.time()
if args.basic:
    for ch in '10 PRINT "HELLO"\r':
        send(ch)
    play_for(args.seconds)
else:
    open_menu(ctrl_q=True)                           # Ctrl-Q opens the same menu as F12
    if args.kept:
        send("m")
        wait_for("kept.")
        send("1")
        wait_for("keeps what's in")
        send("c" if b"A cartridge" in out else "a")  # the C64 restarts with kept disk (or cartridge) 1
    else:
        send("l")
        wait_for("= load it and start")
        send(str(args.title))                        # the C64 restarts with that title
    if args.swap_at > 0:
        play_for(args.swap_at)
        open_menu()
        send("d")
        wait_for("disk 2 of")
        send(" ")
        play_for(args.seconds - args.swap_at)
    elif args.reset_at > 0:
        play_for(args.reset_at)
        open_menu()
        send("r")                                    # the C64 restarts at the BASIC prompt, disk still in drive 8
        play_for(args.seconds - args.reset_at)
    else:
        play_for(args.seconds)
elapsed = time.time() - t0
frames = stats["frames"] - frames_before
mb = len(out) / 1048576

open_menu()
send("q")                                            # Q: back to the BBS
end = time.time() + 15
while time.time() < end:
    pump()
    tail = bytes(out[-3000:])
    if b"Download the changed disk" in tail:
        send("n")
        out.extend(b"[answered]")
    if b"Thanks for visiting" in tail:
        break
wait_for("Press any key")
send(" ")
for _ in range(100):
    if not pump():
        break

print(f"played {elapsed:.1f} s: {frames} pictures ({frames / elapsed:.1f} a second), {mb:.2f} MB sent in all "
      f"({len(out) / elapsed / 1024:.0f} KB/s), {stats['sound_pieces']} sound pieces "
      f"({stats['sound_pieces'] / elapsed:.1f} a second), {stats['acks']} frame answers")
print("key reports were switched on" if b"\x1b[=1h" in out else "key reports were NOT switched on")
_, status = os.waitpid(pid, 0)
print(f"door exited with {os.waitstatus_to_exitcode(status)}")
bar = f"\x1b[{args.rows};1H\x1b[0;44m".encode() in out
print(f"controls row drawn on row {args.rows}" if bar else f"controls row NOT drawn on row {args.rows}")
ok = frames > 0 and (args.nosound or stats["sound_pieces"] > 0) and bar
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
