# PSP: from the current EBOOT to the real Shenmue I menus

Repository assessment: 4 November 2026. Scope: Shenmue I, including the uncommitted PSP port. Current EBOOT/ELF SHA-256: `13FAF3EBE1442196018C90099042E16819D3B738AB53EB1EBD0F22BFF5DA17D6` / `D1692759CD3EAADEADAA123C55E6F9906F057E468DFEDF252067542CEED2F8C43`.

**Milestone status: the original boot path reaches the main menu and Options landing in Windows PPSSPP.** The latest controlled PPSSPP v1.20.4 run used the default JIT CPU core and Vulkan. `DISP_Module` completes, the Press Start callback opens `MENU.SPR` with all 23 surfaces, and the original four-item wheel accepts D-pad navigation. Down selects Options; the PSP bridge loads Dreamcast `TITLE/OPTION1.SPR`, moves its category selection from 0 to 1, and Circle returns to the main wheel. The main menu and Options landing use a centered 4:3 layout with Dreamcast sprite/font assets; they are native PSP GU presentations, not the unavailable PC `.ui` screens. The Options category detail screens, Load/save presentation and New Game route remain unfinished. The exact tested EBOOT is the hash above. [BOOT.LOG](../BOOT.LOG) is cumulative; the latest replay begins at line 117828 and records the main-menu entry around line 118497, Options landing around line 118637 and return to main-menu state 0 at line 118651. The PPSSPP process remains open on the main menu. This is debugger-injected PPSSPP evidence, not a physical PSP controls test.

The separate 90-second no-input capture enters map module 7 once, reads `MAPINFO.BIN` and completes `YKB_M.MT5`. PPSSPP remains alive until the harness closes it, with no captured strict memory fault or STCH denial. There is no later scene-progress checkpoint. This proves process survival through capture, not sustained scene execution. **Correction:** `BOOT.LOG` is cumulative; the previously reported repeated initialization/model loads belonged to separate launches. That capture's launch begins at `entry: Shenmue PSP` on line 67342 and contains one module-7 transition. See [PPSSPP-STRICT-SCENE-TRANSIENT.LOG](psp-bringup/logs/PPSSPP-STRICT-SCENE-TRANSIENT.LOG) and [BOOT.LOG](../BOOT.LOG).

## How close are we?

The main-menu milestone is reached through the original engine boot, title callback and menu state. The PSP bridge draws the recovered Dreamcast title/menu assets through GU in a 4:3 viewport with persistent redraw. D-pad navigation, Options entry, category selection and return are verified. The Options landing is present, but its category detail screens and the Load/save presentation are incomplete; New Game has not been followed into scene startup. Physical PSP boot, Memory Stick asset discovery, physical controls, 30 FPS timing and sustained 4:3 presentation remain unverified.

## Current review findings and priorities

| Priority | Finding and evidence | Required next action |
| --- | --- | --- |
| Reached | **The title, main menu and Options landing have PSP presentation bridges.** `RenderPspFrontendFrame` redraws them each display frame; `MENU.SPR` loads all 23 surfaces and `OPTION1.SPR` loads eight. The four-item menu uses a centered 4:3 viewport, and D-pad input changes selection. | Keep this route stable. The PC `.ui` renderer remains unavailable on PSP. See [ui_frontend.cpp](../src/s1/ui_frontend.cpp), [ui_assets.hpp](../src/s1/ui_assets.hpp), [psp_renderer.cpp](../src/psp/psp_renderer.cpp). |
| Next | **Only the Options landing and category selection are implemented.** D-pad Down changes its selected category and Circle returns to the main wheel. The Options `.ui` handle remains `-1`; confirming a category does not yet open its detail screen. | Add category detail screens if required for the next milestone. Audio output is excluded from this milestone. |
| Next | **Load/save and gameplay startup are not accepted.** No empty-save or valid-save path has been exercised from this menu. Earlier boot traces recorded a `cm_d000.bin` resize allocation failure, so reaching New Game's full scene route still needs a fresh current-build check. | Exercise no-save and valid-save flows, then trace New Game through scene startup and resolve any current allocation failure. |
| Next | **Real PSP deployment and controls remain unverified.** PPSSPP finds `host0:/DISC1/extracted`; the earlier real-PSP report was `dc assets: none found`. Controller verification uses debugger-injected PSP bits. | Confirm Memory Stick data-root discovery and physical input on PSP-2000+, then verify aspect modes and timing. |

## Are the recent changes making progress?

Yes. The observed Wind stride, LCDM bitmap and archive-table faults no longer stop the tested menu route. PSP `HLib::TaskData` now matches the allocator's 64-byte header assumptions, and the PSP HLib arena is 16 MiB for the authorized 64 MiB bring-up target. CHRD requests use owned storage; STCH survives module cleanup. Releasing `TITLE3.SPR` at the Press Start transition frees enough memory for all 23 `MENU.SPR` surfaces. The latest JIT/Vulkan replay verifies original boot, continuous menu drawing, D-pad selection, Options landing/category navigation and return to a live main menu. The PSP face-button mapping now keeps Cross as confirm and Circle as cancel through the legacy input translation.

The requested main-menu milestone is therefore achieved in PPSSPP. The broader port still needs these gates:

1. Implement Options category details and the Load/save screen, including no-save and cancel/return behavior.
2. Recheck current memory-sensitive startup and New Game scene loading; an earlier build logged a failed `cm_d000.bin` preload.
3. Confirm Memory Stick boot and physical controls on real PSP.
4. Verify 16:9/4:3 presentation and sustained timing; audio is outside this milestone.

The older crash screenshot at access `0x00055c24`, PC `0x089d395c`, remains an unpaired build report. Its address maps plausibly into `PruneMNRBReservations` under the previously determined load bias, but no current-build reproduction establishes its cause. Keep strict memory checking enabled and preserve EBOOT/ELF hashes with each capture.

## What “original boot” means here

The PSP entry point calls the recovered C++ engine. It does not execute Dreamcast BIOS code or the original SH-4 executable. Preserve this route rather than replacing it with a standalone menu demo:

```text
src/psp/main.cpp
  -> Shenmue_Main
     -> Initialization
        -> HLib_Malloc
        -> Initialization_2
           -> ShenmueInit
              -> threads, archive discovery, graphics/resource initialization
           -> HLib_Init and the remaining initialization, including fonts
     -> cooperative HLib task loop
        -> ROOT_Callback
        -> Shenmue_Init / InitMain
        -> Audio_Module
        -> Display (or SetToDisplay -> Display)
        -> DISP_Module and frontend tasks
        -> splash/title/Press Start/menu transitions
```

Sources: [entry point](../src/psp/main.cpp), [boot and module table](../src/s1/boot.cpp), [initialization](../src/s1/initialization.cpp), [second initialization stage](../src/s1/initialization_2.cpp), [module transitions](../src/s1/modules/modules.cpp), [display module](../src/s1/task/disp.cpp).

Keep direct-map launch and frontend automation disabled for acceptance testing. A debug shortcut can isolate a renderer fault, but does not prove original boot works.

## 1. Remove the immediate asset-discovery blocker

`dc assets: none found` is printed by [archive_data_dc.cpp](../src/common/archive_data_dc.cpp) when none of its candidate roots yields a nonempty parsed `manifest.txt`. It does not identify a particular missing title texture, and does not prove that initialization or the task loop subsequently completed.

The loader checks, in order:

1. The root supplied by initialization (normally `host0:/DISC1/extracted` for this build).
2. `SHEN_ROOT`, if set.
3. `host0:/DISC1/extracted`.
4. `ms0:/PSP/GAME/Shenmue/data`.
5. `ms0:/PSP/GAME/Shenmue`.

It does **not** recursively discover files. It does **not** mount a GDI or ISO at runtime, despite the broader wording in its introductory comment. An XMB launch should use the Memory Stick fallback; it should not depend on a PSPLink `host0:` mount or environment variable.

Extract a legally owned Shenmue I disc with the supplied tool, substituting the actual GDI path:

```sh
python3 tools/dc/extract.py /path/to/disc1.gdi -o /path/to/extracted-disc1
make -f Makefile.psp DC_ASSETS=1 BUILD_PRX=1
```

If changing build variants, force a fresh final link/package so an old EBOOT is not deployed. The default is `DC_ASSETS=0`; preserve the explicit flag. The observed warning already indicates that the running binary contains the DC backend.

Copy the **contents** of the completed extraction to:

```text
ms0:/PSP/GAME/Shenmue/
  EBOOT.PBP
  data/
    manifest.txt
    SPRITE/
      ...
    SCENE/
      ...
    ...all remaining extracted directories/files...
```

Use the exact hard-coded `Shenmue` install location initially. A renamed game folder is not automatically resolved from `argv[0]`. Supporting arbitrary install folders is a later small improvement.

The runtime manifest is plain text: decimal byte size, a tab, and a relative path. The extractor generates it automatically. Its optional `--manifest` JSON output is **not** the runtime manifest. The extractor writes `manifest.txt` before finishing extraction, and `--skip-existing` only checks existence, so a manifest alone cannot prove extraction completed or that files are intact.

Before copying, verify every listed path exists and matches its recorded size. Copy the entire extracted tree for initial diagnosis; reduce it to a boot-only subset only after tracing actual requests. Check for `SPRITE/TITLE4_A.GZ` in this disc/region and record any discrepancy rather than assuming all disc editions match.

The current checkout's `DISC1/extracted` passes that size check: 2,343 manifest entries, no missing files or size mismatches, and `SPRITE/TITLE4_A.GZ` is 473,132 bytes. Reading its gzip stream succeeds. The PPSSPP boot log confirms that the emulator can find the `host0:` root, so the immediate PPSSPP failure is after archive discovery. The previous real-PSP report still points to a separate Memory Stick deployment/path issue.

**Pass condition:** launch prints `dc assets: ms0:/PSP/GAME/Shenmue/data`; instrumentation confirms a nonzero entry count and an actual successful file read with the expected byte count. A root-found message alone is insufficient: manifest loading does not open each listed file.

## 2. Trace startup and make failures observable

Persistent boot logging is now in the PSP build. `BOOT.LOG` is appended beside the launched EBOOT path when available and also at `ms0:/PSP/GAME/Shenmue/BOOT.LOG`. It records PSP entry, the two initialization stages, archive root/entry count, whether the task loop begins, its first 16 iterations and an explicit message if a task callback leaves a null queue pointer. Record these additional details as tracing continues:

- First failed lookup/open/read.
- Requested logical path, resolved backing path, offset, requested size and returned size.
- `HLib_Init`, task-loop progress after the initial 16 iterations and each module transition. Recent logs add bounded callback begin/end traces for the first 40 task iterations.
- `DISP_Module`, title sprite decompression, texture count, UI-definition readiness and frontend state changes.
- Allocation failures and free/high-water memory at these boundaries.

Fail visibly on a missing manifest instead of proceeding into unrelated initialization with no data. Distinguish “not indexed”, “indexed but cannot open”, “short read”, “decompression failed”, and “unsupported format”. Avoid per-frame disk writes; retain a small ring buffer and flush at milestones or failure.

[Archive_QueueRead](../src/d3t/archive_handles.cpp) has a synchronous PSP/DC path, but this does not make the whole content system synchronous. [UI loading](../src/s1/ui_assets.cpp) still queues work through `PushShaderLoadToThreadQueue`; [threads.cpp](../src/d3t/threads.cpp) still creates the D3T worker group. Verify success **and failure** complete requests and release pending handles. If workers are replaced with cooperative processing, pump all required queues and deliver their completion callbacks; merely disabling workers can leave the splash waiting forever.

**Pass condition:** the log reaches and returns from title TEXN parsing and cycle-manager preload without an invalid memory access. PPSSPP now also records a submitted title frame. The no-input run now reaches `MAPINFO.BIN` and completes the first map model load; sustain execution beyond that boundary and check collision, scene setup and rendering.

Completed local PPSSPP traces establish these startup milestones:

- The archive backend finds `host0:/DISC1/extracted` with 2,343 manifest entries.
- `Initialization_2` completes. The PC HLSL/D3D shader stack is skipped for PSP.
- `InitMain` loads cold storage, motion data and all three font blocks. The first root frame reaches `PresentLatestFrame` and returns.
- The explicit silent-audio path completes `PauseGameAudioSequence()` without allocating host audio output, AICA RAM or loading `system1.snd`.
- `DISP_Module` reads `TITLE4_A.GZ` (473,132 indexed bytes), checks that `inflate()` returns `Z_STREAM_END`, verifies 3,858,860 output bytes, and parses all 33 `TEXN` chunks.
- `LoadTEXN` uses a statically allocated, 2 MiB decode scratch buffer protected by a PSP semaphore. The largest decoded title chunk observed is 2 MiB; a completed run decodes all 33 chunks and returns from the upload API calls without Newlib decoded-buffer allocation failures. The size guard rejects larger PVR textures until a broader memory strategy is added. For the title/menu path, decoded pixels are retained in D3T resources and the PSP frontend holds stable texture handles.
- `DISP_Module` releases the sprite, starts hotplug/bookkeeping, selects the Press Start state, returns from `SAVE_Init` and logs `DISP_Module: complete` with `state=1 splash=6 uiState=-1` and Press Start visible. The original Press Start callback queues eight retained textures; `PresentFrame` later logs a frame with `texturedQuads=8`. The splash UI definition and PNGs remain absent, so their visuals are bypassed.
- `UI_Elements_Init()` now runs even when PSP skips PC shader setup, which registers the shared Splash, Press Start and menu callbacks. The content queue's 32-slot limit was temporarily reached during the first burst; queue-full UI requests now retry, and failed file requests mark their slots for release instead of leaking them.
- The resulting content failures identify missing PC-style assets, for example `/ui/Splash/Splash.ui`, `/ui/mainmenu_New/MainMenu_PressStart.ui`, and `/ui/splash/textures/segalogo_01.png`. The same failure log names many other UI definitions, subtitle fonts and splash images. The frontend state can become visible in engine bookkeeping, but those assets do not load.

The supplied PPSSPP crash screen says `Invalid Memory Access`, `Access: Unknown at 00055c24 (sz: 0)`, and guest PC `089d395c`. PPSSPP's module-list allocation base is `0x08800000`, while the actual ELF load bias is `0x08804000`; subtracting the load bias maps the PC to ELF offset `0x0019595c`, inside `CycleManager::PruneMNRBReservations`. The preceding branch delay-slot instruction at `0x00195958` is `sw v1,60(a0)`, which writes the reservation-tail pointer field. If PPSSPP recorded the PC after that store faulted, address `0x00055c24` is consistent with `a0=0x00055be8`. That is a plausible bad MNRB entry pointer. It remains a lead because the screenshot has no paired EBOOT/ELF hash, PPSSPP reports an unknown access size, and its host JIT mnemonic `mov [rbx+r8-0x1], sil` is translated host code rather than the MIPS store.

A strict D3D11 replay of an earlier build used EBOOT/ELF hashes `90A5FC97E0FA23D2D1E89BA75A4B131A0D43AAE7E1BEAC10FD946BB1CF8C4AD6` / `5F62695178C21131E0C1792C38B4D4452C2C333F8F9587C048A3B429E01DB96A`, PPSSPP v1.20.4 JIT, and `IgnoreBadMemAccess=False`. It stopped at guest address `0x00000002`, PC `0x089dade4`, before `DISP_Module: complete`. The exact ELF for this historical hash is not present, so do not infer a source function from that PC. PPSSPP's log reports a word read and prints host JIT disassembly. Its log is [PPSSPP-STRICT-D3D11-MENU.LOG](psp-bringup/logs/PPSSPP-STRICT-D3D11-MENU.LOG).

A strict OpenGL replay used an instrumented EBOOT/ELF pair `76909D2B3CC5D1228B2BB492B8E46548407630E95D0119FB8041A0ABDAFFF3C9` / `C0446CCB1934B67F062BD82B2321FA5C9876B26340D08A713C650AB0F73F43AE`, PPSSPP v1.20.4 JIT, and `IgnoreBadMemAccess=False`. It stopped at `0x00000002`, PC `0x089db0fc`, before `DISP_Module: complete`; PPSSPP reported `movzx esi, byte [rbx+r8]`. This historical ELF is not present in the workspace, so no source-level access or pointer is established by this run. See [PPSSPP-MNRB-OPENGL-STRICT.LOG](psp-bringup/logs/PPSSPP-MNRB-OPENGL-STRICT.LOG). Later register tracing identified the current access-at-`0x2` as a null LCDM bitmap source, which the current guard handles. Earlier permissive runs reached menu and Options callbacks while ignoring bad accesses, so they are not a clean-memory result.

Separately, a fresh instrumented PPSSPP run reproduced an invalid access in `WIND_Callback_0`: the log showed Wind entry 0 at `0x95a0138` with flags zero, then entry 1 at `0x95a01c0` with bogus flags `0x7fff7fff`, and stopped. On PSP, `sizeof(WindEntry)` is `0x68` because its pointers are 32-bit; the callback had stepped by the PC build's hard-coded `0x88`. Replacing byte arithmetic with iteration over `WindTaskData::entries` fixed that reproduced fault. The follow-up log reports all five entries at the correct `0x68` spacing with zero flags, `WIND_Callback_0: complete`, later display callbacks, and a GU frame with eight textured quads. This resolves the reproduced Wind crash but does not prove that it was the exact crash shown in the screenshot. Preserve each matching ELF and log together for future address-level diagnosis.

Independently, the earlier 4 MiB-scratch build failed inside `std::vector::resize(629716)` for `cm_d000.bin`, with only 46,920 bytes free in Newlib's existing arena. A historical 2 MiB-scratch build grew the arena by 630,784 bytes, read the file and loaded 157 character entries. The current EBOOT fails this same 629,716-byte preload again and continues with zero entries, so that earlier success does not close the present memory-budget gap.

The AICA failure is no longer the active blocker because the PSP build uses an explicit silent-audio bypass. Real audio remains unimplemented for this bring-up path. Runtime heap readings show a very small Newlib heap after early startup, so continue tracking allocations at each boundary; static ELF section sizes do not establish the available runtime heap.

## 3. Resolve the frontend asset contract before building its renderer

This is a substantial compatibility issue. [shenmue.h](../src/shenmue.h) retains PC data semantics for the PSP target. `SHENMUE_USE_DC_ASSETS` changes the archive backend; it does not turn the recovered PC frontend into the Dreamcast frontend.

The current code requests at least these presentation resources:

| Resource | Consumer |
| --- | --- |
| `sprite/TITLE4_A.GZ` | `DISP_Module` -> gzip loader -> TEXN/PVR texture loading |
| `/ui/Splash/Splash.ui` and splash PNGs | Original PC splash path; unavailable in the Dreamcast extraction |
| `title/TITLE3.SPR` | PSP Press Start presentation in `LoadPressStart`/`PressStartRender` |
| `title/MENU.SPR` | PSP main-menu presentation and native wheel in `LoadMainMenu`/`LoadMainMenuWheel` |
| `/ui/mainmenu_New/*.ui` | Original PC frontend path; bypassed by the PSP title/menu presentation bridge |

Sources: [frontend](../src/s1/ui_frontend.cpp), [UI asset queue](../src/s1/ui_assets.cpp), [gzip loader](../src/s1/gzip.cpp). Fonts, localization, images and submenu definitions add further dependencies; this is a starting inventory, not a complete boot manifest.

The DC loader normalizes case, slashes and `.MT3` to `.MT5`. It neither synthesizes `.ui` files nor converts their referenced assets. The PPSSPP content loader now confirms `Local file not found` for the requested UI definitions, including Splash, Press Start, Main Menu, Load Screen and submenu/HUD files, and for the three splash PNGs. This validates the mismatch against the mounted extraction; adding aliases or arbitrary placeholder files would not provide valid UI definitions or their referenced images.

The PSP presentation bridge uses `TITLE3.SPR` and `MENU.SPR`, queues retained pixels through GU, bypasses the unavailable PC splash visuals and keeps the original Press Start/menu callbacks. PPSSPP logs eight title quads reaching `PresentFrame`. The current strict controlled-input run logged Circle reaching `MainMenuWheelRender`, loaded 23 menu surfaces, submitted nine textured quads, changed the wheel selection and invoked the Options transition. The PSP draw calls are currently in activation callbacks and are not resubmitted each frame; fix this lifecycle before treating those submission checkpoints as a persistent visible menu. Windows key injection did not produce PSP controller samples. The bridge currently draws a simple title and a three/four-item main-menu layout; it does not establish every original animation, language treatment, or submenu presentation.

An alternative is to deliberately support the existing PC frontend with an appropriate PC asset source and any needed conversion. That is a separate data contract: document it explicitly rather than assuming a DC extraction can satisfy it. Merely renaming or placing arbitrary files at the requested `.ui` paths will not work.

**Pass condition:** the latest EBOOT displays the Shenmue title from `TITLE3.SPR`; Start opens a readable `MENU.SPR` wheel; the configured confirm control and D-pad invoke the original game callbacks; each missing submenu asset is either provided or has a supported Dreamcast presentation path. A logged callback or a custom debug selector alone does not meet this milestone.

## 4. Validate the boot-critical 32-bit runtime

[src/psp/psp_abi_compat.hpp](../src/psp/psp_abi_compat.hpp) currently defines `static_assert(...)` away for the whole PSP build. Successful linking therefore does not establish correct structure sizes, offsets or disk parsing. Its comment about runtime layouts is not a substitute for auditing raw offset accesses.

Audit the startup path first: HLib task headers and allocator bookkeeping, module/task payloads, archive/content descriptors and callbacks, UI structures, TEXN/PVR headers and texture upload descriptions. Restore fixed-format assertions and add explicit PSP expectations for pointer-dependent structures. Replace assumptions that a pointer occupies eight bytes wherever the same storage is accessed using hard-coded offsets. Check packed/unaligned accesses on MIPS and callback signatures as well as structure sizes.

Use the permitted 64 MB PSP-2000+ configuration initially. The build requests expanded memory and the entry point reserves 1 MB from the remaining heap. Existing reductions include a 12 MB HLib arena, 1 MB malloc arena, 2.5 MB global allocation multiplier base at 1x, a 1 MB large-data buffer, and a 2 MiB static PVR decode scratch buffer. Reducing the scratch from 4 MiB to 2 MiB let the Newlib arena grow enough to load `cm_d000.bin`; textures larger than the 2 MiB scratch are rejected for now. These selected capacities are not proof that all scenes fit; executable/BSS, stacks, workers, asset indexing and textures also consume memory.

Add guards to decompression and texture loading before trusting asset sizes or headers. In particular, validate the gzip trailer size and inflate result, TEXN chunk bounds, pixel formats and allocation results. Replace unreachable assumptions for unsupported input with useful failures. Track peak compressed + decompressed + decoded + uploaded texture memory, and establish texture ownership before freeing upload sources.

**Pass condition:** boot and repeated title/menu transitions run with valid layouts, bounded allocations and no growing memory use. Reserve 32 MB compatibility work for a later measured pass.

## 5. Implement the minimum native GU frontend renderer

[psp_renderer.cpp](../src/psp/psp_renderer.cpp) initializes a 480x272 GU display using 512-pixel-wide 8888 framebuffers, a depth buffer and an aligned display list. `PresentFrame` clears, submits a bounded queue of textured quads, synchronizes, waits for vblank and swaps buffers. The shared D3T resource retains decoded PSP-readable pixels, and each sprite records a stable texture handle. The latest PPSSPP log confirms valid EDRAM bounds, non-null title texture pointers and a presented frame with eight textured quads. GU offset zero is accepted as valid, and title/menu lookups no longer depend on the PVR base that changes during boot. The debug text console remains disabled so it cannot overwrite the GU framebuffer.

Implement in this order:

1. **Implemented and logged:** PSP GU initialization, framebuffers, clear/sync/swap and fixed textured-quad queue. The run logs `edram=00200000` with offsets `00000000,00088000,00110000`; zero is the valid first framebuffer offset.
2. **Implemented and logged for the title:** retained 32-bit TEXN pixels, stable D3T handles, eight non-null title queue lookups and a presented frame reporting `texturedQuads=8`, without skipped-texture warnings.
3. **Visible in PPSSPP:** title and menu sprites use the existing [PVR decoder](../src/s1/pvr_decoder.cpp) and [texture loader](../src/s1/texture_loader.cpp), with textures wider than 512 downscaled. Removing the PSP-only row flip corrected the menu lettering in the observed capture. Hardware orientation, alpha, filtering and cache behavior remain to verify.
4. **Main menu reached, persistent and navigable in PPSSPP:** normal EBOOT Circle reaches the original Press Start callback, loads all 23 `MENU.SPR` surfaces and redraws the menu every frame. D-pad Right changes selection from 0 to 3. The current screen geometry is a simplified PSP bridge; Options/Load page content, clipping, language variants, original animations, help text and fades remain incomplete.
5. Extend beyond this 2D slice only as menu transitions require. For the existing PC UI route, inspect [ui_render.cpp](../src/d3t/ui_render.cpp) and [ui_renderer.cpp](../src/d3t/ui_renderer.cpp), including Nuklear conversion and indexed draws. For legacy sprites/fades, inspect [legacy_renderer.cpp](../src/s1/legacy_renderer.cpp) and [render_passes.cpp](../src/s1/render_passes.cpp). Windows-only draw sites still have no equivalent PSP submission.

Start with the 2D subset actually required by the frontend. Add any verified animated/3D title background dependency afterward; do not wait for the complete world renderer to test a menu. Temporary omission of a background is a diagnostic milestone, not a claim of faithful final presentation.

Define one coordinate transform from original UI coordinates to 480x272. Support full-width 16:9 and a centered 4:3 presentation, applying the same transform to geometry and clipping. Test fades and text at both aspect settings.

Design aligned GU buffers, explicit CPU-cache writeback before GPU reads, safe buffer reuse, and a measured VRAM budget. Optimize texture format/swizzling only after correctness and profiling. The repository-requested `docs/optimisation.md` is absent in this checkout. The referenced [PSP optimisation guide](https://themriron2.github.io/psp-survival-guide/optimisation.html) was inaccessible through the browsing tool during this review; consult it during implementation rather than treating this assessment as a verified summary of that guide.

**PPSSPP milestone pass:** real title/menu assets appear through the normal display task with upright, readable text; the main menu redraws over time, accepts navigation input and stays alive under strict interpreter checks. This pass is met by the current capture and log. **Hardware/release pass:** repeat on PSP with Memory Stick assets, physical controls, correct aspect modes, stable textures and measured timing; this remains open.

## 6. Add controller input and verify time progression

[input.cpp](../src/s1/input.cpp) has controller normalization, button-edge processing and menu consumers. PSP sampling maps D-pad directions, Cross/confirm, Circle/cancel, shoulders, Start/Select and the analog nub into the shared input state. PSP builds now set `g_isInputEnabled` at startup because no Windows focus event can enable it. A debugger-sent Circle event reached the original Press Start callback and opened the menu on the normal EBOOT. D-pad Right reaches polling as `0x8000`; after the initial item animation gate, the wheel selection changes from 0 to 3. Circle activates Options. Physical PSP controls remain unverified.

Map D-pad, Start, confirm/cancel, face buttons, shoulders and analog nub deliberately. Do not copy raw PSP button masks into the engine: its bit meanings differ. Set active-controller state, normalize the nub around its center with a dead zone, and ensure the Steam fallback does not overwrite the PSP sample. Confirm held, pressed, released and repeat behavior separately.

Check [timing.cpp](../src/s1/timing.cpp), [frame_pipeline.cpp](../src/s1/frame_pipeline.cpp) and the platform clock implementation on hardware. One vblank wait is not evidence of a 30 Hz game tick. Establish the intended 30 FPS update/presentation cadence without doubling an existing limiter; verify splash delays, fades and button repeat against elapsed time.

**Pass condition:** Start opens the normal menu; D-pad selection, confirm, cancel and return-to-title work repeatedly. Holding a button must not trigger multiple unintended transitions.

## 7. Finish menu dependencies and acceptance testing

Exercise empty-save and existing-save paths, bookkeeping completion, Options entry/exit and persistence. [task/hplg.cpp](../src/s1/task/hplg.cpp), [task/save.cpp](../src/s1/task/save.cpp) and [save_format.cpp](../src/s1/save_format.cpp) participate in those flows. Do not fake a successful save selection to get past a stall; no-save should be a valid completed result.

Menu audio can be a separate milestone, but audio initialization or completion must not block boot. For faithful presentation, add the required output and test it; use a clearly logged silent mode only while bringing up graphics/input. Reaching New Game's map transition is a useful boundary test, not proof that the opening or gameplay works.

Capture these hardware results, with build flags, PSP model, disc region, last boot checkpoint and memory figures:

| Test | Required result |
| --- | --- |
| Cold XMB boot with complete data | Normal boot reaches splash/title and Press Start |
| Missing/empty manifest | Clear actionable error; no hang |
| Missing or truncated required asset | Named failure and completed/aborted request; no infinite wait |
| Start and main menu navigation | Real frontend responds to physical controls |
| Options, Load with no saves, cancel | Correct screen/state transitions and return |
| Repeated menu/title cycles | No leaked handles, texture corruption or memory growth |
| 16:9 and 4:3 | Correct layout, text, clipping and fades |
| Several minutes idle | Correct timing and any attract-mode transition handled or explicitly recorded as unfinished |

The current implementation sequence is: **install/validate assets -> instrument normal boot -> decode/title/menu sprite bridge -> verify GU textures and title frame -> verify original menu controls -> add submenu presentations and validate save flows -> measure timing and physical PSP behavior**. Advance each stage using its pass condition. This keeps the original boot route as the integration test throughout.

## Review limits

The current EBOOT was built with `make -j2 -f Makefile.psp DC_ASSETS=1 BUILD_PRX=1` and launched from the repository directory in Windows PPSSPP v1.20.4. The latest interaction run used `--graphics=vulkan` and PPSSPP's default JIT CPU core. It started from the exact EBOOT hash listed at the top. The replay appended to cumulative `BOOT.LOG`; no screenshot was taken for this run because the verification was log-driven.

The callback trace then exposed and fixed the 32-bit Wind-entry stride error described above. The corrected run passed all five Wind entries, continued through `DISP_Callback`, and submitted the textured title frame. The no-input replay now advances through the Press Start idle transition, enters map module 7, reads `MAPINFO.BIN`, completes `YKB_M.MT5`, and remains alive without a strict memory-fault marker through its capture window. The extended capture lasts 90 seconds, but its last engine checkpoint remains the first model load; further guest progress is unproven.

PSP input is enabled during PSP startup. Windows keyboard-injection attempts did not produce nonzero samples in `sceCtrlPeekBufferPositive`, so this replay used PPSSPP's remote debugger to send controller bits directly to the emulated PSP. On the normal EBOOT (`PSP_TEST_AUTO_CONFIRM=0`), Start skipped the 3D intro, Cross reached the original `PressStartInput` callback, and `MainMenuWheelRender` loaded all 23 `MENU.SPR` surfaces. D-pad Right/Down/Up changed the wheel selection. Cross opened Options; Down changed its category from 0 to 1; Circle returned to `MainMenuStat=0` with the main wheel visible. This confirms those callbacks and bridge interactions through original boot, without the test-only auto-confirm hook. The PSP face-button translation was corrected so Cross yields the engine's `0x0400` confirm bit and Circle yields `0x0200` cancel.

The latest interaction replay used EBOOT/ELF `13FAF3EBE1442196018C90099042E16819D3B738AB53EB1EBD0F22BFF5DA17D6` / `D1692759CD3EAADEADAA123C55E6F9906F057E468DFEDF252067542CEED2F8C43`. The latest run begins at `DISP_Module` on line 117828, enters `MainMenuWheelRender` at line 118497, loads `MENU.SPR` at line 118583, loads `OPTION1.SPR` at line 118638, changes the Options category at line 118640, and returns through `MainMenuStat` at line 118651. Subsequent redraws show `main=1 wheel=1`; PPSSPP remains alive on the menu. The custom Options landing draws Dreamcast assets despite the missing `.ui` handle. Category-specific detail screens are not implemented. This is verified PPSSPP behavior only: debugger input is not a physical-control test, and the older `0x00055c24` crash report remains unpaired to the current build. The separate 90-second no-input run reaches map module 7 once, reads `MAPINFO.BIN`, and completes one `YKB_M.MT5` mesh load; this does not establish gameplay readiness. `BOOT.LOG` is cumulative, so use exact hashes when comparing evidence.

The PPSSPP content log still records unavailable PC `.ui` definitions and splash PNGs; the PSP bridge covers the title, main menu and Options landing. Options category detail screens and Load/save presentation/flows remain incomplete. The real PSP still needs Memory Stick asset-root verification. Physical controls and sustained 30 FPS remain unverified. The current 4:3 viewport is the bring-up target; 16:9 and long-run timing still need verification. Audio is intentionally not part of this milestone.
