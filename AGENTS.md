# Shenmue PSP

A port of Shenmue I to PSP based on LemonHaze420's reimplementation. Ignore code related to Shenmue II.

The goal is a full speed 30FPS port of Shenmue, running at 480x272 with a native GU renderer, support for both 16:9 and 4:3, and the game playable start to finish.

## Docs

Refer to `docs/optimisation.md` and https://themriron2.github.io/psp-survival-guide/optimisation.html for optimising the code. It contains useful information about how the PSP hardware should be used and should be consulted regularly.

During development, assume expanded 64MB RAM for PSP-2000 onwards. A final release should eventually aim to be 32MB compatible but a 1.0 release for 64MB is fine.

Keep comments concise. Don't create fake handmade bridges where the original code can be used.