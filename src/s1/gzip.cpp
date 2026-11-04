#include "s1/file_loader.hpp"
#include "s1/hlib.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(SHENMUE_HOST_PSP)
#include "psp/boot_log.hpp"
#endif

#include <zlib.h>

namespace shenmue::s1 {

bool decompressGzip(void *source, void *destination, std::uint32_t decompressedSize, std::uint32_t compressedSize, bool traceTitle) {
    z_stream stream{};
#if defined(SHENMUE_HOST_PSP)
    stream.zalloc = [](voidpf, uInt items, uInt size) -> voidpf {
        if (size != 0 && items > UINT32_MAX / size)
            return nullptr;
        return HLib_AllocHeapMemBlock(static_cast<std::int64_t>(items) * size, SHEN_NAME(ZLIB));
    };
    stream.zfree = [](voidpf, voidpf address) {
        if (address)
            HLib_FreeTaskData(static_cast<HLib::TaskData *>(address), 0);
    };
#endif
    stream.next_in = static_cast<Bytef *>(source);
    stream.avail_in = compressedSize;
    stream.next_out = static_cast<Bytef *>(destination);
    stream.avail_out = decompressedSize;

    const int initResult = inflateInit2(&stream, MAX_WBITS + 16);
    if (initResult != Z_OK) {
#if defined(SHENMUE_HOST_PSP)
        if (traceTitle)
            psp::BootLog("gzip: inflateInit2 failed result=%d", initResult);
#endif
        return false;
    }
    const int inflateResult = inflate(&stream, Z_FINISH);
    const uLong totalIn = stream.total_in;
    const uLong totalOut = stream.total_out;
    const uInt availableIn = stream.avail_in;
    const uInt availableOut = stream.avail_out;
#if defined(SHENMUE_HOST_PSP)
    if (traceTitle) {
        std::uint32_t prefix = 0;
        std::memcpy(&prefix, destination, sizeof(prefix));
        psp::BootLog("gzip: inflate result=%d in=%u out=%u availIn=%u availOut=%u prefix=%08x", inflateResult,
                     static_cast<unsigned>(totalIn), static_cast<unsigned>(totalOut), static_cast<unsigned>(availableIn),
                     static_cast<unsigned>(availableOut), static_cast<unsigned>(prefix));
    }
#else
    (void)traceTitle;
#endif
    inflateEnd(&stream);
    return inflateResult == Z_STREAM_END && totalOut == decompressedSize;
}

void decompress(void *source, void *destination, std::int64_t, std::int64_t, std::uint32_t decompressedSize, std::int64_t,
                std::uint32_t compressedSize) {
    (void)decompressGzip(source, destination, decompressedSize, compressedSize, false);
}

void *LoadGzippedSpriteIntoHeapMem(const char *filename, std::int32_t allocationToken, std::uint32_t *decompressedSize) {
    if (decompressedSize)
        *decompressedSize = 0;

    const std::uint32_t compressedSize = static_cast<std::uint32_t>(shenOpenFile("sprite", filename));
#if defined(SHENMUE_HOST_PSP)
    const bool traceTitle = filename && std::strcmp(filename, "TITLE4_A.GZ") == 0;
    if (traceTitle)
        psp::BootLog("gzip: TITLE4_A.GZ indexed bytes=%u", compressedSize);
#else
    constexpr bool traceTitle = false;
#endif
    void *const compressed = HLib_AllocMemBlock(compressedSize, allocationToken);
    if (!compressed) {
#if defined(SHENMUE_HOST_PSP)
        if (traceTitle)
            psp::BootLog("gzip: compressed buffer allocation failed");
#endif
        return nullptr;
    }

    constexpr std::int32_t kScratchToken = SHEN_NAME(GZIP);
    void *const scratch = HLib_AllocMemBlock(0x4000, kScratchToken);
    if (!scratch) {
#if defined(SHENMUE_HOST_PSP)
        if (traceTitle)
            psp::BootLog("gzip: scratch allocation failed");
#endif
        HLib_FreeTaskData(static_cast<HLib::TaskData *>(compressed), 0);
        return nullptr;
    }

    const bool loaded = shenLoadFile("sprite", filename, compressed, static_cast<std::int32_t>(compressedSize)) != 0;
#if defined(SHENMUE_HOST_PSP)
    if (traceTitle)
        psp::BootLog("gzip: file read loaded=%d", loaded ? 1 : 0);
#endif
    if (loaded) {
#if defined(SHENMUE_HOST_PSP)
        if (traceTitle) {
            std::uint32_t compressedPrefix = 0;
            std::memcpy(&compressedPrefix, compressed, sizeof(compressedPrefix));
            psp::BootLog("gzip: compressed prefix=%08x", static_cast<unsigned>(compressedPrefix));
        }
#endif
        std::int32_t outputSize = 0;
        std::memcpy(&outputSize, static_cast<const std::byte *>(compressed) + compressedSize - sizeof(outputSize), sizeof(outputSize));
#if defined(SHENMUE_HOST_PSP)
        if (traceTitle) {
            const auto *const trailer = static_cast<const std::uint8_t *>(compressed) + compressedSize - 4;
            psp::BootLog("gzip: trailer bytes=%u %u %u %u", static_cast<unsigned>(trailer[0]), static_cast<unsigned>(trailer[1]),
                         static_cast<unsigned>(trailer[2]), static_cast<unsigned>(trailer[3]));
            psp::BootLog("gzip: output bytes=%u", static_cast<unsigned>(outputSize));
        }
#endif
        if (outputSize != 0) {
            void *const output = HLib_AllocHeapMemBlock(outputSize, allocationToken);
            if (output) {
                const bool decompressed = decompressGzip(compressed, output, static_cast<std::uint32_t>(outputSize), compressedSize, traceTitle);
#if defined(SHENMUE_HOST_PSP)
                if (traceTitle) {
                    std::uint32_t prefix = 0;
                    std::memcpy(&prefix, output, sizeof(prefix));
                    psp::BootLog("gzip: output after inflate prefix=%08x valid=%d", static_cast<unsigned>(prefix), decompressed ? 1 : 0);
                }
#endif
                if (decompressed) {
                    if (decompressedSize)
                        *decompressedSize = static_cast<std::uint32_t>(outputSize);
                    HLib_FreeTaskData(static_cast<HLib::TaskData *>(compressed), 0);
                    HLib_FreeTaskData(static_cast<HLib::TaskData *>(scratch), 0);
#if defined(SHENMUE_HOST_PSP)
                    if (traceTitle) {
                        std::uint32_t prefix = 0;
                        std::memcpy(&prefix, output, sizeof(prefix));
                        psp::BootLog("gzip: output after source release prefix=%08x", static_cast<unsigned>(prefix));
                    }
#endif
                    return output;
                }
                HLib_FreeTaskData(static_cast<HLib::TaskData *>(output), 0);
            }
#if defined(SHENMUE_HOST_PSP)
            if (traceTitle)
                psp::BootLog("gzip: output HLib allocation failed");
#endif
        }
#if defined(SHENMUE_HOST_PSP)
        else if (traceTitle)
            psp::BootLog("gzip: output size was zero");
#endif
    }

    HLib_FreeTaskData(static_cast<HLib::TaskData *>(scratch), 0);
    HLib_FreeTaskData(static_cast<HLib::TaskData *>(compressed), 0);
    return nullptr;
}

} // namespace shenmue::s1
