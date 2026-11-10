#include "s1/texture_loader.hpp"
#include "s1/d3t/namespace.hpp"

#include "s1/hlib.hpp"
#include "s1/d3t/d3t_texture.hpp"
#include "s1/pvr_decoder.hpp"
#include "s1/texture_cache.hpp"
#include "s1/probed_hash.hpp"
#include "s1/d3t/settings.hpp"
#include "s1/d3t/d3t_render_handles.hpp"
#include "d3t/shader_load.hpp"
#include "s1/archive_data.hpp"
#include "s1/d3t/d3t_content.hpp"
#include "s1/mip_streaming.hpp"
#include "s1/nuklear_integration.hpp"
#include "s1/d3t/model.hpp"
#include "s1/shader_initialization.hpp"
#include "s1/boot.hpp"
#include "s1/graphics_resources.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <json/json.h>

#if defined(SHENMUE_HOST_PSP)
#include "psp/boot_log.hpp"
#include "psp/texture_layout.hpp"
#include <pspthreadman.h>
#include <psputils.h>
#endif

namespace shenmue::s1 {

namespace {

constexpr std::uint32_t kTexnMagic = 0x4e584554;
constexpr std::uint32_t kGbixMagic = 0x58494247;
constexpr std::uint32_t kPvrtMagic = 0x54525650;

#pragma pack(push, 1)
struct PvrHeader {
    std::uint32_t magic;
    std::int32_t dataSize;
    std::uint8_t pixelFormat;
    std::uint8_t dataFormat;
    std::uint16_t reserved_0a;
    std::uint16_t width;
    std::uint16_t height;
};

struct TexnData {
    std::uint32_t magic;
    std::uint32_t reserved_04;
    std::int32_t textureIdLow;
    std::int32_t textureIdHigh;
    std::byte reserved_10[0x0c];
    PvrHeader header;
};

#pragma pack(pop)

static_assert(sizeof(PvrHeader) == 0x10);
static_assert(offsetof(TexnData, header) == 0x1c);

std::int32_t g_textureAllocationNumber = 0;

#if defined(SHENMUE_HOST_PSP)
// The largest decoded texture observed in the verified title TEXN set is 2 MiB.
// Keep that bounded scratch in BSS so it does not compete with the startup heap;
// LoadTEXN rejects any later texture that exceeds this proven bring-up budget.
constexpr std::size_t kPspDecodeScratchBytes = 2 * 1024 * 1024;
alignas(64) std::array<std::byte, kPspDecodeScratchBytes> g_pvrDecodeScratch;
SceUID g_pvrDecodeSemaphore = -1;

void ReleaseRetainedPspTexturePixels(void *pixels) {
    if (pixels)
        HLib_FreeTaskData(static_cast<HLib::TaskData *>(pixels), 0);
}

// Matching release for pixels that came from the newlib heap fallback below.
void ReleaseRetainedPspHeapPixels(void *pixels) {
    if (pixels)
        ::operator delete(pixels, std::align_val_t{32});
}
#endif

[[noreturn]] void unreachableNativePvrInput() {
#if defined(_MSC_VER)
    __assume(0);
#else
    __builtin_unreachable();
#endif
}

void *memcpyReverseBlocks(std::int32_t blockCount, std::int32_t elementCount, std::int32_t bitsPerElement, void *destination, const void *source) {
    void *result = destination;
    if (blockCount <= 0) {
        return result;
    }

    std::int32_t remaining = blockCount - 1;
    std::int32_t index = 0;
    const std::int32_t blockSize = ((bitsPerElement * elementCount / 8) + 3) & ~3;
    do {
        result = std::memcpy(static_cast<std::byte *>(destination) + index * blockSize, static_cast<const std::byte *>(source) + remaining * blockSize,
            static_cast<std::size_t>(blockSize));
        ++index;
        --remaining;
    } while (index < blockCount);
    return result;
}

} // namespace

bool InitTextureLoader() {
#if defined(SHENMUE_HOST_PSP)
    if (g_pvrDecodeSemaphore >= 0)
        return true;
    g_pvrDecodeSemaphore = sceKernelCreateSema("ShenmuePVRDecode", 0, 1, 1, nullptr);
    psp::BootLog("texture loader: decode semaphore=%d scratch=%p bytes=%u", static_cast<int>(g_pvrDecodeSemaphore),
                 static_cast<void *>(g_pvrDecodeScratch.data()), static_cast<unsigned>(g_pvrDecodeScratch.size()));
    return g_pvrDecodeSemaphore >= 0;
#else
    return true;
#endif
}

std::int32_t LoadTEXN(void *texnData, bool retainPspPixels, std::uint16_t maxPspDimension, PspTextureStorage pspStorage) {
    auto *const texn = static_cast<TexnData *>(texnData);
#if defined(SHENMUE_HOST_PSP)
    const bool traceTexture = g_textureAllocationNumber < 64;
    const bool reportTextureFailure = traceTexture || retainPspPixels;
    if (traceTexture)
        psp::BootLog("LoadTEXN: begin data=%p", texnData);
#endif
    if (texn->magic != kTexnMagic) {
#if defined(SHENMUE_HOST_PSP)
        if (reportTextureFailure)
            psp::BootLog("LoadTEXN: invalid TEXN magic=%08x", static_cast<unsigned>(texn->magic));
#endif
        return -1;
    }

    TextureUploadDescription upload{};
    PvrHeader *pvr = &texn->header;
    if (pvr->magic == kGbixMagic) {
        pvr = reinterpret_cast<PvrHeader *>(reinterpret_cast<std::byte *>(pvr) + pvr->dataSize + 8);
    }
    if (pvr->magic != kPvrtMagic) {
        --pvr;
    }
    if (pvr->magic != kPvrtMagic) {
        unreachableNativePvrInput();
    }

    const std::int32_t sourceWidth = pvr->width;
    const std::int32_t sourceHeight = pvr->height;
    if (sourceWidth <= 0 || sourceHeight <= 0 || sourceWidth > INT16_MAX || sourceHeight > INT16_MAX)
        return -1;

    std::int32_t bitsPerElement;
    if (pvr->pixelFormat <= 4) {
        bitsPerElement = 32;
    } else if (pvr->pixelFormat == 5) {
        bitsPerElement = 4;
    } else if (pvr->pixelFormat == 6) {
        bitsPerElement = 8;
    } else {
        unreachableNativePvrInput();
    }

    const std::int32_t elementCount = sourceWidth;
    const std::int32_t blockCount = sourceHeight;
    const std::int64_t decodedSize64 = static_cast<std::int64_t>(blockCount) * (((bitsPerElement * elementCount / 8) + 3) & ~3);
    if (decodedSize64 <= 0 || decodedSize64 > INT32_MAX)
        return -1;
    const std::int32_t decodedSize = static_cast<std::int32_t>(decodedSize64);
    bool scaleForPsp = false;
#if defined(SHENMUE_HOST_PSP)
    std::int32_t outputWidth = sourceWidth;
    std::int32_t outputHeight = sourceHeight;
    if (maxPspDimension != 0 && (sourceWidth > maxPspDimension || sourceHeight > maxPspDimension)) {
        if (bitsPerElement != 32)
            return -1;
        if (sourceWidth >= sourceHeight) {
            outputWidth = maxPspDimension;
            outputHeight = std::max(1, static_cast<std::int32_t>((static_cast<std::int64_t>(sourceHeight) * outputWidth + sourceWidth / 2) / sourceWidth));
        } else {
            outputHeight = maxPspDimension;
            outputWidth = std::max(1, static_cast<std::int32_t>((static_cast<std::int64_t>(sourceWidth) * outputHeight + sourceHeight / 2) / sourceHeight));
        }
        scaleForPsp = true;
    }
#else
    (void)retainPspPixels;
    (void)maxPspDimension;
    (void)pspStorage;
    const std::int32_t outputWidth = sourceWidth;
    const std::int32_t outputHeight = sourceHeight;
#endif
    upload.width = static_cast<std::int16_t>(outputWidth);
    upload.height = static_cast<std::int16_t>(outputHeight);
#if defined(SHENMUE_HOST_PSP)
    const bool native16 = pspStorage == PspTextureStorage::Swizzled16 && bitsPerElement == 32 && pvr->pixelFormat <= 2;
    const std::size_t pixelBytes = native16 ? 2U : 4U;
    const std::uint32_t backendFormat = native16 ? (pvr->pixelFormat == 0 ? ::shenmue::d3t::kPspTexture5551 :
        pvr->pixelFormat == 1 ? ::shenmue::d3t::kPspTexture5650 : ::shenmue::d3t::kPspTexture4444) : 2U;
    const bool swizzled = native16 && outputWidth >= 8 && outputHeight >= 8 && outputWidth % 8 == 0 && outputHeight % 8 == 0;
    const std::size_t outputBytes = (scaleForPsp || native16) ? static_cast<std::size_t>(outputWidth) * outputHeight * pixelBytes : decodedSize;
#else
    const std::size_t outputBytes = decodedSize;
#endif
#if defined(SHENMUE_HOST_PSP)
    if (traceTexture)
        psp::BootLog("LoadTEXN: ids=%08x:%08x pvr=%p format=%u data=%u width=%d height=%d bits=%d decoded=%d",
                     static_cast<unsigned>(texn->textureIdHigh), static_cast<unsigned>(texn->textureIdLow), static_cast<void *>(pvr),
                     static_cast<unsigned>(pvr->pixelFormat), static_cast<unsigned>(pvr->dataFormat), elementCount, blockCount,
                     bitsPerElement, decodedSize);
#endif
    if (decodedSize <= 0 || blockCount <= 0 || elementCount <= 0) {
#if defined(SHENMUE_HOST_PSP)
        if (reportTextureFailure)
            psp::BootLog("LoadTEXN: invalid decoded size");
#endif
        return -1;
    }

    const std::int32_t allocationNumber = g_textureAllocationNumber;
    const std::uint32_t tens = static_cast<std::uint32_t>(allocationNumber / 10 + '0');
    const std::uint32_t units = static_cast<std::uint32_t>(allocationNumber % 10 + '0');
    const std::int32_t token = static_cast<std::int32_t>(((tens | (units << 8)) << 16) | 0x5854u);

#if defined(SHENMUE_HOST_PSP)
    if (static_cast<std::size_t>(decodedSize) > g_pvrDecodeScratch.size()) {
        if (reportTextureFailure)
            psp::BootLog("LoadTEXN: decode request exceeds PSP scratch bytes=%d capacity=%u", decodedSize,
                         static_cast<unsigned>(g_pvrDecodeScratch.size()));
        return -1;
    }
    if (!InitTextureLoader()) {
        if (reportTextureFailure)
            psp::BootLog("LoadTEXN: PVR decode semaphore unavailable");
        return -1;
    }
    const std::int32_t waitResult = sceKernelWaitSema(g_pvrDecodeSemaphore, 1, nullptr);
    if (waitResult < 0) {
        if (reportTextureFailure)
            psp::BootLog("LoadTEXN: PVR decode semaphore wait failed result=%d", waitResult);
        return -1;
    }
    void *const decoded = g_pvrDecodeScratch.data();
#else
    void *const decoded = std::malloc(static_cast<std::size_t>(decodedSize));
    if (!decoded) {
        return -1;
    }
#endif

#if defined(SHENMUE_HOST_PSP)
    if (traceTexture)
        psp::BootLog("LoadTEXN: decoder begin dst=%p", decoded);
#endif
    PVR_DecodeMain(decoded, pvr);
#if defined(SHENMUE_HOST_PSP)
    if (traceTexture)
        psp::BootLog("LoadTEXN: decoder returned");
#endif

#if defined(SHENMUE_HOST_PSP)
    const std::size_t allocationBytes = outputBytes;
#else
    const std::size_t allocationBytes = 2ULL * outputBytes;
#endif
    bool pixelsFromPspHeap = false;
    void *pixels = HLib_AllocHeapMemBlock(static_cast<std::int64_t>(allocationBytes), token);
#if defined(SHENMUE_HOST_PSP)
    if (!pixels) {
        // The HLib arena can be full while the newlib heap still has sbrk room.
        // The frontend's 33 retained title textures exhaust the arena at texture
        // index 27, and losing one texture discards the whole set.
        pixels = ::operator new(allocationBytes, std::align_val_t{32}, std::nothrow);
        pixelsFromPspHeap = pixels != nullptr;
        if (pixelsFromPspHeap)
            psp::BootLog("LoadTEXN: arena full, pixels from heap bytes=%u", static_cast<unsigned>(allocationBytes));
    }
#endif
    const auto releasePixels = [pixelsFromPspHeap](void *block) {
        if (!block)
            return;
        if (pixelsFromPspHeap)
            ::operator delete(block, std::align_val_t{32});
        else
            HLib_FreeTaskData(static_cast<HLib::TaskData *>(block), 0);
    };
    if (!pixels) {
#if defined(SHENMUE_HOST_PSP)
        sceKernelSignalSema(g_pvrDecodeSemaphore, 1);
#else
        std::free(decoded);
#endif
#if defined(SHENMUE_HOST_PSP)
        if (reportTextureFailure) {
            std::uint64_t largestArenaBlock = 0;
            const std::uint64_t arenaFree = HLib_GetArenaFreeBytes(&largestArenaBlock);
            psp::BootLog("LoadTEXN: pixel allocation failed bytes=%u arenaFree=%uKB arenaMax=%uKB", static_cast<unsigned>(allocationBytes),
                         static_cast<unsigned>(arenaFree / 1024), static_cast<unsigned>(largestArenaBlock / 1024));
        }
#endif
        return -1;
    }

#if defined(SHENMUE_HOST_PSP)
    if (scaleForPsp || native16) {
        const auto *const source = static_cast<const std::byte *>(decoded);
        auto *const destination = static_cast<std::byte *>(pixels);
        for (std::int32_t row = 0; row < outputHeight; ++row) {
            const std::int32_t sourceY = static_cast<std::int32_t>((static_cast<std::int64_t>(row) * sourceHeight) / outputHeight);
            for (std::int32_t column = 0; column < outputWidth; ++column) {
                const std::int32_t sourceX = static_cast<std::int32_t>((static_cast<std::int64_t>(column) * sourceWidth) / outputWidth);
                const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4U;
                const std::size_t destinationOffset = swizzled
                    ? psp::SwizzledPixelOffset(outputWidth, column, row, pixelBytes)
                    : (static_cast<std::size_t>(row) * outputWidth + column) * pixelBytes;
                if (native16) {
                    const auto *const rgba = reinterpret_cast<const std::uint8_t *>(source + sourceOffset);
                    const std::uint16_t pixel = psp::PackPvrPixel16(rgba[0], rgba[1], rgba[2], rgba[3], pvr->pixelFormat);
                    std::memcpy(destination + destinationOffset, &pixel, sizeof(pixel));
                } else {
                    std::memcpy(destination + destinationOffset, source + sourceOffset, 4);
                }
            }
        }
    } else {
        std::memcpy(pixels, decoded, static_cast<std::size_t>(decodedSize));
        // PSP GU texture coordinates use the decoded PVR row order directly.
        // The PC upload path flips rows for its Direct3D convention; carrying
        // that flip into GU turns all frontend lettering upside down.
    }
    sceKernelSignalSema(g_pvrDecodeSemaphore, 1);
#else
    memcpyReverseBlocks(blockCount, elementCount, bitsPerElement, pixels, decoded);
    std::free(decoded);
#endif

    ++g_textureAllocationNumber;

    upload.format = 33160 + (bitsPerElement == 32 ? 1 : 0);
    if (bitsPerElement == 24) {
        upload.format = 33160;
    }
    // Only used as a truthiness check by d3t_UploadTexture2DToGPU; a heap-backed
    // buffer has no arena offset, so report a non-zero placeholder.
    upload.arenaOffset = pixelsFromPspHeap
        ? -1
        : static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(pixels) - reinterpret_cast<std::uintptr_t>(g_pFirstTaskData) - 0x40);

    std::int32_t surface = PVR_FindOrAllocateSurface(texn->textureIdLow, texn->textureIdHigh);
#if defined(SHENMUE_HOST_PSP)
    if (traceTexture)
        psp::BootLog("LoadTEXN: pixels=%p uploadBytes=%d arenaOffset=%d surface=%d", pixels, decodedSize, upload.arenaOffset, surface);
#endif
    if (surface < 0) {
#if defined(SHENMUE_HOST_PSP)
        if (reportTextureFailure)
            psp::BootLog("LoadTEXN: no PVR surface available");
#endif
        releasePixels(pixels);
        return -1;
    }
    const std::int32_t uploadResult = d3t_UploadTexture2DToGPU(surface, &upload, pixels);
#if defined(SHENMUE_HOST_PSP)
    if (traceTexture)
        psp::BootLog("LoadTEXN: upload result=%d surface=%d", uploadResult, surface);
#endif
    if (uploadResult == -1) {
#if defined(SHENMUE_HOST_PSP)
        if (reportTextureFailure)
            psp::BootLog("LoadTEXN: releasing failed upload surface=%d", surface);
#endif
        PVR_FreeAllocatedSurfaceByIndex(surface);
        surface = -1;
    }
#if defined(SHENMUE_HOST_PSP)
    bool retained = false;
    bool pixelsFreed = false;
    if (surface >= 0 && retainPspPixels) {
        const std::uint32_t textureHandle = GetPVRSurfaceTextureHandle(surface);
        if (textureHandle == 0) {
            psp::BootLog("LoadTEXN: no texture handle for retained surface=%d", surface);
            PVR_FreeAllocatedSurfaceByIndex(surface);
            surface = -1;
        } else {
            D3TTextureBackendView existing{};
            if (GetTexture2DBackendView(textureHandle, &existing) && existing.texture) {
                retained = true;
                releasePixels(pixels);
                pixelsFreed = true;
            } else {
                sceKernelDcacheWritebackRange(pixels, static_cast<unsigned>(outputBytes));
                retained = ::shenmue::d3t::AttachTexture2DBackendStorage(
                    textureHandle, pixels, static_cast<std::uint32_t>(outputWidth), static_cast<std::uint32_t>(outputHeight), backendFormat,
                    pixelsFromPspHeap ? ReleaseRetainedPspHeapPixels : ReleaseRetainedPspTexturePixels, pixels, swizzled);
                if (!retained) {
                    psp::BootLog("LoadTEXN: failed to retain PSP texture pixels handle=%08x size=%u", static_cast<unsigned>(textureHandle),
                                 static_cast<unsigned>(outputBytes));
                    PVR_FreeAllocatedSurfaceByIndex(surface);
                    surface = -1;
                }
            }
        }
    }
    if (surface >= 0 && retainPspPixels && !retained) {
        psp::BootLog("LoadTEXN: retained surface has no PSP texture storage surface=%d", surface);
        PVR_FreeAllocatedSurfaceByIndex(surface);
        surface = -1;
    }
    if (!retained && !pixelsFreed)
        releasePixels(pixels);
#else
    HLib_FreeTaskData(static_cast<HLib::TaskData *>(pixels), 0);
#endif
#if defined(SHENMUE_HOST_PSP)
    if (traceTexture)
        psp::BootLog("LoadTEXN: complete surface=%d output=%dx%d bytes=%u retained=%d", surface, outputWidth, outputHeight,
                     static_cast<unsigned>(outputBytes), retained ? 1 : 0);
#endif
    return surface;
}

std::array<TextureLoadRequest, 0x2000> g_textureLoadRequests{};

namespace {



struct SDTextureOverrideMapping {
    char source[260];
    char destination[260];
};
static_assert(sizeof(SDTextureOverrideMapping) == 520);



struct TextureOverrideEntry {
    std::uint32_t loadRequest;
    std::uint32_t textureHandle;
    std::uint32_t renderHandle;
    std::uint32_t state;
    std::uint32_t flags;
    char path[280];
};
static_assert(sizeof(TextureOverrideEntry) == 300);


struct TextureOverrideTable {
    std::array<TextureOverrideEntry, 0x100> entries;
    std::array<std::uint32_t, 0x100> hashes;
    std::array<std::uint16_t, 0x100> probes;
    std::uint32_t count;
};

std::vector<SDTextureOverrideMapping> g_sdTextureOverrideMappings;
std::atomic<std::uint16_t> g_textureLoadRequestIndex{0};
TextureOverrideTable g_textureOverrides{};

std::uint8_t g_textureOverridesPending = 0;

} // namespace
namespace d3t {
namespace {



void ParseStandardDefinitionRemappingJson(void *, std::uint32_t, void *loadedData, std::uint64_t loadedSize, const char *) {
    if (!loadedData || !loadedSize)
        return;
    const char *const text = static_cast<const char *>(loadedData);
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    if (!reader->parse(text, text + loadedSize, &root, nullptr) || !root.isObject())
        return;
    const Json::Value *mappings = nullptr;
    for (const std::string &name : root.getMemberNames()) {
        if (D3T_SettingsKeyMatches(name.c_str(), "Mappings")) {
            mappings = &root[name];
            break;
        }
    }
    if (!mappings || !mappings->isArray())
        return;
    g_sdTextureOverrideMappings.assign(mappings->size(), {});
    for (Json::ArrayIndex index = 0; index < mappings->size(); ++index) {
        const Json::Value &mapping = (*mappings)[index];
        if (!mapping.isObject())
            continue;
        const Json::Value *source = nullptr;
        const Json::Value *destination = nullptr;
        for (const std::string &name : mapping.getMemberNames()) {
            if (!source && D3T_SettingsKeyMatches(name.c_str(), "source"))
                source = &mapping[name];
            if (!destination && D3T_SettingsKeyMatches(name.c_str(), "destination"))
                destination = &mapping[name];
        }
        if (!source || !destination || !source->isString() || !destination->isString())
            continue;
        SDTextureOverrideMapping &entry = g_sdTextureOverrideMappings[index];
        std::snprintf(entry.source, sizeof(entry.source), "%s", source->asCString());
        FilepathCleaner(entry.source);
        for (const char *const marker : {".chrm_", ".mt5_", ".mapm_", ".mapd_"}) {
            if (char *const found = std::strstr(entry.source, marker)) {
                const std::size_t markerLength = std::strlen(marker);
                std::memmove(found, found + markerLength, std::strlen(found + markerLength) + 1);
            }
        }
        std::snprintf(entry.destination, sizeof(entry.destination), "%s", destination->asCString());
        FilepathCleaner(entry.destination);
        const std::size_t length = std::strlen(entry.destination);
        if (length > 4 && std::strcmp(entry.destination + length - 4, ".png") == 0)
            std::strcat(entry.destination, "?usage=0");
    }
}


void ParseTextureLoad(void *data, std::uint32_t, void *loadedData, std::uint64_t loadedSize, const char *) {
    TextureLoadRequest &request = *static_cast<TextureLoadRequest *>(data);
    const Texture2DDescription empty{};
    std::uint32_t texture = CreateTexture2D(&empty);
    if (texture && !ReplaceWithDecodedTexture(texture, loadedData, loadedSize, request.linearColour != 0)) {
        InitTexture2DResource(texture);
        texture = 0;
    }
    request.texture = texture;
}
} // namespace
} // namespace d3t



std::uint32_t LoadTextureFromFilepath(const char *path, std::uint32_t linearColour, std::uint32_t renderHandle) {
    const std::uint16_t index = g_textureLoadRequestIndex.fetch_add(1, std::memory_order_seq_cst) & 0x1fffU;
    TextureLoadRequest &request = g_textureLoadRequests[index];
    const std::uint16_t generation = ++request.generation;
    if (generation) {
        request.texture = 0;
        request.linearColour = linearColour;
        const D3TShaderLoadTask task{path, renderHandle, 0, {d3t::ParseTextureLoad, &request}, nullptr, 0, 0, {}};
        PushShaderLoadToThreadQueue(&task);
    }
    return (static_cast<std::uint32_t>(generation) << 16U) | index;
}



std::uint32_t LoadTextureFromFilepathWithUsage(const char *path) {
    std::array<char, 256> request{};
    if (std::strchr(path, '?'))
        std::snprintf(request.data(), request.size(), "%s", path);
    else
        std::snprintf(request.data(), request.size(), "%s%s", path, "?usage=0");
    return LoadTextureFromFilepath(request.data(), 0, 0);
}

namespace {

TextureOverrideEntry *FindTextureOverride(std::uint32_t textureHandle) {
    std::uint32_t slot = textureHandle & 0xfffU;
    while (g_textureOverrides.hashes[slot] != textureHandle) {
        const std::uint16_t distance = g_textureOverrides.probes[slot];
        if (!distance)
            return nullptr;
        slot = (slot + distance) & 0xfffU;
    }
    return &g_textureOverrides.entries[slot];
}



bool UpdateTextureStreamStateFromRenderHandle(TextureOverrideEntry &entry) {
    if (entry.textureHandle) {
        d3t_CloseRenderHandle(entry.renderHandle);
        return true;
    }
    if (entry.state || d3t_IsRenderHandlePending(entry.renderHandle))
        return false;
    const TextureLoadRequest &request = g_textureLoadRequests[static_cast<std::uint16_t>(entry.loadRequest) & 0x1fffU];
    entry.textureHandle = request.generation == (entry.loadRequest >> 16U) ? request.texture : 0;
    d3t_CloseRenderHandle(entry.renderHandle);
    entry.loadRequest = 0;
    entry.state = entry.textureHandle ? 1 : 2;
    return entry.state == 1;
}

} // namespace
namespace d3t {
namespace {


void LocalizeTextureOverridePath(char *output, const char *destination) {
    std::strcpy(output, destination);
    char localized[272];
    std::strcpy(localized, destination);
    std::size_t end = std::strlen(localized);
    while (end > 1 && localized[end - 1] != '.' && localized[end - 1] != '/')
        --end;
    if (localized[end - 1] != '.')
        return;
    char extension[272];
    std::strcpy(extension, localized + end - 1);
    localized[end - 1] = '\0';
    if (g_uiLoadedLanguage == -1) {
        if (g_uiStringTable) {
            g_uiStringTable = nullptr;
            std::free(g_uiStringBuffer);
            g_uiStringBufferSize = 0;
        }
        g_uiLoadedLanguage = English;
        ReadSubtitleGlyphsAndSubs(English);
    }
    switch (g_uiLoadedLanguage) {
    case Japanese:
        std::strcat(localized, "_jp");
        break;
    case French:
        std::strcat(localized, "_fr");
        break;
    case German:
        std::strcat(localized, "_de");
        break;
    case Korean:
        std::strcat(localized, "_kr");
        break;
    case ChineseTrad:
        std::strcat(localized, "_tc");
        break;
    case ChineseSimp:
        std::strcat(localized, "_sc");
        break;
    default:
        break;
    }
    std::strcat(localized, extension);
    if (d3t::CheckForFile(localized))
        std::strcpy(output, localized);
}


void RegisterTextureOverride(const char *path, std::uint32_t textureHandle) {
    if (FindTextureOverride(textureHandle))
        return;
    TextureOverrideEntry entry{};
    char loadPath[512]{'/'};
    std::strcat(loadPath, path);
    for (char *cursor = loadPath; *cursor; ++cursor)
        *cursor = static_cast<char>(std::tolower(static_cast<unsigned char>(*cursor)));
    entry.renderHandle = d3t_AllocateRenderHandle();
    entry.loadRequest = s1::LoadTextureFromFilepath(loadPath, 1, entry.renderHandle);
    std::snprintf(entry.path, sizeof(entry.path), "%s", loadPath);
    InsertProbedHash(g_textureOverrides, 0xfffU, 0x1000U, textureHandle, [&entry](std::uint32_t slot) { g_textureOverrides.entries[slot] = entry; });
    g_textureOverridesPending = 1;
}
} // namespace
} // namespace d3t
namespace {} // namespace


void SDTextureOverride() {
    const D3TShaderLoadTask task{
        "/textureOverride/SDTextureOverride.json", g_activeRenderHandle, 0, {d3t::ParseStandardDefinitionRemappingJson, nullptr}, nullptr, 0, 0, {}};
    PushShaderLoadToThreadQueue(&task);
}

namespace d3t {



void CheckAndRemapIfNeeded(const char *path, std::int32_t ordinal, std::uint32_t textureHandle) {
    char cleaned[272];
    std::strcpy(cleaned, path);
    FilepathCleaner(cleaned);
    const std::size_t length = std::strlen(cleaned);
    const char *name = cleaned;
    for (std::size_t end = length; end > 1; --end) {
        if (cleaned[end - 1] == '/' || cleaned[end - 1] == '\\') {
            name = cleaned + end;
            break;
        }
    }
    std::size_t extensionEnd = length;
    while (extensionEnd > 1 && cleaned[extensionEnd - 1] != '.' && cleaned[extensionEnd - 1] != '/')
        --extensionEnd;
    const std::size_t extensionLength = cleaned[extensionEnd - 1] == '.' ? length - extensionEnd + 1 : 0;
    char key[272];
    if (d3t::CheckForFile(cleaned)) {
        std::strcpy(key, cleaned);
    } else {
        char directory[272];
        std::strcpy(directory, cleaned);
        directory[length - std::strlen(name)] = '\0';
        char stem[272];
        std::strcpy(stem, name);
        stem[std::strlen(stem) - extensionLength] = '\0';
        if (char *const found = std::strstr(directory, stem))
            *found = '\0';
        std::snprintf(key, 0x103, directory[0] == '/' ? "%s%s/%s_%i.dds" : "/%s%s/%s_%i.dds", directory, stem, stem, ordinal);
        if (char *const marker = std::strstr(key, ".xxx_"))
            std::memmove(marker, marker + 5, std::strlen(marker + 5) + 1);
    }
    for (const SDTextureOverrideMapping &mapping : g_sdTextureOverrideMappings) {
        if (std::strcmp(mapping.source, key) != 0)
            continue;
        char destination[272];
        d3t::LocalizeTextureOverridePath(destination, mapping.destination);
        d3t::RegisterTextureOverride(destination, textureHandle);
    }
}
} // namespace d3t



std::uint32_t ResolveStripTextureHandle(std::uint32_t textureHandle) {
    TextureOverrideEntry *const entry = FindTextureOverride(textureHandle);
    if (!entry)
        return textureHandle;
    if (static_cast<std::uint32_t>(g_perspectiveProjection - 1) > 2U && (entry->flags & 1U) != 0U)
        return textureHandle;
    if (entry->state == 1 || (entry->state == 0 && UpdateTextureStreamStateFromRenderHandle(*entry) && entry->state == 1))
        return entry->textureHandle;
    return d3t::g_defaultWhiteTexture;
}

namespace d3t {


std::uint8_t PollTextureOverrides() {
    if (!g_textureOverridesPending)
        return 0;
    bool settled = true;
    for (std::uint32_t slot = 0; slot < 0x1000; ++slot) {
        TextureOverrideEntry &entry = g_textureOverrides.entries[slot];
        if (!g_textureOverrides.hashes[slot] || entry.state)
            continue;
        UpdateTextureStreamStateFromRenderHandle(entry);
        settled = settled && entry.state != 0;
    }
    if (settled)
        g_textureOverridesPending = 0;
    return g_textureOverridesPending;
}
} // namespace d3t


void InitTextureSpace() {
    for (std::uint32_t slot = 0; slot < 0x1000; ++slot) {
        const std::uint32_t hash = g_textureOverrides.hashes[slot];
        if (!hash || g_textureOverrides.entries[slot].state != 1)
            continue;
        InitTexture2DResource(g_textureOverrides.entries[slot].textureHandle);
        RemoveProbedHash(g_textureOverrides, 0xfffU, hash, [](std::uint32_t removed) { g_textureOverrides.entries[removed] = {}; });
    }
}

} // namespace shenmue::s1
