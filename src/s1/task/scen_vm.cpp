#include "s1/task/scen_state.hpp"

namespace shenmue::s1::scen_detail {

ScenarioStack *&NextStack(ScenarioStack &stack) {
    return stack.nextStack;
}

std::uint16_t &StackStatus(ScenarioStack &stack) {
    return stack.status;
}

std::uint32_t PointerOffset(const void *pointer) {
    return HLib_GetTaskDataOffset(pointer);
}

std::byte *PointerFromOffset(std::uint32_t offset) {
    return offset && g_pCurrentTaskData ? g_pCurrentTaskData + static_cast<std::int32_t>(offset) : nullptr;
}

std::size_t OperandByteCount(std::uint8_t opcode) {
    switch ((opcode >> 4U) & 3U) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return 2;
    default:
        return 4;
    }
}

std::int32_t ReadOperand(std::byte *&instruction, std::uint8_t opcode, std::int32_t previous) {
    switch ((opcode >> 4U) & 3U) {
    case 0:
        return previous;
    case 1:
        return static_cast<std::int8_t>(*instruction++);
    case 2: {
        std::int16_t value = 0;
        std::memcpy(&value, instruction, sizeof(value));
        instruction += sizeof(value);
        return value;
    }
    default: {
        std::int32_t value = 0;
        std::memcpy(&value, instruction, sizeof(value));
        instruction += sizeof(value);
        return value;
    }
    }
}

void Push(ScenarioRegisters *&stackPointer, std::uint32_t value) {
    auto *word = reinterpret_cast<std::uint32_t *>(stackPointer);
    *--word = value;
    stackPointer = reinterpret_cast<ScenarioRegisters *>(word);
}

std::uint32_t Pop(ScenarioRegisters *&stackPointer) {
    auto *word = reinterpret_cast<std::uint32_t *>(stackPointer);
    const std::uint32_t value = *word++;
    stackPointer = reinterpret_cast<ScenarioRegisters *>(word);
    return value;
}

float AsFloat(std::uint32_t value) {
    return std::bit_cast<float>(value);
}

std::uint32_t AsWord(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

} // namespace shenmue::s1::scen_detail

namespace shenmue::s1 {

namespace {

std::uint32_t g_scenVmFaultCount = 0;

std::uint32_t ReadModuleWord(const std::byte *module, std::uint32_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, module + offset, sizeof(value));
    return value;
}

void WriteModuleWord(std::byte *module, std::uint32_t offset, std::uint32_t value) {
    std::memcpy(module + offset, &value, sizeof(value));
}

bool ScenarioHeaderPassesLayout(const ScenarioHeader &header) {
    const auto wholeSize = static_cast<std::uint32_t>(header.wholeSize);
    if (wholeSize < sizeof(ScenarioHeader))
        return false;
    const auto within = [wholeSize](std::int32_t offset) {
        return offset >= static_cast<std::int32_t>(sizeof(ScenarioHeader)) && static_cast<std::uint32_t>(offset) < wholeSize;
    };
    if (!within(header.functionOffset) || !within(header.dataSegmentOffset) || !within(header.entrypoint))
        return false;
    const auto dataStart = static_cast<std::uint32_t>(header.dataSegmentOffset);
    const auto dataEnd = static_cast<std::uint32_t>(header.dataEndOffset);
    const auto reloc = static_cast<std::uint32_t>(header.relocationTableOffset);
    return dataEnd >= dataStart && dataEnd <= wholeSize && reloc >= dataEnd && reloc <= wholeSize;
}

} // namespace

ScenarioVmCallHook g_scenVmCallHook = nullptr;

std::uint32_t SCEN_GetVmFaultCount() noexcept {
    return g_scenVmFaultCount;
}

ScenarioModuleStatus ScenValidateModule(const std::byte *module, std::uint64_t availableSize) {
    if (!module || availableSize < sizeof(ScenarioHeader))
        return ScenarioModuleStatus::Truncated;
    const auto *const header = reinterpret_cast<const ScenarioHeader *>(module);
    const auto wholeSize = static_cast<std::uint32_t>(header->wholeSize);
    if (wholeSize < sizeof(ScenarioHeader) || static_cast<std::uint64_t>(wholeSize) > availableSize)
        return ScenarioModuleStatus::Truncated;
    if (static_cast<std::uint32_t>(header->token) != SHEN_NAME(SCN3))
        return ScenarioModuleStatus::BadToken;
    if ((static_cast<std::uint32_t>(header->flags) & 0x00ff0000U) != 0)
        return ScenarioModuleStatus::NativeProfile;
    if (!ScenarioHeaderPassesLayout(*header))
        return ScenarioModuleStatus::BadLayout;

    const auto dataStart = static_cast<std::uint32_t>(header->dataSegmentOffset);
    const auto dataEnd = static_cast<std::uint32_t>(header->dataEndOffset);
    const auto codeStart = static_cast<std::uint32_t>(header->entrypoint);
    for (std::uint32_t offset = static_cast<std::uint32_t>(header->relocationTableOffset); offset + 8U <= wholeSize; offset += 8U) {
        const std::uint32_t kind = ReadModuleWord(module, offset);
        const std::uint32_t site = ReadModuleWord(module, offset + 4U);
        if (!kind && !site)
            break;
        if ((kind != 0x2402U && kind != 0x1402U) || site < dataStart || site > dataEnd - 4U)
            return ScenarioModuleStatus::BadRelocation;
        const std::uint32_t target = ReadModuleWord(module, site);
        if (kind == 0x1402U) {
            if (target < dataStart || target >= dataEnd)
                return ScenarioModuleStatus::BadRelocation;
        } else if (target < codeStart || target >= dataStart) {
            return ScenarioModuleStatus::BadRelocation;
        }
    }
    return ScenarioModuleStatus::Ok;
}

bool ScenApplyModuleRelocations(ScenarioHeader *module, std::uint64_t availableSize) {
    if (!module || ScenValidateModule(reinterpret_cast<const std::byte *>(module), availableSize) != ScenarioModuleStatus::Ok)
        return false;
    auto *const bytes = reinterpret_cast<std::byte *>(module);
    const auto wholeSize = static_cast<std::uint32_t>(module->wholeSize);
    const std::uint32_t moduleBase = HLib_GetTaskDataOffset(module);
    for (std::uint32_t offset = static_cast<std::uint32_t>(module->relocationTableOffset); offset + 8U <= wholeSize; offset += 8U) {
        const std::uint32_t kind = ReadModuleWord(bytes, offset);
        const std::uint32_t site = ReadModuleWord(bytes, offset + 4U);
        if (!kind && !site)
            break;
        const std::uint32_t target = ReadModuleWord(bytes, site);
        if (moduleBase > (std::numeric_limits<std::uint32_t>::max)() - target)
            return false;
        WriteModuleWord(bytes, site, moduleBase + target);
    }
    return true;
}

ScenarioWindow *ASEQ_SCNW_Init(HLib::Task *scenarioTask) {
    auto *const window = static_cast<ScenarioWindow *>(HLib_AllocHeapMemBlock(sizeof(ScenarioWindow), static_cast<std::int32_t>(SHEN_NAME(SCNW))));
    std::memset(window, 0, sizeof(*window));
    window->task = scenarioTask;
    window->stackEnd = window->workspace + 8512;
    window->stackLimit = window->workspace + 256;
    window->stackBase = window->workspace;
    window->stackSize = 8192;
    window->alternateStackLimit = window->workspace + 256;
    window->alternateStackBase = window->workspace;
    window->setStringArgument = SetStrForScnArg;
    window->executeRemovedFunction = ExecuteRemovedScriptFn;
    window->executeMemoryFunction = ExecuteMemScriptFn;
    window->executeGameFunction = ExecuteShenmueScriptFn;
    window->executeScenarioFunction = ExecuteSCNFScriptFn;
    window->executeDoorFunction = ExecuteDoorSupportFunc;
    return window;
}

ScenarioStack *scenAllocateScntForScript(
    ScenarioWindow *scenarioWindow, ScenarioStack *parent, const void *entryInstruction, const std::uint32_t *arguments, std::int32_t argumentCount) {
    constexpr std::int32_t kMaximumArgumentCount = kScenarioSavedWordCount - 2;
    if (!scenarioWindow || !scenarioWindow->info || argumentCount < 0 || argumentCount > kMaximumArgumentCount || (argumentCount != 0 && !arguments)) {
        return nullptr;
    }
    auto *const stack = static_cast<ScenarioStack *>(HLib_AllocHeapMemBlock(sizeof(ScenarioStack), static_cast<std::int32_t>(SHEN_NAME(SCNT))));
    if (!stack)
        return nullptr;
    std::memset(stack, 0, sizeof(ScenarioStack));

    stack->instruction = static_cast<std::byte *>(const_cast<void *>(entryInstruction));

    const std::uint32_t stackTopOffset =
        kScenarioVmStackTopOffset + ((static_cast<std::uint32_t>(scenarioWindow->info->flags) & 0x00ff0000U) != 0 ? 8U : 0U);
    auto *const stackTop = reinterpret_cast<ScenarioRegisters *>(
        scenarioWindow->workspace + stackTopOffset - 4U * static_cast<std::uint32_t>(argumentCount));
    stack->stackPointer = stackTop;
    stack->framePointer = stackTop;

    // The child's arguments become the low words of its initial operand stack.
    std::uint32_t *output = stack->savedWords + (kScenarioSavedWordCount - 1);
    if (argumentCount != 0) {
        const std::uint32_t *input = arguments + argumentCount;
        for (std::int32_t index = 0; index < argumentCount; ++index)
            *output-- = *--input;
    }
    stack->savedWords[kScenarioSavedWordCount - 1 - argumentCount] = 0;
    stack->savedWords[kScenarioSavedWordCount - 2 - argumentCount] = 0;
    stack->accumulator = 0;
    stack->asyncFunctionIndex = 0;

    ScenarioStack **link = nullptr;
    if (parent) {
        link = &parent->nextStack;
    } else {
        link = &scenarioWindow->stacks;
        while (*link)
            link = &(*link)->nextStack;
    }
    stack->nextStack = *link;
    *link = stack;
    return stack;
}

std::uint32_t SCN_Parse(ScenarioContext *context, ScenarioStack *stack, ScenarioRegisters *controlBase, std::byte *, std::byte *) {
    if (!context || !context->task || !stack || !context->task->activeInfo) {
        if (stack) {
            StackStatus(*stack) |= kScenarioContextFinished | kScenarioContextFaulted;
            ++g_scenVmFaultCount;
        }
        return 0;
    }

    std::byte *instruction = stack->instruction;
    ScenarioRegisters *stackPointer = stack->stackPointer;
    ScenarioRegisters *framePointer = stack->framePointer;
    std::uint32_t accumulator = stack->accumulator;
    std::int32_t operand = stack->previousOperand;
    const auto absoluteTarget = [controlBase](std::int32_t displacement) {
        std::byte *base = nullptr;
        std::memcpy(&base, controlBase, sizeof(base));
        return base + displacement;
    };

    const auto rangeContains = [](const void *begin, std::size_t size, const void *pointer, std::size_t requested) {
        if (!begin || !pointer || requested > size)
            return false;
        const auto first = reinterpret_cast<std::uintptr_t>(begin);
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return address >= first && address - first <= size - requested;
    };
    const std::int32_t scriptSize = context->task->activeInfo->wholeSize;
    const auto canReadInstruction = [&](const void *pointer, std::size_t size) {
        return scriptSize >= static_cast<std::int32_t>(sizeof(ScenarioHeader)) &&
            rangeContains(context->task->activeInfo, static_cast<std::size_t>(scriptSize), pointer, size);
    };
    const auto canReadTaskMemory = [&](const void *pointer, std::size_t size) { return rangeContains(context->task, sizeof(ScenarioWindow), pointer, size); };
    const auto canReadStackWords = [&](std::size_t count) { return canReadTaskMemory(stackPointer, count * sizeof(std::uint32_t)); };
    const auto canPushStackWords = [&](std::size_t count) {
        const auto address = reinterpret_cast<std::uintptr_t>(stackPointer);
        const std::size_t bytes = count * sizeof(std::uint32_t);
        if (address < bytes)
            return false;
        return canReadTaskMemory(reinterpret_cast<const void *>(address - bytes), bytes);
    };
    auto save = [&] {
        stack->instruction = instruction;
        stack->stackPointer = stackPointer;
        stack->framePointer = framePointer;
        stack->accumulator = accumulator;
        stack->previousOperand = static_cast<std::uint16_t>(operand);
    };
    auto fail = [&] {
        save();
        StackStatus(*stack) |= kScenarioContextFinished | kScenarioContextFaulted;
        ++g_scenVmFaultCount;
        return accumulator;
    };

    for (;;) {
        if (!canReadInstruction(instruction, 1))
            return fail();
        const std::byte *const instructionStart = instruction;
        const auto opcode = static_cast<std::uint8_t>(*instruction++);

        if ((opcode & 0xc0U) == 0x40U) {
            const std::size_t operandBytes = OperandByteCount(opcode);
            if (!canReadInstruction(instruction, operandBytes))
                return fail();
            operand = ReadOperand(instruction, opcode, operand);
            const std::uint32_t submode = opcode & 3U;
            const std::uint32_t category = opcode & 0x0cU;
            std::uint32_t value = 0;
            if (category == 0) {
                if (submode == 0) {
                    value = static_cast<std::uint32_t>(operand);
                } else {
                    value = PointerOffset(absoluteTarget(operand));
                }
            } else {
                std::byte *base = nullptr;
                if (category == 4)
                    base = context->task->workspace;
                else if (category == 8)
                    base = context->task->workspace + 256;
                else
                    base = reinterpret_cast<std::byte *>(framePointer);
                const auto baseAddress = reinterpret_cast<std::uintptr_t>(base);
                const auto addressValue = baseAddress + static_cast<std::intptr_t>(operand);
                auto *const address = reinterpret_cast<std::byte *>(addressValue);
                const std::size_t valueSize = submode == 0 ? 1U : submode == 1 ? 2U : submode == 2 ? 4U : 0U;
                if (valueSize && !canReadTaskMemory(address, valueSize))
                    return fail();
                if (submode == 0) {
                    value = static_cast<std::uint32_t>(static_cast<std::int32_t>(*reinterpret_cast<std::int8_t *>(address)));
                } else if (submode == 1) {
                    std::int16_t half = 0;
                    std::memcpy(&half, address, sizeof(half));
                    value = static_cast<std::uint32_t>(half);
                } else if (submode == 2) {
                    std::memcpy(&value, address, sizeof(value));
                } else {
                    value = category == 4 ? static_cast<std::uint32_t>(addressValue) : PointerOffset(address);
                }
            }
            if (!canPushStackWords(1))
                return fail();
            Push(stackPointer, value);
            continue;
        }

        if ((opcode & 0xc0U) == 0x80U) {
            const std::uint32_t operation = opcode & 0x3fU;
            std::size_t requiredWords = 0;
            if (operation == 0 || (operation >= 2 && operation <= 4) || operation == 14 || operation == 29 || operation == 30) {
                requiredWords = 1;
            } else if ((operation >= 5 && operation <= 13) || (operation >= 15 && operation <= 24) || (operation >= 31 && operation <= 38)) {
                requiredWords = 2;
            }
            if (operation == 1) {
                if (!canPushStackWords(1))
                    return fail();
            } else if (requiredWords && !canReadStackWords(requiredWords)) {
                return fail();
            }

            auto *words = reinterpret_cast<std::uint32_t *>(stackPointer);
            switch (operation) {
            case 0:
                accumulator = Pop(stackPointer);
                break;
            case 1:
                Push(stackPointer, accumulator);
                break;
            case 2: {
                std::byte *const address = PointerFromOffset(words[0]);
                if (!address)
                    return fail();
                words[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(*reinterpret_cast<std::int8_t *>(address)));
                break;
            }
            case 3: {
                std::byte *const address = PointerFromOffset(words[0]);
                if (!address)
                    return fail();
                std::int16_t value = 0;
                std::memcpy(&value, address, sizeof(value));
                words[0] = static_cast<std::uint32_t>(value);
                break;
            }
            case 4: {
                std::byte *const address = PointerFromOffset(words[0]);
                if (!address)
                    return fail();
                std::memcpy(&words[0], address, sizeof(words[0]));
                break;
            }
            case 5:
            case 6:
            case 7: {
                auto *const address = PointerFromOffset(words[1]);
                if (!address)
                    return fail();
                const std::size_t size = operation == 5 ? 1U : operation == 6 ? 2U : 4U;
                std::memcpy(address, &words[0], size);
                words[1] = words[0];
                Pop(stackPointer);
                break;
            }
            case 8:
                words[1] = words[1] == words[0] ? 0xffffffffU : 0;
                Pop(stackPointer);
                break;
            case 9:
                words[1] = words[1] != words[0] ? 0xffffffffU : 0;
                Pop(stackPointer);
                break;
            case 10:
                words[1] = static_cast<std::int32_t>(words[1]) < static_cast<std::int32_t>(words[0]) ? 0xffffffffU : 0;
                Pop(stackPointer);
                break;
            case 11:
                words[1] = static_cast<std::int32_t>(words[1]) <= static_cast<std::int32_t>(words[0]) ? 0xffffffffU : 0;
                Pop(stackPointer);
                break;
            case 12:
                words[1] = static_cast<std::int32_t>(words[1]) > static_cast<std::int32_t>(words[0]) ? 0xffffffffU : 0;
                Pop(stackPointer);
                break;
            case 13:
                words[1] = static_cast<std::int32_t>(words[1]) >= static_cast<std::int32_t>(words[0]) ? 0xffffffffU : 0;
                Pop(stackPointer);
                break;
            case 14:
                words[0] = ~words[0];
                break;
            case 15:
                words[1] &= words[0];
                Pop(stackPointer);
                break;
            case 16:
                words[1] |= words[0];
                Pop(stackPointer);
                break;
            case 17:
                words[1] ^= words[0];
                Pop(stackPointer);
                break;
            case 18:
                words[1] += words[0];
                Pop(stackPointer);
                break;
            case 19:
                words[1] -= words[0];
                Pop(stackPointer);
                break;
            case 20:
                words[1] *= words[0];
                Pop(stackPointer);
                break;
            case 21:
            case 22: {
                const std::int32_t divisor = static_cast<std::int32_t>(words[0]);
                const std::int32_t dividend = static_cast<std::int32_t>(words[1]);
                if (!divisor || (dividend == (std::numeric_limits<std::int32_t>::min)() && divisor == -1)) {
                    return fail();
                }
                words[1] = static_cast<std::uint32_t>(operation == 21 ? dividend / divisor : dividend % divisor);
                Pop(stackPointer);
                break;
            }
            case 23:
                words[1] <<= (words[0] & 31U);
                Pop(stackPointer);
                break;
            case 24:
                words[1] = static_cast<std::uint32_t>(static_cast<std::int32_t>(words[1]) >> (words[0] & 31U));
                Pop(stackPointer);
                break;
            case 29:
                words[0] = AsWord(static_cast<float>(static_cast<std::int32_t>(words[0])));
                break;
            case 30:
                words[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(AsFloat(words[0])));
                break;
            case 31:
                words[1] = AsFloat(words[0]) <= AsFloat(words[1]) ? 0 : 0xffffffffU;
                Pop(stackPointer);
                break;
            case 32:
                words[1] = AsFloat(words[0]) < AsFloat(words[1]) ? 0 : 0xffffffffU;
                Pop(stackPointer);
                break;
            case 33:
                words[1] = AsFloat(words[1]) <= AsFloat(words[0]) ? 0 : 0xffffffffU;
                Pop(stackPointer);
                break;
            case 34:
                words[1] = AsFloat(words[1]) < AsFloat(words[0]) ? 0 : 0xffffffffU;
                Pop(stackPointer);
                break;
            case 35:
                words[1] = AsWord(AsFloat(words[0]) + AsFloat(words[1]));
                Pop(stackPointer);
                break;
            case 36:
                words[1] = AsWord(AsFloat(words[1]) - AsFloat(words[0]));
                Pop(stackPointer);
                break;
            case 37:
                words[1] = AsWord(AsFloat(words[0]) * AsFloat(words[1]));
                Pop(stackPointer);
                break;
            case 38:
                words[1] = AsWord(AsFloat(words[1]) / AsFloat(words[0]));
                Pop(stackPointer);
                break;
            default:
                break;
            }
            continue;
        }

        if ((opcode & 0xc0U) != 0)
            continue;
        const std::size_t operandBytes = OperandByteCount(opcode);
        if (!canReadInstruction(instruction, operandBytes))
            return fail();
        operand = ReadOperand(instruction, opcode, operand);
        switch (opcode & 0x0fU) {
        case 0: {
            if (!rangeContains(context->task, sizeof(ScenarioWindow), framePointer, sizeof(ScenarioRegisters))) {
                return fail();
            }
            const ScenarioRegisters *const oldFrame = framePointer;
            framePointer = reinterpret_cast<ScenarioRegisters *>(PointerFromOffset(oldFrame->r0));
            instruction = PointerFromOffset(oldFrame->r1);
            stackPointer = reinterpret_cast<ScenarioRegisters *>(const_cast<std::uint32_t *>(&oldFrame->r2));
            if (framePointer)
                break;
            StackStatus(*stack) |= kScenarioContextFinished;
            save();
            return accumulator;
        }
        case 1: {
            if (!canPushStackWords(2))
                return fail();
            auto *words = reinterpret_cast<std::uint32_t *>(stackPointer);
            words[-1] = PointerOffset(instruction);
            words -= 2;
            words[0] = PointerOffset(framePointer);
            framePointer = reinterpret_cast<ScenarioRegisters *>(words);
            stackPointer = framePointer;
            instruction = absoluteTarget(operand);
            break;
        }
        case 2: {
            if (!canReadStackWords(1) || !canPushStackWords(1))
                return fail();
            auto *words = reinterpret_cast<std::uint32_t *>(stackPointer);
            std::byte *const target = PointerFromOffset(words[0]);
            if (!target)
                return fail();
            words[0] = PointerOffset(instruction);
            --words;
            words[0] = PointerOffset(framePointer);
            framePointer = reinterpret_cast<ScenarioRegisters *>(words);
            stackPointer = framePointer;
            instruction = target;
            break;
        }
        case 3: {
            const auto address = reinterpret_cast<std::uintptr_t>(stackPointer);
            const auto adjusted = address + static_cast<std::intptr_t>(4 * (operand / 4));
            auto *const next = reinterpret_cast<ScenarioRegisters *>(adjusted);
            if (!rangeContains(context->task, sizeof(ScenarioWindow), next, 0)) {
                return fail();
            }
            stackPointer = next;
            break;
        }
        case 4:
            instruction = absoluteTarget(operand);
            break;
        case 5:
            instruction += operand;
            break;
        case 6:
            if (!accumulator)
                instruction += operand;
            break;
        case 7:
            stack->previousOperand = static_cast<std::uint16_t>(operand);
            break;
        case 8: {
            std::byte *const string = absoluteTarget(operand);
            if (!canReadInstruction(string, 1))
                return fail();
            SetStrForScnArg(context, reinterpret_cast<std::intptr_t>(string), opcode, controlBase);
            save();
            return accumulator;
        }
        case 9:
            if (operand < 0 || static_cast<std::size_t>(operand) >= LegacyFunctions.size() || !LegacyFunctions[static_cast<std::size_t>(operand)] ||
                !canReadStackWords(1)) {
                return fail();
            }
            accumulator = LegacyFunctions[static_cast<std::size_t>(operand)](stackPointer);
            break;
        case 10:
            if (operand < 0 || static_cast<std::size_t>(operand) >= GeneralPurposeFuncs.size() || !GeneralPurposeFuncs[static_cast<std::size_t>(operand)] ||
                !canReadStackWords(4)) {
                return fail();
            }
            accumulator = static_cast<std::uint32_t>(ExecuteMemScriptFn(operand, stackPointer));
            break;
        case 11:
            if (operand < 0 || static_cast<std::size_t>(operand) >= ShenmueScriptFuncs.size() || !ShenmueScriptFuncs[static_cast<std::size_t>(operand)] ||
                !canReadStackWords(1)) {
                return fail();
            }
            if (g_scenVmCallHook)
                g_scenVmCallHook(context, instructionStart, static_cast<std::uint32_t>(operand), false);
            accumulator = static_cast<std::uint32_t>(ExecuteShenmueScriptFn(context, operand, stackPointer));
            break;
        case 12: {
            const std::int32_t argumentCount = stack->previousOperand;
            constexpr std::int32_t kMaximumParameters = kScenarioAsyncArgumentWords;
            const std::size_t beginSlot = static_cast<std::size_t>(operand) * 2;
            if (operand < 0 || beginSlot >= SCNF_Functions.size() || !SCNF_Functions[beginSlot] || argumentCount < 0 || argumentCount > kMaximumParameters ||
                !canReadStackWords(static_cast<std::size_t>(argumentCount))) {
                return fail();
            }
            if (g_scenVmCallHook)
                g_scenVmCallHook(context, instructionStart, static_cast<std::uint32_t>(operand), true);
            const std::int64_t result =
                ExecuteSCNFScriptFn(context, static_cast<std::uint32_t>(operand), reinterpret_cast<std::int64_t>(stackPointer), argumentCount);
            if ((result & 0x10000) != 0) {
                accumulator = g_tmpScnfReturnValue;
                break;
            }
            accumulator = static_cast<std::uint32_t>(result);
            if (canReadInstruction(instruction, 2) && static_cast<std::uint8_t>(*instruction) == 0x13U) {
                const std::uint8_t bytes = static_cast<std::uint8_t>(instruction[1]);
                auto *const adjusted = reinterpret_cast<std::byte *>(stackPointer) + (bytes & 0xfcU);
                if (!rangeContains(context->task, sizeof(ScenarioWindow), adjusted, 0)) {
                    return fail();
                }
                instruction += 2;
                stackPointer = reinterpret_cast<ScenarioRegisters *>(adjusted);
            }
            save();
            return accumulator;
        }
        case 13: {
            if (operand < 0 || static_cast<std::size_t>(operand) >= DSWKFunctions.size() || !DSWKFunctions[static_cast<std::size_t>(operand)] ||
                !canReadStackWords(1)) {
                return fail();
            }
            ExecuteDoorSupportFunc(context, static_cast<std::uint32_t>(operand), stackPointer);
            accumulator = stack->accumulator;
            break;
        }
        default:
            break;
        }
    }
    return fail();
}

void scenCollectCompletedStacks(ScenarioWindow *window) {
    if (!window)
        return;
    ScenarioStack **link = &window->stacks;
    while (*link) {
        ScenarioStack *const candidate = *link;
        if ((StackStatus(*candidate) & kScenarioContextFinished) != 0) {
            *link = NextStack(*candidate);
            HLib_FreeTaskData(reinterpret_cast<HLib::TaskData *>(candidate), 0);
        } else {
            link = &NextStack(*candidate);
        }
    }
}

std::uint32_t scenParseByPtr(ScenarioContext *context) {
    if (!context || !context->task || !context->stack)
        return 0;
    ScenarioStack *const activeStack = context->stack;
    const auto activeWordCount = [&]() {
        const auto stackAddress = reinterpret_cast<std::uintptr_t>(activeStack->stackPointer);
        const auto workspaceAddress = reinterpret_cast<std::uintptr_t>(context->task->workspace);
        const auto restoreEnd = workspaceAddress + kScenarioVmStackRestoreEndOffset;
        if (stackAddress < workspaceAddress || stackAddress > restoreEnd)
            return -1;
        const std::uintptr_t bytes = restoreEnd - stackAddress;
        if ((bytes & 3U) != 0 || bytes / 4U > static_cast<std::uintptr_t>(kScenarioSavedWordCount))
            return -1;
        return static_cast<std::int32_t>(bytes / 4U);
    };

    std::int32_t wordCount = activeWordCount();
    if (wordCount < 0) {
        StackStatus(*activeStack) |= kScenarioContextFinished | kScenarioContextFaulted;
        ++g_scenVmFaultCount;
    } else if (wordCount > 0) {
        std::uint32_t *const savedWords = activeStack->savedWords;
        std::memcpy(activeStack->stackPointer, savedWords + (kScenarioSavedWordCount - wordCount), static_cast<std::size_t>(wordCount) * 4);
    }

    if ((StackStatus(*activeStack) & kScenarioContextFinished) == 0 && context->task->info &&
        (static_cast<std::uint32_t>(context->task->info->flags) & 0x00ff0000U) == 0) {
        SCN_Parse(context, activeStack, reinterpret_cast<ScenarioRegisters *>(&context->task->activeInfo), reinterpret_cast<std::byte *>(activeStack), nullptr);
    }

    // Completed contexts are reaped by scenCollectCompletedStacks() after the
    // scheduler has advanced past them; freeing here would invalidate both the
    // returned accumulator and the caller's traversal link.
    wordCount = activeWordCount();
    if (wordCount > 0) {
        std::uint32_t *const savedWords = activeStack->savedWords;
        std::memcpy(savedWords + (kScenarioSavedWordCount - wordCount), activeStack->stackPointer, static_cast<std::size_t>(wordCount) * 4);
    }
    return activeStack->accumulator;
}

std::uint32_t scenParseByPtr2Frames(ScenarioWindow *scenarioWindow, ScenarioStack *stack) {
    if (!scenarioWindow || !stack)
        return 0;
    ScenarioContext context{scenarioWindow, stack};
    stack->accumulator = 0;
    const auto functionIndex = stack->asyncFunctionIndex;
    const std::size_t updateSlot = static_cast<std::size_t>(functionIndex) * 2 + 1;
    if (updateSlot < SCNF_Functions.size() && SCNF_Functions[updateSlot]) {
        const std::int64_t result = SCNF_Functions[updateSlot](&context, stack->asyncArguments);
        if (result != 0x10000)
            return static_cast<std::uint32_t>(result);
    }
    return scenParseByPtr(&context);
}

void scenParseStackRecursively(ScenarioWindow *scenarioWindow) {
    if (!scenarioWindow)
        return;
    // Traverse without freeing: SCN_Parse can insert children after the current
    // context, so the successor link must be re-read after each parse. Reaping is
    // deferred until the whole pass has finished.
    for (ScenarioStack *stack = scenarioWindow->stacks; stack; stack = NextStack(*stack)) {
        if ((StackStatus(*stack) & (kScenarioContextFinished | kScenarioContextSuspended)) == 0)
            scenParseByPtr2Frames(scenarioWindow, stack);
    }
    scenCollectCompletedStacks(scenarioWindow);
}

ScenarioStack *Startup_EventBase(ScenarioWindow *scenarioWindow, ScenarioHeader *info) {
    if (!scenarioWindow || !info || info->wholeSize < static_cast<std::int32_t>(sizeof(ScenarioHeader))) {
        return nullptr;
    }
    const auto validOffset = [info](std::int32_t offset) { return offset >= static_cast<std::int32_t>(sizeof(ScenarioHeader)) && offset < info->wholeSize; };
    if (!validOffset(info->functionOffset) || !validOffset(info->dataSegmentOffset) || !validOffset(info->entrypoint)) {
        return nullptr;
    }

    scenarioWindow->info = info;
    scenarioWindow->activeInfo = info;
    scenarioWindow->functionInfo = info;
    scenarioWindow->entryOffsets = nullptr;
    scenarioWindow->entryCount = 0;
    scenarioWindow->reserved024 = 0;
    g_pSCN_INFO_TaskData = info;
    scenarioWindow->dataSegment = reinterpret_cast<std::byte *>(info) + info->dataSegmentOffset;
    scenarioWindow->entrypoint = reinterpret_cast<std::byte *>(info) + info->entrypoint;
    const std::uint32_t block = 0;
    ScenarioStack *const stack = scenAllocateScntForScript(scenarioWindow, nullptr, reinterpret_cast<std::byte *>(info) + info->functionOffset, &block, 0);
    if (!stack)
        return nullptr;

    scenParseByPtr2Frames(scenarioWindow, stack);
    scenCollectCompletedStacks(scenarioWindow);
    for (ScenarioStack *candidate = scenarioWindow->stacks; candidate; candidate = NextStack(*candidate)) {
        if (candidate == stack)
            return stack;
    }
    return nullptr;
}

} // namespace shenmue::s1
