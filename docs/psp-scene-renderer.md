# OP02 native scene bringup

The USA idle boot path now reaches original MT5 character submission and native GU textured triangles. This is a first geometry milestone, not a completed attract sequence. The rendered Ryo body is a base-map player placeholder which the original script hides. The actual OP02 intro features the bird and Shenhua; see [the recovered intro trace](original-intro-scene.md).

## What runs

`CharDraw_DRWP` → `Character_SubmitForDrawing` → original figure/node traversal → `DrawSceneObjectFromCommandStream` → original material/strip parser → `psp::QueueSceneTriangles`.

The backend consumes the existing expanded triangle buffers, original node/view transforms, camera projection, texture surfaces and material opacity. GE performs transformation and clipping. Opaque draws write depth; transparent draws use alpha blending without depth writes. The frame queue holds at most 32,768 vertices and 512 draws. Textures use retained swizzled native 16-bit storage. Original Dreamcast lighting, fog and environment mapping are not implemented in this backend yet.

The frame context retains engine state bookkeeping without requiring PC HLSL shaders or modern scene passes. Scene rendering precedes the existing frontend quad queue.

Vertex buffer copies previously used the almost full C++ heap. Failed allocations returned handles anyway, leaving only one 24-vertex batch drawable. PSP vertex allocations now use HLib where available, retain allocator ownership for release, and fall back to the C++ heap during early bootstrap. Failed retained-buffer allocation returns zero. The resulting base-map Ryo body submits 41 batches / 5,379 vertices.

## Data and reproduction

PPSSPP now searches `host0:/USA/DISC1/extracted` before the legacy extracted-tree path. `tools/dc/extract.py` accepts the supplied split-file MODE1/2352 CUE layout as well as GDI. It derives absolute LBAs from sector headers and skips track pregaps. The USA extraction contains 2,343 files; its `1ST_READ.BIN` SHA256 is `ca98879a97df87b668836bbfa597d38c6f9efc3fd45160208b8d5d866325f32c`.

Build:

```
make -j4 -f Makefile.psp DC_ASSETS=1 BUILD_PRX=1
```

Verified in PPSSPP 1.20.4 with Vulkan and `IgnoreBadMemAccess=False`, following title idle → SEGA → AM2 → Dreamcast → OP02. The textured body is visible against black, stationary, facing away, with no head. This does not validate the attract camera or animation. Capture: `build/psp/op02-window.png`; isolated run log: `build/psp/op02-first-frame.log`.

Host checks:

```
g++ -std=c++20 -O2 -Wall -Isrc tools/dc/scene_renderer_test.cpp -o build/dc-menu-audit/scene_renderer_test
build/dc-menu-audit/scene_renderer_test
build/dc-menu-audit/frontend_test PAL/DISC1/extracted/1ST_READ.BIN USA/DISC1/extracted/1ST_READ.BIN
```

The scene check covers vertex attributes, colour/opacity, reversed-depth conversion, matrix alignment and queue limits. The frontend regression passes.

## Next dependency: the original scenario

`SCN3_Process` currently calls `SCEN_LoadCurrentMapScript`, which requests the PC file `scenario/act01_OP02.scn`. The Dreamcast disc instead embeds native SH-4 SCN3 inside MAPINFO. Therefore the current rendered body is not evidence that the original OP02 scenario executes. Do not add a replacement sequence, camera, or completion timer.

The repository's existing `tools/scn3` lifter successfully converts the USA chunk to the existing VM representation. Verified commands with the handoff's prepared tool:

```
/tmp/opencode/scn3build/scn3cc build/psp/op02-native.scn --target pc -o build/psp/op02-vm.scn
```

The native chunk starts at MAPINFO offset 8, length 10,252; lifted output is 4,988 bytes. Wire this conversion into a reproducible disc preparation path and load the converted original scenario through the existing VM. Do not interpret the raw SH-4 bytes as VM opcodes. Validate VM pointer/stack handling on PSP before expecting motion, face setup or script-driven cameras.

USA/PAL OP02 MAPINFO are byte identical. `MAPR=-1` means no base map mesh is referenced; it does not rule out scenery loaded dynamically by the script. Base CHRD declares `YKB_M` and one Player character (AKIR). The original script hides this actor and loads the real scenery, HAWK and SINF from the SHDR/SHD2 scene packs. The earlier handoff's classification of these packs as shadow-only data was incorrect. The normal script completion requests Display entry 0, preserving title phase 6 so it advances to credits phase 7. Start skip tests input mask `0x800` and requests Display entry 49; these are distinct return paths.
