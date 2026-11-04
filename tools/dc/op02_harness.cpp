// OP02 execution-contract harness.
//
// Loads the lifted USA OP02 VM module, validates/applies relocations, and runs
// it through the real scen_vm.cpp with a recording command table. Engine
// services are simulated: pack/AFS polling reports ready, shenSeqUtils advances
// a frame timeline using the SCEX track frame counts, GetInputData returns a
// configurable mask, and every command is recorded with frame, context,
// module-relative instruction offset, index, argument words and result.
//
// Build:
//   g++ -std=gnu++20 -O2 -ffunction-sections -fdata-sections \
//     -Isrc -Ithird_party/nuklear -Ilibs/ShenmueDK/include \
//     -DSHENMUE_TARGET_S1_PC=1 tools/dc/op02_harness.cpp src/s1/task/scen_vm.cpp \
//     -Wl,--gc-sections -o build/dc-menu-audit/op02_harness
// Run:
//   build/dc-menu-audit/op02_harness build/dc-menu-audit/intro-usa-repro/op02-vm.scn
// Writes build/dc-menu-audit/op02-harness-{normal,skip}.tsv.

#include "s1/task/scen.hpp"
#include "s1/hlib.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(SHENMUE_HOST_PSP)
#include <pspkernel.h>
#include <pspthreadman.h>
// The pspdev C++ macro stringizes its first argument; pass it unquoted.
PSP_MODULE_INFO(Op02Harness, 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(-1024);
#endif

using namespace shenmue;
using namespace shenmue::s1;

namespace {

struct CommandRecord {
    int frame = 0;
    std::uint32_t context = 0;
    std::uint32_t instruction = 0; // module-relative
    std::uint32_t command = 0;
    std::uint32_t arguments[8]{};
    std::uint32_t result = 0;
};

constexpr std::size_t kArenaBytes = 8 * 1024 * 1024;
std::vector<std::byte> g_arena;
std::size_t g_bump = 0;
std::vector<CommandRecord> g_records;
int g_frame = 0;
std::uint32_t g_moduleBase = 0;
std::uint32_t g_currentInstruction = 0;
std::uint32_t g_inputMask = 0;
bool g_done = false;
bool g_skip = false;
bool g_sequenceActive = false;
std::uint32_t g_sequenceRemaining = 0;
std::uint32_t g_fakeAfsOwner = 0;
// ASEQ frame counts for tracks 0..6 from the MAPINFO SCEX records.
constexpr std::uint32_t kTrackFrames[7] = {450, 350, 147, 580, 530, 57, 243};

std::uint32_t ReadWord(const std::uint32_t *words, std::size_t index) {
    return words ? words[index] : 0;
}

void HarnessCallHook(ScenarioContext *, const std::byte *instruction, std::uint32_t, bool) {
    g_currentInstruction = HLib_GetTaskDataOffset(instruction) - g_moduleBase;
}

const char *ModuleString(std::uint32_t offset) {
    if (!offset || !g_pCurrentTaskData)
        return "<null>";
    return reinterpret_cast<const char *>(g_pCurrentTaskData + offset);
}

const Vector3 *ModuleVector3(std::uint32_t offset) {
    if (!offset || !g_pCurrentTaskData)
        return nullptr;
    return reinterpret_cast<const Vector3 *>(g_pCurrentTaskData + offset);
}

void Record(ScenarioContext *context, std::uint32_t command, const std::uint32_t *words, std::uint32_t result) {
    CommandRecord record;
    record.frame = g_frame;
    record.context = HLib_GetTaskDataOffset(context->stack);
    record.instruction = g_currentInstruction;
    record.command = command;
    for (std::size_t index = 0; index < std::size(record.arguments); ++index)
        record.arguments[index] = ReadWord(words, index);
    record.result = result;
    g_records.push_back(record);
}

} // namespace

namespace shenmue::s1 {

extern std::uint32_t g_tmpScnfReturnValue;

std::byte *g_pCurrentTaskData = nullptr;
ScenarioHeader *g_pSCN_INFO_TaskData = nullptr;

void *HLib_AllocHeapMemBlock(std::int64_t size, std::int32_t) {
    const std::size_t bytes = static_cast<std::size_t>((size + 15) & ~static_cast<std::int64_t>(15));
    if (!g_pCurrentTaskData || g_bump + bytes > g_arena.size())
        return nullptr;
    void *const block = g_arena.data() + g_bump;
    g_bump += bytes;
    std::memset(block, 0, bytes);
    return block;
}

std::uint64_t HLib_FreeTaskData(HLib::TaskData *, std::int32_t) {
    return 1; // bump arena: contexts are unlinked but not reused
}

std::uint32_t HLib_GetTaskDataOffset(const void *pointer) {
    if (!pointer || !g_pCurrentTaskData)
        return 0;
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer) - reinterpret_cast<std::uintptr_t>(g_pCurrentTaskData));
}

namespace {

void DummyCommand(ScenarioContext *, void *) {
}

std::uint32_t LegacyStub(ScenarioRegisters *) {
    return 0;
}

std::uint32_t GeneralPurposeStub(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
    return 0;
}

// SCNF slots 0/1 are SCNF_WaitFrames; every other slot completes immediately
// (OP02 uses no other async primitive).
std::int64_t HarnessWaitFrames(ScenarioContext *, void *params) {
    auto &frames = *static_cast<std::uint32_t *>(params);
    const std::uint32_t previous = frames--;
    return previous ? 0 : 0x10000;
}

std::int64_t HarnessScnfComplete(ScenarioContext *context, void *) {
    if (context && context->stack)
        context->stack->accumulator = 0;
    return 0x10000;
}

} // namespace

std::int64_t ExecuteShenmueScriptFn(ScenarioContext *context, std::int32_t index, void *params) {
    if (!context || !context->stack)
        return 0;
    const auto *const words = static_cast<const std::uint32_t *>(params);
    std::uint32_t result = 0;
    switch (index) {
    case 2: { // Script_CreateScenarioTask(functionOffset, argumentCount, arguments...)
        const std::int32_t functionOffset = static_cast<std::int32_t>(ReadWord(words, 0));
        const std::int32_t argumentCount = static_cast<std::int32_t>(ReadWord(words, 1));
        auto *const entry = functionOffset ? g_pCurrentTaskData + functionOffset : nullptr;
        ScenarioStack *const child = scenAllocateScntForScript(context->task, context->stack, entry, words + 2, argumentCount);
        result = child ? HLib_GetTaskDataOffset(child) : 0;
        break;
    }
    case 50: // Script_GetInputData(index, type)
        result = g_inputMask;
        break;
    case 316: { // Script_EV_PAK_AFS_Utils(mode, operation, offset2, offset, value)
        const std::int32_t mode = static_cast<std::int32_t>(ReadWord(words, 0));
        const std::int32_t operation = static_cast<std::int32_t>(ReadWord(words, 1));
        if (mode == 0) { // EVPAK_MODE_PACK_FILE
            if (operation == 0)
                result = 0xffffffffU; // queued
            else if (operation == 2)
                result = 0; // not busy -> polling loop exits
            else if (operation == 8)
                result = 0xffffffffU; // AFS entry ready
        } else if (mode == 1) { // EVPAK_MODE_AFS_READ
            if (operation == 0) {
                if (!g_fakeAfsOwner)
                    g_fakeAfsOwner = HLib_GetTaskDataOffset(HLib_AllocHeapMemBlock(64, 0));
                result = g_fakeAfsOwner;
            }
        }
        break;
    }
    case 80: { // Script_shenSeqUtils(mode, offset, value)
        const std::int32_t mode = static_cast<std::int32_t>(ReadWord(words, 0));
        if (mode >= 0 && mode < static_cast<std::int32_t>(std::size(kTrackFrames))) {
            g_sequenceActive = true;
            g_sequenceRemaining = kTrackFrames[static_cast<std::size_t>(mode)];
            result = 0;
        } else if (mode == -1) {
            if (g_sequenceActive && g_sequenceRemaining > 0) {
                --g_sequenceRemaining;
                result = 1;
            } else {
                g_sequenceActive = false;
                result = 0;
            }
        }
        break;
    }
    case 391: // MODU_Script_shenRequestGameModule(module, scene, area, entry)
        g_done = true;
        break;
    default:
        break;
    }
    context->stack->accumulator = result;
    Record(context, static_cast<std::uint32_t>(index), words, result);
    return result;
}

std::int64_t ExecuteSCNFScriptFn(ScenarioContext *context, std::uint32_t index, std::int64_t stackPointer, std::int32_t argumentCount) {
    if (!context || !context->stack)
        return 0;
    auto *const parameters = context->stack->asyncArguments;
    const auto *const source = reinterpret_cast<const std::uint32_t *>(stackPointer);
    const std::int32_t count = std::clamp(argumentCount, 0, kScenarioAsyncArgumentWords);
    for (std::int32_t argument = 0; argument < count; ++argument)
        parameters[argument] = source[argument];
    context->stack->asyncFunctionIndex = static_cast<std::uint16_t>(index);
    context->stack->accumulator = 0;
    const auto begin = SCNF_Functions[static_cast<std::size_t>(index) * 2U];
    const std::int64_t result = begin ? begin(context, parameters) : 0;
    g_tmpScnfReturnValue = context->stack->accumulator;
    Record(context, 0x8000U | index, source, static_cast<std::uint32_t>(result));
    return result;
}

void SetStrForScnArg(ScenarioContext *context, std::int64_t pointer, std::uint32_t, ScenarioRegisters *) {
    if (!context || !context->stack)
        return;
    auto *const parameters = context->stack->asyncArguments;
    parameters[0] = pointer && g_pCurrentTaskData ? static_cast<std::uint32_t>(reinterpret_cast<const std::byte *>(pointer) - g_pCurrentTaskData) : 0;
    parameters[1] = 0xffffffffU;
    context->stack->asyncFunctionIndex = 1;
    context->stack->accumulator = 0;
    if (SCNF_Functions[2])
        SCNF_Functions[2](context, parameters);
}

void ExecuteRemovedScriptFn(std::int32_t, ScenarioContext *, std::int64_t, void *) {
}
std::int64_t ExecuteMemScriptFn(std::int32_t, void *) {
    return 0;
}
std::int64_t ExecuteDoorSupportFunc(ScenarioContext *, std::uint32_t, void *) {
    return 0;
}

std::array<LegacyScenarioFunction, kLegacyScenarioFunctionCount> LegacyFunctions{};
std::array<GeneralScenarioFunction, kGeneralScenarioFunctionCount> GeneralPurposeFuncs{};
std::array<ShenmueScenarioFunction, kShenmueScenarioFunctionCount> ShenmueScriptFuncs{};
std::array<SCNFScenarioFunction, kSCNFScenarioFunctionCount> SCNF_Functions{};
std::array<DoorScenarioFunction, kDoorScenarioFunctionCount> DSWKFunctions{};
std::uint32_t g_tmpScnfReturnValue = 0;

} // namespace shenmue::s1

namespace {

std::vector<std::byte> ReadModule(const char *path) {
    std::FILE *const file = std::fopen(path, "rb");
    if (!file)
        return {};
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size())
        return {};
    return bytes;
}

std::uint32_t ModuleOffset(const CommandRecord &record, std::size_t argument) {
    return record.arguments[argument] >= g_moduleBase ? record.arguments[argument] - g_moduleBase : record.arguments[argument];
}

const CommandRecord *FindCommand(std::uint32_t command, std::uint32_t argument0, std::uint32_t argument1) {
    for (const CommandRecord &record : g_records) {
        if (record.command != command)
            continue;
        if (argument0 != 0xffffffffU && record.arguments[0] != argument0)
            continue;
        if (argument1 != 0xffffffffU && record.arguments[1] != argument1)
            continue;
        return &record;
    }
    return nullptr;
}

int Fail(const char *message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int Summarize(bool skip) {
    std::fprintf(stderr, "=== OP02 %s: %zu commands over %d frames ===\n", skip ? "skip" : "normal", g_records.size(), g_frame);
    int failures = 0;

    const CommandRecord *create1011 = nullptr;
    for (const CommandRecord &record : g_records)
        if (record.command == 2 && ModuleOffset(record, 0) == 0x1011)
            create1011 = &record;
    if (!create1011)
        failures += Fail("CreateScenarioTask(script_1011) missing");
    else if (create1011->arguments[1] != 1 || create1011->arguments[2] != SHEN_NAME(AKIR))
        failures += Fail("CreateScenarioTask(script_1011) arguments");
    else
        std::fprintf(stderr, "  create script_1011 arg0=0x%08x\n", create1011->arguments[2]);

    if (!FindCommand(168, SHEN_NAME(AKIR), 0))
        failures += Fail("scenToggleVisibleFor(AKIR, 0) missing");

    bool visibleOff = false;
    for (const CommandRecord &record : g_records)
        if (record.command == 31 && record.arguments[0] == SHEN_NAME(AKIR) && record.arguments[1] == 2)
            visibleOff = true;
    if (!visibleOff)
        failures += Fail("SetCharaVisibility(AKIR, 2) missing");

    const CommandRecord *const dynamicMap = FindCommand(174, 1, 0xffffffffU);
    if (!dynamicMap)
        failures += Fail("SetDynamicMapTransform missing");
    else {
        const Vector3 *const position = ModuleVector3(dynamicMap->arguments[1]);
        if (!position)
            failures += Fail("SetDynamicMapTransform position is null");
        else {
            std::fprintf(stderr, "  dynamic map 1: (%.1f, %.1f, %.1f) angle=0x%x\n", position->x, position->y, position->z,
                dynamicMap->arguments[2]);
            if (position->x != 5020.0f || position->y != -210.0f || position->z != -5620.0f || dynamicMap->arguments[2] != 0xC000)
                failures += Fail("SetDynamicMapTransform values");
        }
    }

    const CommandRecord *const voice = FindCommand(202, 0xffffffffU, 0xffffffffU);
    if (!voice)
        failures += Fail("LoadVoiceFile missing");
    else if (std::strcmp(ModuleString(voice->arguments[1]), "A0100") != 0)
        failures += Fail("LoadVoiceFile name");

    std::vector<std::int32_t> sequence;
    for (const CommandRecord &record : g_records)
        if (record.command == 80 && static_cast<std::int32_t>(record.arguments[0]) >= 0 &&
            static_cast<std::int32_t>(record.arguments[0]) < 7)
            sequence.push_back(static_cast<std::int32_t>(record.arguments[0]));
    std::fprintf(stderr, "  shenSeqUtils start order:");
    for (const std::int32_t value : sequence)
        std::fprintf(stderr, " %d", value);
    std::fprintf(stderr, "\n");
    if (!skip) {
        const std::array<std::int32_t, 7> expectedSequence{0, 1, 2, 3, 6, 4, 5};
        if (!std::equal(sequence.begin(), sequence.end(), expectedSequence.begin(), expectedSequence.end()))
            failures += Fail("shenSeqUtils order");

        bool imageKaku = false;
        bool imageHawk = false;
        for (const CommandRecord &record : g_records) {
            if (record.command != 143)
                continue;
            const char *const image = ModuleString(record.arguments[1]);
            imageKaku |= std::strcmp(image, "KAKU") == 0;
            imageHawk |= std::strcmp(image, "HAWK") == 0;
        }
        if (!imageKaku || !imageHawk)
            failures += Fail("CreateNPCImageModule KAKU/HAWK");
    }

    const CommandRecord *const display = FindCommand(391, 0xffffffffU, 0xffffffffU);
    if (!display)
        failures += Fail("Display transition missing");
    else if (display->arguments[1] != 1 || display->arguments[2] != 0x3230504FU || display->arguments[3] != (skip ? 49U : 0U))
        failures += Fail("Display transition values");
    else
        std::fprintf(stderr, "  display: scene=%d area=0x%08x entry=%d\n", static_cast<std::int32_t>(display->arguments[1]), display->arguments[2],
            static_cast<std::int32_t>(display->arguments[3]));

    return failures;
}

void WriteTrace(std::FILE *file) {
    std::fprintf(file, "frame\tcontext\tinstruction\tcommand\targ0\targ1\targ2\targ3\targ4\tresult\n");
    for (const CommandRecord &record : g_records) {
        std::fprintf(file, "%d\t0x%x\t0x%x\t%u", record.frame, record.context, record.instruction, record.command);
        for (std::size_t index = 0; index < 5; ++index)
            std::fprintf(file, "\t0x%08x", record.arguments[index]);
        std::fprintf(file, "\t0x%08x\n", record.result);
    }
}

void DumpTrace(const char *name) {
#if defined(SHENMUE_HOST_PSP)
    for (const char *root : {"host0:/", "ms0:/"}) {
        char path[160];
        std::snprintf(path, sizeof(path), "%s%s", root, name);
        if (std::FILE *file = std::fopen(path, "w")) {
            WriteTrace(file);
            std::fclose(file);
            return;
        }
    }
#else
    char path[256];
    std::snprintf(path, sizeof(path), "build/dc-menu-audit/%s", name);
    if (std::FILE *file = std::fopen(path, "w")) {
        WriteTrace(file);
        std::fclose(file);
    }
#endif
}

int Run(const std::vector<std::byte> &moduleBytes, bool skip) {
    g_arena.assign(kArenaBytes, std::byte{0});
    g_bump = 0;
    g_pCurrentTaskData = g_arena.data();
    g_records.clear();
    g_frame = 0;
    g_done = false;
    g_skip = skip;
    g_inputMask = skip ? 0x800U : 0U;
    g_sequenceActive = false;
    g_sequenceRemaining = 0;
    g_fakeAfsOwner = 0;

    auto *const module = static_cast<std::byte *>(HLib_AllocHeapMemBlock(static_cast<std::int64_t>(moduleBytes.size()), 0));
    if (!module) {
        std::fprintf(stderr, "FAIL: module allocation\n");
        return 2;
    }
    std::memcpy(module, moduleBytes.data(), moduleBytes.size());
    g_moduleBase = HLib_GetTaskDataOffset(module);
    g_currentInstruction = 0;
    g_scenVmCallHook = HarnessCallHook;
    if (ScenValidateModule(module, moduleBytes.size()) != ScenarioModuleStatus::Ok) {
        std::fprintf(stderr, "FAIL: module validation\n");
        return 2;
    }
    if (!ScenApplyModuleRelocations(reinterpret_cast<ScenarioHeader *>(module), moduleBytes.size())) {
        std::fprintf(stderr, "FAIL: module relocations\n");
        return 2;
    }

    ScenarioWindow *const window = ASEQ_SCNW_Init(nullptr);
    if (!window) {
        std::fprintf(stderr, "FAIL: scenario window\n");
        return 2;
    }
    // The startup root creates its child and returns, so Startup_EventBase
    // legitimately returns null; the child is what the frame loop drives.
    Startup_EventBase(window, reinterpret_cast<ScenarioHeader *>(module));
    if (!window->stacks && g_records.empty()) {
        std::fprintf(stderr, "FAIL: startup produced no contexts\n");
        return 2;
    }

    constexpr int kMaxFrames = 5000;
    for (int frame = 1; frame <= kMaxFrames && !g_done && window->stacks; ++frame) {
        g_frame = frame;
        scenParseStackRecursively(window);
    }

    g_scenVmCallHook = nullptr;
    const int failures = Summarize(skip);
    DumpTrace(skip ? "op02-harness-skip.tsv" : "op02-harness-normal.tsv");
    return failures;
}

} // namespace

int main(int argc, char **argv) {
    for (std::size_t index = 0; index < ShenmueScriptFuncs.size(); ++index)
        ShenmueScriptFuncs[index] = DummyCommand;
    for (std::size_t index = 0; index < SCNF_Functions.size(); ++index)
        SCNF_Functions[index] = HarnessScnfComplete;
    SCNF_Functions[0] = HarnessWaitFrames;
    SCNF_Functions[1] = HarnessWaitFrames;
    LegacyFunctions.fill(LegacyStub);
    GeneralPurposeFuncs.fill(GeneralPurposeStub);
    DSWKFunctions.fill(DummyCommand);

    std::vector<std::string> candidates;
    if (argc > 1)
        candidates.emplace_back(argv[1]);
    if (argc > 0 && argv[0]) {
        const std::string executable = argv[0];
        const std::size_t slash = executable.find_last_of("/\\");
        if (slash != std::string::npos)
            candidates.push_back(executable.substr(0, slash + 1) + "op02-vm.scn");
    }
    candidates.emplace_back("op02-vm.scn");
    candidates.emplace_back("host0:/op02-vm.scn");
    candidates.emplace_back("ms0:/op02-vm.scn");
    candidates.emplace_back("ms0:/PSP/GAME/OP02/op02-vm.scn");
    candidates.emplace_back("ef0:/op02-vm.scn");
    candidates.emplace_back("build/dc-menu-audit/intro-usa-repro/op02-vm.scn");

    std::vector<std::byte> module;
    for (const std::string &candidate : candidates) {
        module = ReadModule(candidate.c_str());
        if (!module.empty())
            break;
    }
    if (module.empty()) {
        std::fprintf(stderr, "cannot read the OP02 VM module. Tried:\n");
        for (const std::string &candidate : candidates)
            std::fprintf(stderr, "  %s\n", candidate.c_str());
        std::fprintf(stderr, "Copy op02-vm.scn to the USBHostFS root (host0:/) or to ms0:/op02-vm.scn.\n");
        return 2;
    }
    const int normal = Run(module, false);
    const int skipped = Run(module, true);
    const bool pass = normal == 0 && skipped == 0;
    std::fprintf(stderr, "OP02 harness: %s\n", pass ? "PASS" : "FAIL");
#if defined(SHENMUE_HOST_PSP)
    for (const char *candidate : {"host0:/op02_harness_result.txt", "ms0:/op02_harness_result.txt"}) {
        if (std::FILE *file = std::fopen(candidate, "w")) {
            std::fprintf(file, "%s\n", pass ? "PASS" : "FAIL");
            std::fclose(file);
            break;
        }
    }
#endif
    return pass ? 0 : 1;
}
