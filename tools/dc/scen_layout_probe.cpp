// Compile-time layout probe for the 32-bit PSP scenario VM context.
//
// psp_abi_compat.hpp force-nulls `static_assert`; restore the real keyword so
// these checks actually execute during the PSP build.
//
// psp-g++ -std=c++20 -DSHENMUE_TARGET_S1_PSP=1 -DSHENMUE_HOST_PSP=1 \
//   -Isrc -Ithird_party/nuklear -Ilibs/ShenmueDK/include \
//   -include src/psp/psp_abi_compat.hpp -fsyntax-only tools/dc/scen_layout_probe.cpp
#include "s1/task/scen.hpp"

#undef static_assert

using namespace shenmue::s1;

static_assert(sizeof(void *) == 4, "PSP layout probe must target a 32-bit host");

// Fixed-width script state is host-independent.
static_assert(offsetof(ScenarioStack, accumulator) == 0x00);
static_assert(offsetof(ScenarioStack, status) == 0x04);
static_assert(offsetof(ScenarioStack, asyncFunctionIndex) == 0x06);
static_assert(offsetof(ScenarioStack, previousOperand) == 0x08);
static_assert(offsetof(ScenarioStack, trailingFlags) == 0x0a);
static_assert(offsetof(ScenarioStack, asyncArguments) == 0x0c);
static_assert(offsetof(ScenarioStack, savedWords) == 0x0c + 4 * kScenarioAsyncArgumentWords);

// Native pointers follow the fixed block; on a 4-byte-pointer host they are
// tightly packed.
static_assert(offsetof(ScenarioStack, instruction) == 0x42c);
static_assert(offsetof(ScenarioStack, stackPointer) == 0x430);
static_assert(offsetof(ScenarioStack, framePointer) == 0x434);
static_assert(offsetof(ScenarioStack, nextStack) == 0x438);
static_assert(sizeof(ScenarioStack) == 0x43c);

// The scenario window keeps its established 32-bit layout.
static_assert(offsetof(ScenarioWindow, activeInfo) == 0x18);
static_assert(offsetof(ScenarioWindow, dataSegment) == 0x34);
static_assert(offsetof(ScenarioWindow, workspace) == 0x8c);
static_assert(sizeof(ScenarioWindow) == 0x260c);

// The explicit VM stack top must stay inside the scenario window on the PSP.
static_assert(offsetof(ScenarioWindow, workspace) + kScenarioVmStackRestoreEndOffset + 4 <= sizeof(ScenarioWindow),
    "task VM stack top overruns ScenarioWindow");
