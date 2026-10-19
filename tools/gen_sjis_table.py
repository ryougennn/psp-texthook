#!/usr/bin/env python3
"""Writes the Shift-JIS (CP932) -> Unicode table texthook.prx loads from
ms0:/SEPLUGINS/texthook/sjis.tbl.

Layout: little-endian u16 code points for every double-byte code,
lead-major. Leads 0x81-0x9F then 0xE0-0xFC (60), trails 0x40-0xFC (189,
0x7F included as an invalid slot). Unmapped codes are U+FFFD.
Single-byte codes (ASCII, half-width katakana) are decoded in code.
"""
import struct
import sys

LEADS = list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD))
TRAILS = range(0x40, 0xFD)


def code_point(lead, trail):
    try:
        s = bytes((lead, trail)).decode("cp932")
    except UnicodeDecodeError:
        return 0xFFFD
    return ord(s) if len(s) == 1 and ord(s) <= 0xFFFF else 0xFFFD


table = b"".join(struct.pack("<H", code_point(l, t)) for l in LEADS for t in TRAILS)
assert len(table) == 60 * 189 * 2
with open(sys.argv[1], "wb") as f:
    f.write(table)
