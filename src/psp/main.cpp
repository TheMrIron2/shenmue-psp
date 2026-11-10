#include "s1/boot.hpp"
#include "s1/texture_loader.hpp"
#include "psp/boot_log.hpp"

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspthreadman.h>

// NOTE: the pspdev C++ PSP_MODULE_INFO macro stringizes its first argument
// (`#name`), so pass it *unquoted* to get the module name "Shenmue" rather than
// "\"Shenmue\"".
PSP_MODULE_INFO(Shenmue, 0, 1, 0);
// The engine uses the VFPU (floating point) heavily and relies on a large heap
// for the D3T scratch buffer and scene memory.
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
// The recovered init path stacks deep frames (InitMain -> InitSpriteBank ->
// PACK_Drain -> PACK_Callback); 256 KB was not enough on hardware.
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);
// Negative = "all remaining memory minus |n| KB" (reserve the stack + slack),
// so the heap adapts to whatever partition the EBOOT is given.
PSP_HEAP_SIZE_KB(-2048);

extern "C" int main(int argc, char **argv) {
    shenmue::psp::InitBootLog(argc, argv);
    shenmue::psp::ResetBootLog();
    shenmue::psp::BootLog("entry: build=%s %s", __DATE__, __TIME__);
    shenmue::psp::BootLog("entry: argv0=%s", argc > 0 && argv && argv[0] ? argv[0] : "<missing>");
    // The recovered input pipeline polls the active controller once per game
    // frame. Ask the PSP SDK for the analog nub in addition to digital buttons.
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    if (!shenmue::s1::InitTextureLoader()) {
        shenmue::psp::BootLog("fatal: PVR decode scratch initialization failed");
        return 1;
    }

    shenmue::psp::BootLog("entry: Shenmue PSP argc=%d", argc);
    shenmue::psp::BootLogHeap("entry before Shenmue_Main");
    // Enter the recovered Shenmue boot sequence unmodified. `Shenmue_Main`
    // performs `Initialization` -> `Initialization_2` -> `ShenmueInit` and then
    // runs the HLib cooperative task loop forever.
    const int result = shenmue::s1::Shenmue_Main(argc, argv);
    shenmue::psp::BootLog("fatal: Shenmue_Main returned %d", result);
    return result;
}
