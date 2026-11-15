#!/usr/bin/env python3
"""Write a keyboard script for the smoke test: evkeys.py out.ev MS:CODE:VALUE...

Each argument becomes one evdev key event (VALUE 1 = press, 0 = release) for
key CODE, stamped MS milliseconds after the game starts. czduke plays such a
file back against its virtual clock when CZDUKE_MAX_FRAMES is set.
"""
import struct
import sys

with open(sys.argv[1], "wb") as f:
    for arg in sys.argv[2:]:
        ms, code, value = (int(v) for v in arg.split(":"))
        # struct input_event on 64-bit Linux: timeval (2 x long), type, code, value
        f.write(struct.pack("qqHHi", ms // 1000, (ms % 1000) * 1000, 1, code, value))
