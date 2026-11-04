#!/usr/bin/env python3
"""Extract the verified Shenmue I USA Dreamcast display/boot controller evidence.

Names are descriptive recovery labels. No PSP runtime state is changed.
"""

import argparse
import hashlib
import json
from pathlib import Path

from menu_layout import BASE, address, profile, unpack, write_disassembly


PROFILES = {
    "USA": {
        "module_table": 0x0C28CB28,
        "timings": 0x0C29CAD4,
        "globals": 0x0C225270,
        "callback": 0x0C183DC8,
        "callback_global_loads": [0x0C183E12, 0x0C183E14, 0x0C183E1A, 0x0C183E28],
        "attract_area_literal": 0x0C183274,
        "opening_area_literal": 0x0C18328C,
        "routines": (
            ("boot_init_module", 0x0C0EFCD6, 0x0C0EFCE6),
            ("boot_audio_module", 0x0C0EFD1A, 0x0C0EFD48),
            ("boot_set_to_display_module", 0x0C0EF848, 0x0C0EF854),
            ("boot_transition_helpers", 0x0C1831B8, 0x0C1834CE),
            ("boot_audio_by_phase", 0x0C1834CE, 0x0C183540),
            ("boot_logo_draw", 0x0C1838F4, 0x0C18396E),
            ("boot_notice_draw", 0x0C183D14, 0x0C183DC8),
        ),
    },
}

PHASES = (
    ("title_variant_0", "title", None, 1024, 512, "black"),
    ("title_fade_in", "title", None, 1024, 512, "black"),
    ("title_wait_for_start", "title", None, 1024, 512, "black"),
    ("sega_logo", "logo", 9, 256, 128, "white"),
    ("am2_logo", "logo", 7, 512, 128, "white"),
    ("dreamcast_logo", "logo", 8, 256, 256, "white"),
    ("attract_scene_handoff", "none", None, 0, 0, "black"),
    ("ys_logo", "logo", 10, 128, 128, "white"),
    ("yu_suzuki_credit", "logo", 11, 256, 256, "white"),
    ("no_vmu_notice", "NOVM.SPR", None, 512, 512, "black"),
)


def literal32(blob, instruction):
    word = unpack(blob, instruction, "H")[0]
    if word >> 12 != 0xD:
        raise ValueError(f"Expected PC-relative mov.l at {address(instruction)}")
    location = ((instruction + 4) & ~3) + (word & 255) * 4
    return unpack(blob, location, "I")[0]


def recover(blob):
    region, _, menu_routines = profile(blob)
    if region != "USA":
        raise ValueError("Boot recovery targets the USA Disc 1 executable; PAL TV modes are outside the PSP target")
    p = PROFILES[region]
    count = 10
    ticks = unpack(blob, p["timings"], f"{count}h")
    if ticks != (150, 60, 240, 60, 90, 60, 60, 60, 60, 150):
        raise ValueError("Unexpected phase timing table")
    base = p["globals"]
    offsets = {"input_delay": 0, "menu_active": 4, "menu_page": 8, "save_resume_kind": 12,
               "bookkeeping_cache": 16, "phase_countdown": 20, "alpha_or_pulse": 24,
               "step_or_hold_marker": 28, "phase": 32, "last_phase": 36,
               "blink_counter": 40, "phoenix_angle": 44, "display_task_data": 48}
    expected = [base + offsets[name] for name in
                ("menu_active", "alpha_or_pulse", "step_or_hold_marker", "phase")]
    actual = [literal32(blob, instruction) for instruction in p["callback_global_loads"]]
    if actual != expected:
        raise ValueError("Controller global references do not match the recovered profile")
    modules = [unpack(blob, p["module_table"] + i * 12, "3I") for i in range(5)]
    if modules[4][0] != next(start for name, start, _ in menu_routines if name == "display_module_init"):
        raise ValueError("Display module table mismatch")
    scenes = {}
    for kind, key, entry in (("attract", "attract_area_literal", 1), ("new_game_opening", "opening_area_literal", 0)):
        area = unpack(blob, p[key], "I")[0].to_bytes(4, "little").decode("ascii")
        if area != ("OP02" if kind == "attract" else "OP00"):
            raise ValueError(f"Unexpected {kind} area")
        scenes[kind] = {"area": area, "scene_id": 1, "entry_point": entry,
                        "target_module": 7, "area_literal": address(p[key])}
    phases = []
    for i, (name, draw, texture, width, height, clear) in enumerate(PHASES[:count]):
        item = {"phase": i, "name": name, "table_ticks": ticks[i], "draw": draw, "clear": clear}
        if texture is not None:
            item.update(texture_index=texture, center=[320, 240], size=[width, height], depth=-5)
        phases.append(item)
    routines = tuple(menu_routines) + p["routines"]
    return {
        "binary": {"region": region, "size": len(blob), "sha256": hashlib.sha256(blob).hexdigest(), "load_base": address(BASE)},
        "recovery": "Static SH-4 analysis; semantic labels and scene_id/entry_point are instruction analysis, not original symbols.",
        "module_table": {"address": address(p["module_table"]), "stride": 12,
                         "entries": [{"module": i, "callback": address(row[0]), "reserved": row[1],
                                      "reserved_data": address(row[2])} for i, row in enumerate(modules)]},
        "globals": {name: address(base + offset) for name, offset in offsets.items()},
        "execution_mode_global": address(0x0C0200E4),
        "timing_table": {"address": address(p["timings"]), "values": ticks,
                         "unit": "display callback ticks; not seconds; countdown transition occurs after decrement below zero"},
        "phases": phases,
        "initialization": {"alpha": 0, "step": 16, "normal_last_phase": 8, "other_mode_last_phase": 2,
                           "normal_previous_module_2_with_cache_0x140": 1,
                           "normal_previous_module_2_without_cache_0x140": 9,
                           "normal_previous_module_3_or_entry_49": 1,
                           "normal_return_with_old_phase_6": 7},
        "scene_handoffs": scenes,
        "routines": [{"name": name, "start": address(start), "end": address(end)} for name, start, end in routines],
        "scope": "USA display/title controller and scene handoff boundary. Scene playback and its scenario-side completion remain separate work.",
    }, routines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--disassembly-dir", type=Path)
    args = parser.parse_args()
    try:
        blob = args.binary.read_bytes()
        result, routines = recover(blob)
        if args.disassembly_dir:
            write_disassembly(blob, args.disassembly_dir, routines)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    except (OSError, ValueError) as error:
        parser.exit(1, f"boot_controller: {error}\n")


if __name__ == "__main__":
    main()
