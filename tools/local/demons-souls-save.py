#!/usr/bin/env python3
"""Demon's Souls (PPSA01341) saves: what each slot holds, and test saves that start on another map.

    demons-souls-save.py info SAVE_DIR                  every USR-DATA under SAVE_DIR/PPSA01341
    demons-souls-save.py set-map SRC_DIR DST_DIR MAPUID  copy SRC_DIR (PPSA01341 and .kyty-capacity) to
                                                        DST_DIR, the copy's character on map MAPUID
                                                        (e.g. 0x03000000) at the map's default start

A test save is a bench baseline: KYTY_BENCH_BASELINE=DST_DIR for tools/local/bench-windows.py.
MAPUIDs seen: 0x08000000 the tutorial (the bench baseline), 0x01000000 the Nexus, 0x02000000 the first
world; 0x03000000 crashes the game and 0x04000000 returns to the title (other worlds need other IDs).
USR-DATA: u32 version, u32 FNV-1a 32 of the payload, u32 payload size, u32, payload: entries of
key (FNV-1a 32 of an upper-case name: MAPUID, PLAYTIME, PLAYER_NAME, ...), type byte, value. A
character with no saved position (as a new game's) starts at the map's default start.
"""
import argparse
import shutil
import struct
import sys
from pathlib import Path


def fnv1a(data):
    h = 0x811c9dc5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def key(name):
    return struct.pack('<I', fnv1a(name.encode()))


MAPUID = key('MAPUID') + b'\x04'
POSITION = struct.pack('<I', 0xfb0faeb2) + b'\x0e\x10'  # name unknown: 16-byte blob x y z 1
ROTATION = struct.pack('<I', 0x739273b8) + b'\x0e\x10'  # name unknown: 16-byte blob 0 yaw 0 0


def fields(data):
    out = {}
    at = data.find(MAPUID)
    if at >= 0:
        out['map'] = f'{struct.unpack_from("<I", data, at + 5)[0]:#010x}'
    at = data.find(key('PLAYTIME') + b'\x0a')
    if at >= 0:
        out['playtime'] = f'{struct.unpack_from("<f", data, at + 5)[0] / 1000 / 60:.0f} min'
    at = data.find(key('PLAYER_NAME') + b'\x0d')
    if at >= 0:
        out['name'] = data[at + 6:at + 6 + data[at + 5]].decode('latin-1')
    at = data.find(POSITION)
    if at >= 0:
        out['position'] = ' '.join(f'{v:.1f}' for v in struct.unpack_from('<3f', data, at + 6))
    return out


def info(save_dir):
    for path in sorted(Path(save_dir, 'PPSA01341').glob('*/USR-DATA'), key=lambda p: p.stat().st_mtime):
        data = path.read_bytes()
        valid = fnv1a(data[16:]) == struct.unpack_from('<I', data, 4)[0]
        print(f'{path.parent.name:28} {"" if valid else "BAD CHECKSUM "}' +
              '  '.join(f'{k}={v}' for k, v in fields(data).items()))


def set_map(src, dst, mapuid, profile):
    dst = Path(dst)
    if dst.exists():
        sys.exit(f'{dst} exists')
    path = Path(src, 'PPSA01341', profile, 'USR-DATA')
    data = path.read_bytes()
    if fnv1a(data[16:]) != struct.unpack_from('<I', data, 4)[0]:
        sys.exit(f'{path}: checksum does not match')
    payload = bytearray(data[16:])
    at = payload.find(MAPUID)
    if at < 0 or payload.find(MAPUID, at + 1) >= 0:
        sys.exit(f'{path}: no single MAPUID')
    old = struct.unpack_from('<I', payload, at + 5)[0]
    struct.pack_into('<I', payload, at + 5, mapuid)
    for field in (POSITION, ROTATION):
        at = payload.find(field)
        if at >= 0:
            del payload[at:at + len(field) + 16]
    shutil.copytree(src, dst)
    header = struct.pack('<4I', struct.unpack_from('<I', data, 0)[0], fnv1a(payload), len(payload),
                         struct.unpack_from('<I', data, 12)[0])
    Path(dst, 'PPSA01341', profile, 'USR-DATA').write_bytes(header + bytes(payload))
    print(f'{dst}: {profile} map {old:#010x} -> {mapuid:#010x} at its default start')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('info')
    p.add_argument('save_dir')
    p = sub.add_parser('set-map')
    p.add_argument('src')
    p.add_argument('dst')
    p.add_argument('mapuid', type=lambda s: int(s, 0))
    p.add_argument('--profile', default='SAVEDATA0PlayerProfile0')
    args = parser.parse_args()
    if args.command == 'info':
        info(args.save_dir)
    else:
        set_map(args.src, args.dst, args.mapuid, args.profile)
