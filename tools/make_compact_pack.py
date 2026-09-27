#!/usr/bin/env python3
"""Packs YOUR OWN game files into one compact, lossless pack (.rdpk) for the
"Compact" Android app: every piece of content is stored once.

Rayman Origins keeps each level in its own .ipk bundle, and each bundle carries
its own copy of everything it uses (music, sounds, Rayman's sprites...): 5.4 GB
of files hold about 1.1 GB of unique data. The runtime reads the pack as if it
were the original files (android/rexglue-patches: the RDPK device), byte for
byte the same.

Format (little-endian):
  header   'RDPK', u32 version (1), u32 file count, u32 reserved,
           u64 table offset, u64 table size, u64 store offset, u64 store size
  store    unique byte runs, each 16-byte aligned
  table    per file: u16 path length, UTF-8 path ('/' separators, relative to
           the game folder), u64 size, u32 extent count, then per extent
           u64 file offset, u64 length, u64 store offset (absolute in the pack).
           Extents are sorted and cover the file exactly.

It contains the game: keep it to yourself, never publish or share it.

Usage: python tools/make_compact_pack.py [game dir] [out.rdpk]
"""
import hashlib
import os
import struct
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
game = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'private', 'game')
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, 'private', 'dist', 'RaymanOrigins-compact.rdpk')
HEADER = 48
ALIGN = 16


def ipk_ranges(data):
    """(offset, length) of every entry's stored data in a UbiArt IPK (v3)."""
    if len(data) < 0x30 or struct.unpack('>I', data[:4])[0] != 0x50EC12BA:
        return []
    base = struct.unpack('>I', data[12:16])[0]
    ranges, p = [], 0x30
    while p + 24 < base:
        count, size, zsize = struct.unpack('>III', data[p:p + 12])
        p += 20
        offset, = struct.unpack('>Q', data[p:p + 8])
        p += 8 * count
        n, = struct.unpack('>I', data[p:p + 4])
        p += 4 + 2 * n
        length = zsize or size
        if length and base + offset + length <= len(data):
            ranges.append((base + offset, length))
    return ranges


def split(data, ranges):
    """Cuts a file into runs: the entries, and the bytes between them."""
    runs, pos = [], 0
    for at, length in sorted(set(ranges)):
        if at < pos:  # overlapping entry: keep what isn't covered yet
            if at + length <= pos:
                continue
            length -= pos - at
            at = pos
        if at > pos:
            runs.append((pos, at - pos))
        runs.append((at, length))
        pos = at + length
    if pos < len(data):
        runs.append((pos, len(data) - pos))
    return runs


def main():
    if not os.path.isfile(os.path.join(game, 'default.xex')):
        sys.exit(f'{game}: default.xex not found')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    files = []
    for dirpath, _, names in os.walk(game):
        for name in names:
            path = os.path.join(dirpath, name)
            files.append((os.path.relpath(path, game).replace(os.sep, '/'), path))
    files.sort()

    stored = {}  # sha1 -> store offset
    table = []
    total = 0
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(b'\0' * HEADER)
        for rel, path in files:
            data = open(path, 'rb').read()
            total += len(data)
            runs = split(data, ipk_ranges(data)) if rel.endswith('.ipk') else [(0, len(data))]
            extents = []
            for at, length in runs:
                chunk = data[at:at + length]
                key = hashlib.sha1(chunk).digest() + struct.pack('<Q', length)
                where = stored.get(key)
                if where is None:
                    pad = (-f.tell()) % ALIGN
                    f.write(b'\0' * pad)
                    where = f.tell()
                    f.write(chunk)
                    stored[key] = where
                extents.append((at, length, where))
            table.append((rel, len(data), extents))
            print(f'\r{len(table)}/{len(files)} files, pack {f.tell() >> 20} MB', end='', flush=True)
        store_size = f.tell() - HEADER
        table_offset = f.tell()
        for rel, size, extents in table:
            name = rel.encode('utf-8')
            f.write(struct.pack('<H', len(name)) + name + struct.pack('<QI', size, len(extents)))
            for e in extents:
                f.write(struct.pack('<QQQ', *e))
        table_size = f.tell() - table_offset
        f.seek(0)
        f.write(b'RDPK' + struct.pack('<IIIQQQQ', 1, len(table), 0, table_offset, table_size, HEADER, store_size))
    os.replace(tmp, out)
    size = os.path.getsize(out)
    print(f'\n{out}: {len(table)} files, {total / 2**30:.2f} GB -> {size / 2**30:.2f} GB ({len(stored)} unique runs)')
    verify(out, files)


def verify(pack, files):
    """Rebuilds every file from the pack and compares it with the original."""
    with open(pack, 'rb') as f:
        head = f.read(HEADER)
        _, count, _, table_offset, table_size, _, _ = struct.unpack('<IIIQQQQ', head[4:])
        f.seek(table_offset)
        table = f.read(table_size)
        p = 0
        originals = dict(files)
        for i in range(count):
            n, = struct.unpack_from('<H', table, p); p += 2
            rel = table[p:p + n].decode('utf-8'); p += n
            size, extents = struct.unpack_from('<QI', table, p); p += 12
            h, pos = hashlib.sha1(), 0
            for _ in range(extents):
                at, length, where = struct.unpack_from('<QQQ', table, p); p += 24
                assert at == pos, f'{rel}: gap at {pos}'
                f.seek(where)
                h.update(f.read(length))
                pos += length
            assert pos == size, f'{rel}: covers {pos} of {size}'
            assert h.digest() == hashlib.sha1(open(originals[rel], 'rb').read()).digest(), f'{rel}: differs'
            print(f'\rverified {i + 1}/{count}', end='', flush=True)
    print(' - every file rebuilds byte for byte')


if __name__ == '__main__':
    main()
