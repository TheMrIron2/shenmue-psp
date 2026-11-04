#!/usr/bin/env python3
"""Extract a Dreamcast GDI disc to a flat directory using its ISO9660 tree.

Shenmue's GD-ROM stores the ISO9660 filesystem on the high-density data track
(usually Track 3 at LBA 45000), but file extents are *absolute disc LBAs* and
can live on later data tracks (Track 6). This tool resolves each sector to the
correct track, strips the raw Mode 1 framing to 2048-byte payloads, and writes
the named files out.

Usage:
    python3 tools/dc/extract.py DISC1/foo.gdi -o DISC1/extracted [--manifest m.json]
    python3 tools/dc/extract.py DISC1/foo.gdi --list
"""

import argparse
import json
import os
import shlex
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Track:
    number: int
    lba: int
    kind: int
    stride: int
    path: Path
    offset: int

    @property
    def count(self):
        return (self.path.stat().st_size - self.offset) // self.stride


def read_tracks(gdi):
    tracks = []
    for line in gdi.read_text().splitlines()[1:]:
        fields = shlex.split(line)
        if not fields:
            continue
        number, lba, kind, stride = map(int, fields[:4])
        tracks.append(Track(number, lba, kind, stride, (gdi.parent / fields[4]).resolve(), int(fields[5])))
    return tracks


def find_track(tracks, lba):
    for track in tracks:
        if track.kind == 4 and track.lba <= lba < track.lba + track.count:
            return track
    raise ValueError(f"No data track contains LBA {lba}")


def payload_sector(tracks, lba):
    track = find_track(tracks, lba)
    with track.path.open("rb") as source:
        source.seek(track.offset + (lba - track.lba) * track.stride)
        sector = source.read(track.stride)
    if len(sector) != track.stride:
        raise ValueError("Truncated sector")
    if track.stride == 2048:
        return sector
    if sector[:12] != b"\0" + b"\xff" * 10 + b"\0" or sector[15] != 1:
        raise ValueError(f"LBA {lba} is not a Mode 1 data sector")
    return sector[16:2064]


def walk_iso(tracks, out, path="", depth=0):
    if depth > 16:
        return
    for record in read_dir(tracks, out["lba"], out["size"]):
        name = record["name"]
        if name in ("\0", "\1"):
            continue
        full = f"{path}/{name}"
        if record["flags"] & 2:
            yield from walk_iso(tracks, record, full, depth + 1)
        else:
            yield full, record


def read_dir(tracks, lba, size):
    data = b"".join(payload_sector(tracks, lba + i) for i in range((size + 2047) // 2048))
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        record = data[pos:pos + length]
        pos += length
        name_length = record[32]
        raw_name = record[33:33 + name_length].decode("ascii", "replace")
        name = raw_name.split(";")[0]
        yield {
            "name": name,
            "lba": struct.unpack_from("<I", record, 2)[0],
            "size": struct.unpack_from("<I", record, 10)[0],
            "flags": record[25],
        }


def extract_file(tracks, record, destination):
    remaining = record["size"]
    lba = record["lba"]
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as output:
        while remaining:
            chunk = payload_sector(tracks, lba)[:remaining]
            output.write(chunk)
            remaining -= len(chunk)
            lba += 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gdi", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--skip-existing", action="store_true")
    args = parser.parse_args()

    tracks = read_tracks(args.gdi.resolve())
    data_track = next((t for t in tracks if t.kind == 4 and t.lba >= 45000), None)
    if data_track is None:
        raise SystemExit("No high-density data track (LBA >= 45000)")

    pvd = payload_sector(tracks, data_track.lba + 16)
    if pvd[:6] != b"\1CD001":
        raise SystemExit("Missing ISO9660 primary volume descriptor")
    root = {
        "lba": struct.unpack_from("<I", pvd, 156 + 2)[0],
        "size": struct.unpack_from("<I", pvd, 156 + 10)[0],
    }

    entries = list(walk_iso(tracks, root))
    manifest = []
    for path, record in entries:
        manifest.append({"path": path, "lba": record["lba"], "size": record["size"]})

    if args.list:
        for item in manifest:
            print(f"{item['size']:>10}  LBA {item['lba']:>7}  {item['path']}")
        print(f"# {len(manifest)} files, {sum(i['size'] for i in manifest)/1048576:.1f} MB", file=sys.stderr)
        return

    if args.manifest:
        args.manifest.write_text(json.dumps(manifest, indent=1) + "\n")
        print(f"wrote manifest {args.manifest} ({len(manifest)} files)")

    if not args.output:
        raise SystemExit("--output is required unless --list is used")

    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "manifest.txt").open("w") as manifest_txt:
        for path, record in entries:
            manifest_txt.write(f"{record['size']}\t{path.lstrip('/')}\n")

    for index, (path, record) in enumerate(entries, 1):
        relative = path.lstrip("/")
        destination = args.output / relative
        if args.skip_existing and destination.exists():
            continue
        extract_file(tracks, record, destination)
        if index % 200 == 0:
            print(f"  {index}/{len(entries)} {relative}")

    print(f"extracted {len(entries)} files to {args.output}")


if __name__ == "__main__":
    main()
