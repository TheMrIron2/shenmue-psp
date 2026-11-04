#!/usr/bin/env python3
"""Audit USA OP02's original scene packs without changing disc assets."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib

from boot_controller import recover


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def chunks(data, start=0, end=None):
    end = len(data) if end is None else end
    while start + 8 <= end:
        tag, size = struct.unpack_from('<4sI', data, start)
        if size < 8 or start + size > end:
            raise ValueError(f'Invalid chunk at {start:#x}')
        yield tag.decode('ascii'), start, size
        start += size
    if start != end:
        raise ValueError('Incomplete chunk header')


def text(data, offset):
    if not 0 <= offset < len(data):
        raise ValueError('String outside resource')
    return data[offset:data.index(b'\0', offset)].decode('ascii')


def tokens(data, start, end):
    result = []
    while start < end:
        at = start
        if start + 8 > end:
            raise ValueError('Incomplete character token')
        name = text(data, start + u32(data, start))
        types = u32(data, start + 4)
        start += 8
        args = []
        while types:
            if start + 4 > end:
                raise ValueError('Character argument outside block')
            kind, value = types & 7, u32(data, start)
            if kind == 1:
                args.append(struct.unpack_from('<f', data, start)[0])
            elif kind == 2:
                raw = data[start:start + 4]
                args.append({'word': value, 'fourcc': raw.decode('ascii') if all(32 <= c < 127 for c in raw) else None})
            elif kind == 3:
                args.append(text(data, start + value))
            elif kind == 4:
                block_end = start + 4 + 4 * value
                if block_end > end:
                    raise ValueError('Character block outside resource')
                args.append(tokens(data, start + 4, block_end))
                start += 4 * value
            else:
                raise ValueError(f'Unknown character argument {kind}')
            start += 4
            types >>= 3
        result.append({'offset': at, 'token': name, 'args': args})
    return result


def pack(raw):
    data = zlib.decompress(raw, 31) if raw[:2] == b'\x1f\x8b' else raw
    directory = u32(data, 4)
    if data[:4] not in (b'PAKF', b'PAKS') or data[directory:directory + 4] != b'IPAC':
        raise ValueError('Expected PAKF/PAKS with IPAC directory')
    count, entries = struct.unpack_from('<II', data, directory + 8)
    result = {'decoded_bytes': len(data), 'entries': []}
    for index in range(count):
        name, tag, relative, size = struct.unpack_from('<8s4sII', data, directory + entries + index * 20)
        offset = directory + relative
        if offset + size > len(data):
            raise ValueError('Pack entry outside resource')
        entry = {'name': name.rstrip(b'\0').decode('ascii'), 'tag': tag.decode('ascii'), 'offset': offset, 'bytes': size}
        if tag == b'CHRT':
            content = data[offset:offset + size]
            if content[:4] != b'CHRS':
                raise ValueError('Expected character definitions')
            entry['tokens'] = tokens(content, 8, u32(content, 4))
        result['entries'].append(entry)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path, help='USA extracted disc root')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scn3cc', type=Path, help='Also lift/disassemble and export key original script functions')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'boot': recover((args.root / '1ST_READ.BIN').read_bytes())[0]['scene_handoffs']}
    scene = args.root / 'SCENE/01/OP02'
    data = (scene / 'MAPINFO.BIN').read_bytes()
    report['mapinfo_sha256'] = hashlib.sha256(data).hexdigest()
    report['chunks'] = []
    native = None
    for tag, offset, size in chunks(data):
        entry = {'tag': tag, 'offset': offset, 'bytes': size}
        if tag == 'SCN3':
            native = data[offset:offset + size]
            entry['native_flags'] = u32(native, 8)
            entry['startup_offset'] = u32(native, 12)
        if tag == 'SCEX':
            entry['tracks'] = []
            for child, start, length in chunks(data, offset + 8, offset + size):
                track = {'tag': child, 'offset': start, 'bytes': length}
                if child == 'TRCK':
                    track['children'] = []
                    for t, o, n in chunks(data, start + 8, start + length):
                        child_entry = {'tag': t, 'offset': o, 'bytes': n}
                        if t == 'ASEQ':
                            child_entry['frame_count'] = u32(data, o + 12)
                        track['children'].append(child_entry)
                entry['tracks'].append(track)
        report['chunks'].append(entry)
    report['packs'] = {name: pack((scene / name).read_bytes()) for name in ('SHDR.PKF', 'SHDR.PKS', 'SHD2.PKF', 'SHD2.PKS')}
    afs = (scene / 'OP02.AFS').read_bytes()
    if afs[:4] != b'AFS\0' or u32(afs, 4) != 2:
        raise ValueError('Unexpected OP02 AFS directory')
    report['afs'] = []
    for index, name in enumerate(('SHD2.PKF', 'SHD2.PKS')):
        offset, size = struct.unpack_from('<II', afs, 8 + index * 8)
        matches = afs[offset:offset + size] == (scene / name).read_bytes()
        if not matches:
            raise ValueError(f'AFS entry differs from {name}')
        report['afs'].append({'index': index, 'offset': offset, 'bytes': size, 'identical_to': name})
    (args.output / 'trace.json').write_text(json.dumps(report, indent=2) + '\n')
    if args.scn3cc:
        if native is None:
            raise ValueError('SCN3 missing')
        source = args.output / 'op02-native.scn'
        source.write_bytes(native)
        def run(*options):
            subprocess.run([str(args.scn3cc.resolve()), str(source), *map(str, options)], check=True)
        run('--disassemble', args.output / 'op02.scn3asm')
        run('--target', 'pc', '-o', args.output / 'op02-vm.scn')
        for function in ('0030', '0C00', '0C5A', '0D6C', '0E2B', '0E58', '0F33', '1011'):
            run('--export-c', args.output / f'script_{function}.c', '--only-function', f'script_{function}')
    print(f'USA OP02 audit written to {args.output / "trace.json"}')


if __name__ == '__main__':
    main()
