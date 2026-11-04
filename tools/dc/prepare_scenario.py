#!/usr/bin/env python3
"""Prepare the converted USA OP02 scenario for the PSP build.

The USA disc embeds the native SH-4 SCN3 for OP02 inside
SCENE/01/OP02/MAPINFO.BIN and has no separate PC scenario file. This lifts that
chunk offline with scn3cc and installs the VM module as
`scenario/act01_OP02.scn` in the extracted disc tree. The Dreamcast asset
backend indexes the tree case-insensitively, so SCEN_LoadCurrentMapScript finds
it and applies the loader/relocation contract.

Run:
  python3 tools/dc/prepare_scenario.py USA/DISC1/extracted --scn3cc build/scn3tool/scn3cc
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path, help='USA extracted disc root')
    parser.add_argument('--scn3cc', type=Path, default=Path('build/scn3tool/scn3cc'), help='Path to the scn3 compiler')
    parser.add_argument('--output', type=Path, help='Destination directory (default: <root>/scenario)')
    parser.add_argument('--mapinfo', default='SCENE/01/OP02/MAPINFO.BIN', help='MAPINFO path relative to the disc root')
    parser.add_argument('--name', default='act01_OP02.scn', help='Output module name')
    args = parser.parse_args()

    if not args.scn3cc.is_file():
        raise SystemExit(f'scn3cc not found: {args.scn3cc} (build tools/scn3 or pass --scn3cc)')

    mapinfo = args.root / args.mapinfo
    data = mapinfo.read_bytes()
    native = None
    native_offset = 0
    native_size = 0
    for tag, offset, size in chunks(data):
        if tag == 'SCN3':
            native = data[offset:offset + size]
            native_offset = offset
            native_size = size
            break
    if native is None:
        raise SystemExit(f'SCN3 chunk missing from {mapinfo}')

    with tempfile.TemporaryDirectory(prefix='op02-') as temporary:
        source = Path(temporary) / 'op02-native.scn'
        source.write_bytes(native)
        lifted = Path(temporary) / 'op02-vm.scn'
        subprocess.run([str(args.scn3cc), str(source), '--target', 'pc', '-o', str(lifted)], check=True)
        module = lifted.read_bytes()

    if len(module) < 0x30 or module[:4] != b'SCN3':
        raise SystemExit('scn3cc output is not an SCN3 module')
    whole_size = u32(module, 4)
    flags = u32(module, 8)
    if whole_size != len(module):
        raise SystemExit(f'scn3cc output size mismatch: header {whole_size:#x}, file {len(module):#x}')
    if flags & 0x00ff0000:
        raise SystemExit(f'scn3cc output still uses the native profile (flags {flags:#x})')

    output = args.output if args.output else args.root / 'scenario'
    output.mkdir(parents=True, exist_ok=True)
    destination = output / args.name
    destination.write_bytes(module)

    manifest = {
        'source': str(mapinfo),
        'source_sha256': hashlib.sha256(data).hexdigest(),
        'native_offset': native_offset,
        'native_size': native_size,
        'native_flags': u32(native, 8),
        'converter': str(args.scn3cc),
        'converter_sha256': hashlib.sha256(args.scn3cc.read_bytes()).hexdigest(),
        'output': str(destination),
        'output_size': len(module),
        'output_sha256': hashlib.sha256(module).hexdigest(),
        'header_whole_size': whole_size,
        'header_flags': flags,
    }
    (output / (args.name + '.manifest.json')).write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'wrote {destination} ({len(module)} bytes)')
    print(f'manifest {output / (args.name + ".manifest.json")}')

    # The DC asset backend indexes the extracted tree from a flat manifest, so
    # the new module must be registered there as "<size>\t<relative path>".
    asset_manifest = args.root / 'manifest.txt'
    if asset_manifest.is_file():
        try:
            relative = destination.resolve().relative_to(args.root.resolve()).as_posix()
        except ValueError:
            print(f'warning: {destination} is outside {args.root}; add it to the asset manifest manually')
        else:
            lines = [line for line in asset_manifest.read_text().splitlines()
                     if line.split('\t', 1)[-1].lstrip('/') != relative]
            lines.append(f'{len(module)}\t{relative}')
            asset_manifest.write_text('\n'.join(lines) + '\n')
            print(f'asset manifest {asset_manifest} (+{relative})')
    else:
        print(f'warning: {asset_manifest} not found; the DC backend needs a manifest entry')


if __name__ == '__main__':
    main()
