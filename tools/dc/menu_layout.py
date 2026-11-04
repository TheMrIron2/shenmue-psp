#!/usr/bin/env python3
"""Extract verified title/menu tables from Shenmue I PAL or USA Disc 1.

Uses only the standard library, unless --disassembly-dir is requested (Capstone).
Addresses and descriptive routine names are documented in original-menu-routines.md.
"""

import argparse
import gzip
import hashlib
import json
import struct
import sys
from pathlib import Path


BASE = 0x0C010000
EXPECTED_SHA256 = "04a1394e141e858e61fb3f4dc38a82503b0cc7c98b7d1b2748064bbb098fc19f"
ROUTINES = (
    ("load_frontend_archive", 0x0C18355C, 0x0C1835CC),
    ("options_input", 0x0C183AD4, 0x0C183D66),
    ("main_menu_input", 0x0C184298, 0x0C184594),
    ("draw_title", 0x0C1847DC, 0x0C184B80),
    ("display_callback", 0x0C184C76, 0x0C184F6E),
    ("draw_layout_record", 0x0C184F80, 0x0C18506A),
    ("draw_main_menu", 0x0C185088, 0x0C1852A8),
    ("draw_options", 0x0C18533A, 0x0C185972),
    ("draw_load_screen", 0x0C185980, 0x0C18679E),
    ("title_input", 0x0C1867C0, 0x0C186876),
    ("display_module_init", 0x0C1868D4, 0x0C186ADC),
    ("build_rectangle", 0x0C17E866, 0x0C17E9AA),
    ("build_scaled_rotated_rectangle", 0x0C17E9E6, 0x0C17EC42),
    ("submit_textured_quad", 0x0C17E690, 0x0C17E866),
)
USA_SHA256 = "ca98879a97df87b668836bbfa597d38c6f9efc3fd45160208b8d5d866325f32c"
USA_ROUTINES = (
    ("load_frontend_archive", 0x0C182C6C, 0x0C182C9C),
    ("options_input", 0x0C182E00, 0x0C1830B4),
    ("main_menu_input", 0x0C183540, 0x0C1837B8),
    ("draw_title", 0x0C18396E, 0x0C183D14),
    ("display_callback", 0x0C183DC8, 0x0C18409A),
    ("draw_layout_record", 0x0C18409A, 0x0C18417C),
    ("draw_main_menu", 0x0C1841AC, 0x0C1843CC),
    ("draw_options", 0x0C18445E, 0x0C184A96),
    ("draw_load_screen", 0x0C184AA4, 0x0C1858C2),
    ("title_input", 0x0C1858E4, 0x0C18598C),
    ("display_module_init", 0x0C1859E4, 0x0C185B5E),
    ("build_rectangle", 0x0C17DCF6, 0x0C17DE3A),
    ("build_scaled_rotated_rectangle", 0x0C17DE76, 0x0C17E0D2),
    ("submit_textured_quad", 0x0C17DB20, 0x0C17DCF6),
)


def profile(blob):
    digest = hashlib.sha256(blob).hexdigest()
    if digest == EXPECTED_SHA256:
        return "PAL", 0, ROUTINES
    if digest == USA_SHA256:
        return "USA", -0xE6E6, USA_ROUTINES
    raise ValueError(f"Unsupported 1ST_READ.BIN SHA-256: {digest}; expected a documented PAL or USA Disc 1 build")


def address(value):
    return f"0x{value:08x}"


def unpack(blob, location, fmt):
    return struct.unpack_from("<" + fmt, blob, location - BASE)


def cstring(blob, location):
    return blob[location - BASE:].split(b"\0", 1)[0].decode("ascii")


def layouts(blob, start, names):
    result = []
    for index, name in enumerate(names):
        location = start + index * 10
        values = unpack(blob, location, "5h")
        result.append({"address": address(location), "role": name,
                       **dict(zip(("center_x", "center_y", "width", "height", "depth"), values))})
    return result


def texture_inventory(path):
    compressed = path.read_bytes()
    blob = gzip.decompress(compressed)
    result = []
    offset = 0
    while offset + 16 <= len(blob) and blob[offset:offset + 4] == b"TEXN":
        size = struct.unpack_from("<I", blob, offset + 4)[0]
        if size < 16 or offset + size > len(blob):
            raise ValueError(f"Invalid TEXN at {offset:#x} in {path}")
        chunk = blob[offset:offset + size]
        pvr = chunk.find(b"PVRT", 16, 80)
        if pvr < 0 or pvr + 16 > size:
            raise ValueError(f"Missing PVRT at {offset:#x} in {path}")
        pixel_format, data_format, _, width, height = struct.unpack_from("<BBHHH", chunk, pvr + 8)
        result.append({"index": len(result), "texn_offset": offset,
                       "texture_id_hex": chunk[8:16].hex(),
                       "name_hint": chunk[8:16].rstrip(b"\0").decode("ascii", "backslashreplace"),
                       "width": width, "height": height,
                       "pixel_format": pixel_format, "data_format": data_format})
        offset += size
    return {"filename": path.name, "sha256": hashlib.sha256(compressed).hexdigest(), "textures": result}


def extract(blob, assets):
    digest = hashlib.sha256(blob).hexdigest()
    region, table_delta, routines = profile(blob)

    title = layouts(blob, 0x0C2AB1CE + table_delta,
                    ("clouds", "phoenix", "main_logo", "script_subtitle", "press_start", "copyright", "ys_logo"))
    for index, record in enumerate(title):
        record["texture_index"] = index
    shared = layouts(blob, 0x0C2AB214 + table_delta, ("menu_background", "direction_symbol", "confirm_symbol", "cancel_symbol"))
    shared_indices = list(blob[0x0C2AB23C + table_delta - BASE:0x0C2AB240 + table_delta - BASE])
    for record, index in zip(shared, shared_indices):
        record["texture_index"] = index
    menu = layouts(blob, 0x0C2AB27C + table_delta, (
        "header", "help_panel", "new_game_label", "load_label", "continue_label", "options_label",
        "new_game_medallion", "load_medallion", "continue_medallion", "options_medallion", "help_text"))
    menu_indices = list(blob[0x0C2AB240 + table_delta - BASE:0x0C2AB246 + table_delta - BASE])
    for record, index in zip(menu, menu_indices):
        record["texture_index"] = index
    for index, record in enumerate(menu[6:10]):
        record["selection_index"] = index
        record["selected_texture_index"] = 15
        record["unselected_texture_index"] = 16

    help_variants = []
    for index, role in enumerate(("new_game", "load", "continue", "options",
                                  "new_game_alternate", "load_alternate", "continue_alternate",
                                  "options_alternate", "no_files")):
        location = 0x0C2AB246 + table_delta + index * 6
        texture, width, height = unpack(blob, location, "3h")
        help_variants.append({"address": address(location), "role": role,
                              "texture_index": texture, "width": width, "height": height})

    if region == "PAL":
        suffixes = [cstring(blob, unpack(blob, 0x0C2AB7A0 + index * 4, "I")[0]) for index in range(4)]
        prefixes = [cstring(blob, unpack(blob, 0x0C2AB7B0 + index * 4, "I")[0]) for index in range(2)]
        resources = {"language_suffix_table": address(0x0C2AB7A0), "suffixes": suffixes,
                     "archive_prefix_table": address(0x0C2AB7B0), "prefixes": prefixes,
                     "english_title": prefixes[0] + suffixes[0] + ".GZ",
                     "english_options": prefixes[1] + suffixes[0] + ".GZ"}
        phase_address, phase_count = 0x0C2AB1B8, 11
    else:
        filenames = [cstring(blob, unpack(blob, 0x0C29CFA0 + index * 4, "I")[0]) for index in range(2)]
        resources = {"archive_filename_table": address(0x0C29CFA0),
                     "english_title": filenames[0], "english_options": filenames[1]}
        phase_address, phase_count = 0x0C29CAD4, 10
    result = {
        "binary": {"filename": "1ST_READ.BIN", "region": region, "disc": 1, "size": len(blob),
                   "sha256": digest, "load_base": address(BASE), "address_rule": "file_offset = address - load_base"},
        "routine_names": "Descriptive analysis labels, not recovered original symbols. End addresses are exclusive.",
        "routines": [{"name": name, "start": address(start), "end": address(end)} for name, start, end in routines],
        "layout_format": {"byte_order": "little", "stride": 10, "fields": "int16 center_x, center_y, width, height, depth",
                          "alignment_argument": 5, "canvas": [640, 480]},
        "resource_selection": resources,
        "title_layout": title,
        "shared_layout": shared,
        "main_menu_layout": menu,
        "help_variants": help_variants,
        "title_phase_ticks": {"address": address(phase_address), "values": list(unpack(blob, phase_address, f"{phase_count}h")),
                              "unit": "callback ticks; wall-clock conversion not verified"},
        "options_layout": layouts(blob, 0x0C2AB358 + table_delta, [f"record_{index}" for index in range(31)]),
        "options_layout_note": "Raw records; draw_options changes some coordinates and selects textures dynamically. Roles not fully decoded.",
    }
    if assets:
        result["archives"] = [texture_inventory(assets / "SPRITE" / filename)
                              for filename in (resources["english_title"], resources["english_options"])]
    return result


def write_disassembly(blob, directory, routines=None):
    try:
        from capstone import Cs, CS_ARCH_SH, CS_MODE_LITTLE_ENDIAN, CS_MODE_SH4, CS_MODE_SHFPU
    except ImportError as error:
        raise ValueError("--disassembly-dir requires the optional capstone Python package") from error
    decoder = Cs(CS_ARCH_SH, CS_MODE_LITTLE_ENDIAN | CS_MODE_SH4 | CS_MODE_SHFPU)
    directory.mkdir(parents=True, exist_ok=True)
    for name, start, end in (profile(blob)[2] if routines is None else routines):
        lines = [f"; {name}: descriptive analysis label", "; Linear decode includes embedded literal pools; consult control flow."]
        for location in range(start, end, 2):
            raw = blob[location - BASE:location - BASE + 2]
            word = int.from_bytes(raw, "little")
            instruction = next(decoder.disasm(raw, location), None)
            text = f"{instruction.mnemonic} {instruction.op_str}" if instruction else ".word"
            if word >> 12 == 13:
                literal = ((location + 4) & ~3) + (word & 255) * 4
                text += f" ; [{address(literal)}] = {address(unpack(blob, literal, 'I')[0])}"
            elif word >> 12 == 9:
                literal = location + 4 + (word & 255) * 2
                text += f" ; [{address(literal)}] = {unpack(blob, literal, 'h')[0]}"
            elif word >> 8 == 0xC7:
                text += f" ; address {address(((location + 4) & ~3) + (word & 255) * 4)}"
            lines.append(f"{location:08x}: {word:04x}  {text}")
        (directory / f"{name}.asm").write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--assets", type=Path, help="Extracted disc root; also inventory the English TEXN archives")
    parser.add_argument("--output", type=Path, help="Write JSON here instead of stdout")
    parser.add_argument("--disassembly-dir", type=Path, help="Write annotated SH-4 listings using optional Capstone")
    args = parser.parse_args()
    try:
        blob = args.binary.read_bytes()
        result = extract(blob, args.assets)
        if args.disassembly_dir:
            write_disassembly(blob, args.disassembly_dir)
        text = json.dumps(result, indent=2) + "\n"
        if args.output:
            args.output.write_text(text)
        else:
            sys.stdout.write(text)
    except (OSError, ValueError, struct.error, EOFError) as error:
        parser.exit(1, f"menu_layout: {error}\n")


if __name__ == "__main__":
    main()
