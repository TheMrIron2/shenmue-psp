// Focused host fixture test for scenario context lifetime/reaping.
//
// Exercises the real scen_vm.cpp scheduler with guard-banded allocations and a
// hand-assembled `ctrl.ret` module. The command boundary is stubbed so the test
// isolates context traversal, child insertion and reaping.
//
// Build (host, ASan+UBSan):
//   g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
//     -ffunction-sections -fdata-sections \
//     -Isrc -Ithird_party/nuklear -Ilibs/ShenmueDK/include -DSHENMUE_TARGET_S1_PC=1 \
//     tools/dc/scen_lifetime_test.cpp src/s1/task/scen_vm.cpp \
//     -Wl,--gc-sections -o build/dc-menu-audit/scen_lifetime_test
//   ASAN_OPTIONS=detect_leaks=0 build/dc-menu-audit/scen_lifetime_test
#include "s1/task/scen.hpp"
#include "s1/hlib.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace shenmue::s1 {

namespace {
constexpr std::size_t kGuard = 16;
std::unordered_map<void *, std::size_t> g_allocSizes;
std::vector<void *> g_freed;
} // namespace

std::byte *g_pCurrentTaskData = nullptr;

void *HLib_AllocHeapMemBlock(std::int64_t size, std::int32_t) {
    const std::size_t bytes = static_cast<std::size_t>(size);
    auto *base = static_cast<std::byte *>(std::malloc(bytes + 2 * kGuard));
    if (!base)
        return nullptr;
    std::memset(base, 0xCD, kGuard);
    std::memset(base + kGuard, 0, bytes);
    std::memset(base + kGuard + bytes, 0xCD, kGuard);
    g_allocSizes[base + kGuard] = bytes;
    return base + kGuard;
}

std::uint64_t HLib_FreeTaskData(HLib::TaskData *taskData, std::int32_t) {
    void *const mid = taskData;
    const auto it = g_allocSizes.find(mid);
    assert(it != g_allocSizes.end() && "free of unknown allocation");
    auto *const base = static_cast<std::byte *>(mid) - kGuard;
    for (std::size_t i = 0; i < kGuard; ++i)
        assert(std::to_integer<int>(base[i]) == 0xCD && "front guard overwritten");
    for (std::size_t i = 0; i < kGuard; ++i)
        assert(std::to_integer<int>(base[kGuard + it->second + i]) == 0xCD && "rear guard overwritten");
    g_freed.push_back(mid);
    g_allocSizes.erase(it);
    std::free(base);
    return 1;
}

const void *g_offsetBasePointer = nullptr;
std::uint32_t g_offsetBaseValue = 0;

std::uint32_t HLib_GetTaskDataOffset(const void *pointer) {
    if (!pointer || !g_offsetBasePointer)
        return 0;
    const auto delta = reinterpret_cast<std::uintptr_t>(pointer) - reinterpret_cast<std::uintptr_t>(g_offsetBasePointer);
    return g_offsetBaseValue + static_cast<std::uint32_t>(delta);
}

// Command-boundary stubs: the fixtures never dispatch a real command.
void SetStrForScnArg(ScenarioContext *, std::int64_t, std::uint32_t, ScenarioRegisters *) {
}
std::int64_t ExecuteMemScriptFn(std::int32_t, void *) {
    return 0;
}
std::int64_t ExecuteShenmueScriptFn(ScenarioContext *, std::int32_t, void *) {
    return 0;
}
std::int64_t ExecuteSCNFScriptFn(ScenarioContext *, std::uint32_t, std::int64_t, std::int32_t) {
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

using namespace shenmue::s1;

namespace {

constexpr std::byte kOpReturn{0x00};

ScenarioStack *nextOf(ScenarioStack *stack) {
    return stack->nextStack;
}

std::uint16_t &statusOf(ScenarioStack *stack) {
    return stack->status;
}

struct Fixture {
    std::byte *module{};
    ScenarioHeader *header{};
    ScenarioWindow *window{};

    Fixture() {
        module = static_cast<std::byte *>(std::calloc(1, 0x100));
        module[0x30] = kOpReturn; // root entry
        module[0x40] = kOpReturn; // child entry
        header = reinterpret_cast<ScenarioHeader *>(module);
        header->wholeSize = 0x100;
        header->flags = 0;
        header->functionOffset = 0x30;
        header->dataSegmentOffset = 0x80;
        header->dataEndOffset = 0x100;
        header->entrypoint = 0x30;
        window = static_cast<ScenarioWindow *>(std::calloc(1, sizeof(ScenarioWindow)));
        window->info = header;
        window->activeInfo = header;
        window->dataSegment = module + 0x80;
        window->entrypoint = module + 0x30;
    }
    ~Fixture() {
        std::free(window);
        std::free(module);
    }
    ScenarioStack *root() {
        return scenAllocateScntForScript(window, nullptr, module + 0x30, nullptr, 0);
    }
    ScenarioStack *child(ScenarioStack *parent) {
        return scenAllocateScntForScript(window, parent, module + 0x40, nullptr, 0);
    }
};

bool wasFreed(void *pointer) {
    return std::find(g_freed.begin(), g_freed.end(), pointer) != g_freed.end();
}

struct ModuleBuffer {
    std::vector<std::byte> bytes;

    explicit ModuleBuffer(std::uint32_t size) : bytes(size, std::byte{0}) {
        auto *header = reinterpret_cast<ScenarioHeader *>(bytes.data());
        header->token = static_cast<std::int32_t>(SHEN_NAME(SCN3));
        header->wholeSize = static_cast<std::int32_t>(size);
        header->flags = 0x100;
        header->functionOffset = 0x40;
        header->dataSegmentOffset = 0x80;
        header->dataEndOffset = 0xC0;
        header->entrypoint = 0x30;
        header->relocationTableOffset = 0xC0;
    }
    ScenarioHeader *header() {
        return reinterpret_cast<ScenarioHeader *>(bytes.data());
    }
    std::uint32_t &word(std::uint32_t offset) {
        return *reinterpret_cast<std::uint32_t *>(bytes.data() + offset);
    }
};

void testModuleRelocations() {
    g_offsetBasePointer = nullptr;
    g_offsetBaseValue = 0;
    ModuleBuffer module(0xE0);
    module.word(0x90) = 0x88; // data target
    module.word(0x94) = 0x40; // code/function target
    module.word(0xC0) = 0x1402;
    module.word(0xC4) = 0x90;
    module.word(0xC8) = 0x2402;
    module.word(0xCC) = 0x94;
    module.word(0xD0) = 0;
    module.word(0xD4) = 0;
    assert(ScenValidateModule(module.bytes.data(), module.bytes.size()) == ScenarioModuleStatus::Ok);
    g_offsetBasePointer = module.bytes.data();
    g_offsetBaseValue = 0x1000;
    assert(ScenApplyModuleRelocations(module.header(), module.bytes.size()));
    assert(module.word(0x90) == 0x1088);
    assert(module.word(0x94) == 0x1040);
    // A relocated module no longer validates, which prevents double rebasing.
    assert(ScenValidateModule(module.bytes.data(), module.bytes.size()) == ScenarioModuleStatus::BadRelocation);
    assert(!ScenApplyModuleRelocations(module.header(), module.bytes.size()));
    g_offsetBasePointer = nullptr;
    g_offsetBaseValue = 0;
    std::fprintf(stderr, "ok: module relocations\n");
}

void testModuleRejections() {
    g_offsetBasePointer = nullptr;
    g_offsetBaseValue = 0;
    {
        ModuleBuffer module(0xE0);
        module.word(0x90) = 0x88;
        module.word(0xC0) = 0x1402;
        module.word(0xC4) = 0x90;
        assert(ScenValidateModule(module.bytes.data(), module.bytes.size() - 1) == ScenarioModuleStatus::Truncated);
    }
    {
        ModuleBuffer module(0xE0);
        module.header()->token = 0;
        assert(ScenValidateModule(module.bytes.data(), module.bytes.size()) == ScenarioModuleStatus::BadToken);
    }
    {
        ModuleBuffer module(0xE0);
        module.header()->flags = 0x20000;
        assert(ScenValidateModule(module.bytes.data(), module.bytes.size()) == ScenarioModuleStatus::NativeProfile);
    }
    {
        ModuleBuffer module(0xE0);
        module.header()->dataEndOffset = 0x1000;
        assert(ScenValidateModule(module.bytes.data(), module.bytes.size()) == ScenarioModuleStatus::BadLayout);
    }
    {
        ModuleBuffer module(0xE0);
        module.word(0x90) = 0x88;
        module.word(0xC0) = 0x1402;
        module.word(0xC4) = 0x10; // site outside the data segment
        assert(ScenValidateModule(module.bytes.data(), module.bytes.size()) == ScenarioModuleStatus::BadRelocation);
    }
    {
        ModuleBuffer module(0xE0);
        module.word(0x90) = 0x10; // data target outside the data segment
        module.word(0xC0) = 0x1402;
        module.word(0xC4) = 0x90;
        assert(ScenValidateModule(module.bytes.data(), module.bytes.size()) == ScenarioModuleStatus::BadRelocation);
    }
    {
        ModuleBuffer module(0xE0);
        module.word(0x90) = 0x88;
        module.word(0xC0) = 0x9999; // unknown relocation kind
        module.word(0xC4) = 0x90;
        assert(ScenValidateModule(module.bytes.data(), module.bytes.size()) == ScenarioModuleStatus::BadRelocation);
    }
    std::fprintf(stderr, "ok: module rejections\n");
}

// Optional end-to-end check for the lifted USA OP02 module. Verifies its single
// data relocation (site 0x1298 -> module offset 0x136a) before and after apply.
void testOp02Module(const char *path) {
    std::FILE *file = std::fopen(path, "rb");
    if (!file) {
        std::fprintf(stderr, "skip: %s not found\n", path);
        return;
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    assert(read == bytes.size());
    auto *const header = reinterpret_cast<ScenarioHeader *>(bytes.data());
    assert(ScenValidateModule(bytes.data(), bytes.size()) == ScenarioModuleStatus::Ok);
    constexpr std::uint32_t kSite = 0x1298;
    constexpr std::uint32_t kTarget = 0x136a;
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + kSite, sizeof(value));
    assert(value == kTarget);
    g_offsetBasePointer = bytes.data();
    g_offsetBaseValue = 0x2000;
    assert(ScenApplyModuleRelocations(header, bytes.size()));
    std::memcpy(&value, bytes.data() + kSite, sizeof(value));
    assert(value == g_offsetBaseValue + kTarget);
    g_offsetBasePointer = nullptr;
    g_offsetBaseValue = 0;
    std::fprintf(stderr, "ok: real OP02 module\n");
}

void testRelativeAddress() {
    Fixture f;
    f.module[0x30] = std::byte{0x73}; // addr.curr.rel3
    const std::uint32_t target = 0x88;
    std::memcpy(f.module + 0x31, &target, sizeof(target));
    f.module[0x35] = std::byte{0x80}; // alu.pop_eax
    f.module[0x36] = std::byte{0x00}; // ctrl.ret
    g_offsetBasePointer = f.module;
    g_offsetBaseValue = 0x1000;
    ScenarioStack *stack = scenAllocateScntForScript(f.window, nullptr, f.module + 0x30, nullptr, 0);
    assert(stack != nullptr);
    const std::uint32_t result = scenParseByPtr2Frames(f.window, stack);
    assert(result == 0x1088);
    assert((stack->status & kScenarioContextFinished) != 0);
    assert((stack->status & kScenarioContextFaulted) == 0);
    f.window->stacks = nullptr;
    HLib_FreeTaskData(reinterpret_cast<HLib::TaskData *>(stack), 0);
    g_offsetBasePointer = nullptr;
    g_offsetBaseValue = 0;
    std::fprintf(stderr, "ok: relative address\n");
}

void testFaultDiagnostic() {
    Fixture f;
    const std::uint32_t before = SCEN_GetVmFaultCount();
    f.header->wholeSize = 0x30; // entry instruction is past the declared module
    ScenarioStack *stack = scenAllocateScntForScript(f.window, nullptr, f.module + 0x30, nullptr, 0);
    assert(stack != nullptr);
    scenParseByPtr2Frames(f.window, stack);
    assert((stack->status & kScenarioContextFinished) != 0);
    assert((stack->status & kScenarioContextFaulted) != 0);
    assert(SCEN_GetVmFaultCount() == before + 1);
    f.window->stacks = nullptr;
    HLib_FreeTaskData(reinterpret_cast<HLib::TaskData *>(stack), 0);
    std::fprintf(stderr, "ok: fault diagnostic\n");
}

void testRootReturn() {
    g_freed.clear();
    Fixture f;
    ScenarioStack *root = f.root();
    assert(root && f.window->stacks == root);
    scenParseStackRecursively(f.window);
    assert(f.window->stacks == nullptr && "completed root must be reaped");
    assert(wasFreed(root));
    std::fprintf(stderr, "ok: root return\n");
}

void testParentWithLiveChild() {
    g_freed.clear();
    Fixture f;
    ScenarioStack *root = f.root();
    ScenarioStack *child = f.child(root);
    assert(child && nextOf(root) == child);
    scenParseStackRecursively(f.window);
    assert(f.window->stacks == nullptr && "both completed contexts reaped");
    assert(wasFreed(root) && wasFreed(child));
    std::fprintf(stderr, "ok: parent return with live child\n");
}

void testWaitingSiblingSurvives() {
    g_freed.clear();
    Fixture f;
    ScenarioStack *root = f.root();
    ScenarioStack *waiting = f.child(root);
    ScenarioStack *tail = f.child(waiting);
    statusOf(waiting) |= 2U; // waiting: must not parse or reap
    scenParseStackRecursively(f.window);
    assert(f.window->stacks == waiting && "waiting context stays linked");
    assert(nextOf(waiting) == nullptr && "completed tail reaped");
    assert(wasFreed(root) && wasFreed(tail));
    assert(!wasFreed(waiting));
    HLib_FreeTaskData(reinterpret_cast<HLib::TaskData *>(waiting), 0); // deliberate survivor
    std::fprintf(stderr, "ok: waiting sibling survives\n");
}

ScenarioStack *g_inserted = nullptr;
std::int64_t insertChildUpdate(ScenarioContext *context, void *) {
    g_inserted = scenAllocateScntForScript(context->task, context->stack, context->task->entrypoint + 0x10, nullptr, 0);
    // The child is a plain script: give it an out-of-range SCNF index so it runs
    // (and completes) instead of re-entering the update callback.
    if (g_inserted)
        g_inserted->asyncFunctionIndex = 0xffff;
    return 0; // suspend parent; do not run its bytecode this frame
}

void testChildInsertedDuringPass() {
    g_freed.clear();
    g_inserted = nullptr;
    Fixture f;
    ScenarioStack *root = f.root();
    SCNF_Functions[1] = insertChildUpdate;
    root->asyncFunctionIndex = 0; // SCNF index 0
    scenParseStackRecursively(f.window);
    SCNF_Functions[1] = nullptr;
    assert(g_inserted != nullptr && "child created during pass");
    assert(f.window->stacks == root && "suspended parent stays linked");
    assert(nextOf(root) == nullptr && "inserted child visited and reaped in the same pass");
    assert(wasFreed(g_inserted));
    assert(!wasFreed(root));
    HLib_FreeTaskData(reinterpret_cast<HLib::TaskData *>(root), 0); // deliberate survivor
    std::fprintf(stderr, "ok: child inserted during pass\n");
}

} // namespace

int main(int argc, char **argv) {
    testRootReturn();
    testParentWithLiveChild();
    testWaitingSiblingSurvives();
    testChildInsertedDuringPass();
    testModuleRelocations();
    testModuleRejections();
    testRelativeAddress();
    testFaultDiagnostic();
    if (argc > 1)
        testOp02Module(argv[1]);
    assert(g_allocSizes.empty() && "all allocations freed");
    std::fprintf(stderr, "all scenario VM fixtures passed\n");
    return 0;
}
