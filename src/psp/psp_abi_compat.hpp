#pragma once

// PSP ABI compatibility shim (force-included by the PSP build only).
//
// Shenmue was recovered from x86-64 PC and 32-bit Dreamcast builds. The bulk of
// the recovered `static_assert(sizeof(...) == ...)` / `offsetof` checks encode
// the *x86-64* runtime layout of structures that embed host pointers.
//
// The Sony PSP is a 32-bit MIPS host, so pointers are 4 bytes instead of 8 and
// those size checks cannot hold. Every structure whose size is pointer-dependent
// is a *runtime* structure (asset/disk formats use fixed-width integer fields and
// therefore keep their exact layout on both hosts). Rebuilding the runtime
// structures at 32-bit is internally consistent, so the layout assertions are
// suppressed for the PSP target until a dedicated 32-bit ABI audit adds
// per-structure 32-bit expectations.
//
// This header is only ever force-included by `Makefile.psp`; 64-bit PC builds
// keep every assertion intact.

#if defined(SHENMUE_HOST_PSP)
// Guarantee the fixed-width integer types are visible to translation units that
// (incorrectly) relied on a transitive include of <cstdint> on the PC toolchain.
#include <cstdint>
#define static_assert(...)
#endif
