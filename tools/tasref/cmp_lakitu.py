#!/usr/bin/env python3
"""Compare gLakituState field by field: a tasref N64 RAM dump (TASREF_DUMP_AT) against the
pc-endjinn host's dump of the struct (SM64_DUMP_AT) at the same movie sample.

    tools/tasref/cmp_lakitu.py tasref_rdram_<n>.bin sm64_lakitu_<n>.bin
"""
import struct
import sys

LAKITU = 0x8033C698
FIELDS = [  # offset, name, type ('f' float, 'h' s16, 'B' u8, 'I' u32), count
    (0x00, "curFocus", "f", 3), (0x0C, "curPos", "f", 3), (0x18, "goalFocus", "f", 3), (0x24, "goalPos", "f", 3),
    (0x3C, "mode", "B", 1), (0x3D, "defMode", "B", 1), (0x48, "focusDistance", "f", 1),
    (0x52, "shakeMagnitude", "h", 3), (0x58, "shakePitch", "h", 3), (0x7A, "roll", "h", 1), (0x7C, "yaw", "h", 1),
    (0x7E, "nextYaw", "h", 1), (0x80, "focus", "f", 3), (0x8C, "pos", "f", 3), (0x98, "shakeRoll", "h", 3),
    (0x9E, "shakeYaw", "h", 3), (0xA4, "focHSpeed", "f", 1), (0xA8, "focVSpeed", "f", 1), (0xAC, "posHSpeed", "f", 1),
    (0xB0, "posVSpeed", "f", 1), (0xB4, "keyDanceRoll", "h", 1), (0xB8, "lastFrameAction", "I", 1),
]
SIZE = {"f": 4, "h": 2, "B": 1, "I": 4}


def n64_read(ram, addr, kind):
    """Mupen keeps RDRAM as little-endian 32-bit words of the big-endian N64 memory."""
    off = addr & 0x7FFFFF
    word = struct.unpack_from("<I", ram, off & ~3)[0]
    if kind in "fI":
        return struct.unpack("<f" if kind == "f" else "<I", struct.pack("<I", word))[0]
    if kind == "h":
        half = word >> 16 if (off & 2) == 0 else word & 0xFFFF
        return struct.unpack("<h", struct.pack("<H", half))[0]
    return (word >> (24 - 8 * (off & 3))) & 0xFF


def main():
    ram = open(sys.argv[1], "rb").read()
    host = open(sys.argv[2], "rb").read()
    for off, name, kind, count in FIELDS:
        a = [n64_read(ram, LAKITU + off + i * SIZE[kind], kind) for i in range(count)]
        b = [struct.unpack_from("<" + {"f": "f", "h": "h", "B": "B", "I": "I"}[kind], host, off + i * SIZE[kind])[0]
             for i in range(count)]
        mark = "  " if a == b else "**"
        print(f"{mark} {name:16s} n64 {a}\n{'':19s}host {b}" if a != b else f"{mark} {name:16s} {a}")


if __name__ == "__main__":
    main()
