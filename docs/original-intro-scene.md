# USA original intro scene trace

**OP02 is the eagle/canyon/Shenhua attract intro.** The earlier identification of it as an AKIR demonstration on a black background was incorrect. Its base CHRD describes the player placeholder, not the complete scene. The original scenario hides that player and loads the actual scene from packs.

This is a static trace of the supplied USA Disc 1 assets and the existing engine routines. It does not claim PSP playback of the recovered sequence.

## Boot entry

The hash-checked USA `1ST_READ.BIN` boot controller distinguishes two routes:

| Route | SH-4 routine | Scene / area / entry | Module |
| --- | --- | --- | --- |
| Title idle, after SEGA/AM2/Dreamcast logos | `0x0c1831de` | `1 / OP02 / 1` | Map (7) |
| Accepted New Game | `0x0c1831f4` | `1 / OP00 / 0` | Map (7) |

OP00 is the new-game opening, not the destination to substitute for this attract intro. The current OP02 boot destination is correct.

## Why Ryo appears

OP02 MAPINFO's base CHRD declares `YKB_M` and one Player character. MAPR contains -1, so there is no base map model. Neither fact describes what the scenario will subsequently load.

The embedded SCN3 startup disables player control and creates a script task associated with AKIR, the initial player actor. The task loads the first scene pack, then calls both `Script_scenToggleVisibleFor(CHARA_AKIR, 0)` and `Script_SetCharaVisibility(CHARA_AKIR, 2)`. Mode 2 clears the draw visibility bit in the existing character command implementation.

The PSP currently renders this initial player without executing the scenario. Ryo is therefore a visible loading placeholder caused by the missing scenario path, not the intended intro performer. Do not add a special-case hidden-Ryo flag to imitate that part of the script.

## Original startup and loading order

The function labels below are generated from the **lifted VM offsets**, not SH-4 executable addresses or original symbols. Single-function exports avoid the full-file decompiler's overlapping-data limitation.

1. `script_0F33` disables player control, initializes presentation state and creates `script_1011` associated with AKIR.
2. `script_1011` establishes a black film transition and calls `script_0C5A`.
3. `script_0C5A` queues `/scene/01/OP02/SHDR`, waits for the pack loader, makes SHDR ready, configures HAWK, hides AKIR and starts the bird's motion.
4. `script_1011` configures camera flags, dynamic-map slot 1 at `(5020, -210, -5620)` with angle `0xC000`, LOD, scroll presentation and voice bank `A0100`, then calls `script_0030(7)`.
5. `script_0030` creates a concurrent `script_0D6C` loader and starts SCEX sequence 0. `script_0D6C` opens `OP02.afs`, loads its paired entries 0/1, waits for readiness, then configures SINF's face, hands, cloth, hair and reference-board state.
6. The original motion/camera/visibility sequence then runs. It changes HAWK's image between HAWK and KAKU, hides/shows SINF, changes map render flags, drives facial/hand motion, lighting and fog, and checks Start for skip.

`SHDR` and `SHD2` are **full scene packs**, not shadow-volume-only data. The latter description in the external investigation handoff was wrong.

## Located assets

Decoded IPAC directories and CHRS tokens identify these exact resources:

| Resource | Contents / role |
| --- | --- |
| `SHDR.PKF` | Textures and CHRT/CHRS definitions: HAWK image → `TAK02M7G.MT5`; KAKU image → `TAK02M8G.MT5`; character HAWK uses Object/TinyMot with collision and shadow disabled |
| `SHDR.PKS` | `MAP` and `MAP03` (MAPM scenery), `M_TORI` (MOTN bird motion), `SCROLL67` (SCR0), `TAK02M7G` and `TAK02M8G` (CHRM model geometry) |
| `OP02.AFS` entry 0 | Byte-identical to loose `SHD2.PKF`, 2,118,360 bytes |
| `OP02.AFS` entry 1 | Byte-identical to loose `SHD2.PKS`, 980,648 bytes |
| `SHD2.PKF` | Textures and CHRT/CHRS: SINF character uses `MGR_M.MT5`, face `MGR_F.MT5`, hands `SIN_TL.MT5` / `SIN_TR.MT5`; Human, Cloth, Osage and RefBoard options |
| `SHD2.PKS` | `MAP01` and `MAP02` (MAPM scenery), `MGR_M`, `MGR_F`, `MGR_FTBL`, `M_0605` (MOTN), `SCROLL53` (SCR1), `SIN_HM`, `SIN_TL`, `SIN_TR` |
| MAPINFO `SCEX` | Seven TRCK records, each with ASEQ, ACAM and AMOV; several also contain ASTR |
| MAPINFO `LGHT` / `ECAM` | Initial lighting / event camera data |

The SINF character definition and the script's face/hand/cloth commands identify the Shenhua side of the sequence. The pack directories establish where scenery geometry is stored; assigning individual MAP meshes to exact canyon shots still needs visual verification.

The SCEX records begin at MAPINFO offsets `0x2894`, `0x2ed4`, `0x423c`, `0x4944`, `0x5278`, `0x5d44`, `0x6324`. Their ASEQ frame counts are respectively 450, 350, 147, 580, 530, 57, 243. The script plays tracks in order **0 → 1 → 2 → 3 → 6 → 4 → 5**. Preserve this order rather than sorting by physical storage.

Normal completion requests Display module with scene 1 / OP02 / entry 0. The Start skip helper tests input `0x800`, fades, then requests Display entry 49. These return paths differ: the normal phase-6 return advances to the credit logos; entry 49 restarts the title flow.

## Existing code to reuse

- `src/s1/task/scen_objects.cpp`: `SCN3_Process`, `SCEN_LoadCurrentMapScript`.
- `src/s1/task/scen_vm.cpp`: `Startup_EventBase` and the existing VM.
- `tools/scn3/sh4/lift.cpp`: the existing SH-4 → VM lifter.
- `src/s1/task/scen_commands_system.cpp`: `Script_EV_PAK_AFS_Utils`, `Script_shenSeqUtils`, `Script_SetDynamicMapTransform`, `Script_CreateNPCImageModule`.
- `src/s1/task/pack.cpp`: `PACK_QueueFile`, `PACK_ReadAFS`, `PKF_Reader`, dynamic-map registration and `ParseCHID` after PAKS completion.
- `src/s1/chrd.cpp`: packed CHRS image and actor definitions.
- `src/s1/task/scenario_sequence.cpp`: `ASEQ_InitCameraTrackPointers`, `SEQ_ParseInternal`, `SEQ_ParseFrame`, animation/camera execution.

The immediate missing connection is still scenario loading. `SCEN_LoadCurrentMapScript` requests the separate PC `scenario/act01_OP02.scn`; the USA disc supplies native SH-4 SCN3 embedded at MAPINFO offset 8, size 10,252, flags `0x20000`. The existing lifter produces a 4,988-byte VM file. Connecting that converted original script to the existing VM should trigger the real pack loads and hide the placeholder. The PC script file is absent; simply submitting CHRD meshes cannot recover this sequence.

After that connection, validate PACK loading, script pointer/stack semantics, camera/motion execution and dynamic-map drawing on PSP. These have not yet been demonstrated by the renderer bringup. Keep the PSP GU adaptation below this original presentation and controller logic.

## Reproduce the audit

```
python3 tools/dc/intro_scene.py USA/DISC1/extracted --output build/dc-menu-audit/intro-usa
```

This verifies the USA executable profile and boot handoffs, inventories MAPINFO and both pack pairs, decodes character definitions, and checks the AFS entries against loose SHD2 files. It only reads disc assets.

With the handoff's prepared compiler, also reproduce disassembly, lifted VM and eight single-function C exports:

```
python3 tools/dc/intro_scene.py USA/DISC1/extracted \
  --output build/dc-menu-audit/intro-usa \
  --scn3cc /tmp/opencode/scn3build/scn3cc
```

Generated evidence is local under the build directory. No recovered game assets or script bytes are embedded in the source changes.

The [PSP scenario VM investigation](scenario-vm-psp.md) documents the execution contract, measured 32-bit layout problems and focused validation needed before connecting this script.
