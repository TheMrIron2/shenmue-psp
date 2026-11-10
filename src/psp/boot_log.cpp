#include "psp/boot_log.hpp"

#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <malloc.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace shenmue::psp {
namespace {

constexpr char kFallbackLogPath[] = "ms0:/PSP/GAME/Shenmue/BOOT.LOG";
constexpr char kRelativeLogPath[] = "BOOT.LOG";
char g_logPath[260] = "BOOT.LOG";

void writeLine(const char *path, const char *line, std::size_t length) {
    const SceUID file = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (file < 0) {
        return;
    }
    sceIoWrite(file, line, length);
    sceIoClose(file);
}

} // namespace

namespace {
void truncateLog(const char *path) {
    const SceUID file = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (file >= 0)
        sceIoClose(file);
}
} // namespace

void InitBootLog(int argc, char **argv) {
    std::memcpy(g_logPath, kRelativeLogPath, sizeof(kRelativeLogPath));
    if (argc <= 0 || !argv || !argv[0] || !argv[0][0]) {
        return;
    }

    const char *lastSeparator = nullptr;
    for (const char *cursor = argv[0]; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            lastSeparator = cursor;
        }
    }
    if (!lastSeparator) {
        return;
    }

    const std::size_t directoryLength = static_cast<std::size_t>(lastSeparator - argv[0] + 1);
    constexpr char kLogName[] = "BOOT.LOG";
    if (directoryLength + sizeof(kLogName) > sizeof(g_logPath)) {
        return;
    }
    std::memcpy(g_logPath, argv[0], directoryLength);
    std::memcpy(g_logPath + directoryLength, kLogName, sizeof(kLogName));
}

void ResetBootLog() {
    truncateLog(g_logPath);
    if (std::strcmp(g_logPath, kFallbackLogPath) != 0)
        truncateLog(kFallbackLogPath);
}

void BootLog(const char *format, ...) {
    char line[256]{};
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(line, sizeof(line) - 2, format, arguments);
    va_end(arguments);

    if (written < 0) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(written);
    if (length > sizeof(line) - 2) {
        length = sizeof(line) - 2;
    }
    line[length++] = '\n';
    line[length] = '\0';

    writeLine(g_logPath, line, length);
    if (std::strcmp(g_logPath, kFallbackLogPath) != 0) {
        writeLine(kFallbackLogPath, line, length);
    }
}

void BootLogHeap(const char *stage) {
    const struct mallinfo heap = mallinfo();
    BootLog("heap: %s arena=%u used=%u free=%u top=%u stack=%d", stage ? stage : "<unknown>",
            static_cast<unsigned>(heap.arena), static_cast<unsigned>(heap.uordblks),
            static_cast<unsigned>(heap.fordblks), static_cast<unsigned>(heap.keepcost),
            sceKernelGetThreadStackFreeSize(0));
}

} // namespace shenmue::psp
