// Dreamcast asset backend for ArchiveData_*.
//
// The PC build serves assets from the D3T .tac/.tad archives. The Dreamcast
// disc instead ships a plain ISO9660 tree with the original file names
// (MODEL/…, SCENE/…, SPRITE/…, …), which is what the engine already requests.
// This backend indexes either an extracted disc tree or a packed image and
// serves bytes straight from disk.
//
// Selected at build time with -DSHENMUE_USE_DC_ASSETS=1 (see Makefile.psp).

#include "common/archive_data.hpp"
#if defined(SHENMUE_HOST_PSP)
#include "psp/boot_log.hpp"
#endif

#include <shendk/utils/path_hash.h>
#include <shendk/utils/murmurhash2.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#if defined(SHENMUE_HOST_PSP)
#include <pspiofilemgr.h>
#endif

#if defined(SHENMUE_HOST_PSP)
#include <pspdebug.h>
#define SHEN_PSP_LOG(...) ::shenmue::psp::BootLog(__VA_ARGS__)
#else
#define SHEN_PSP_LOG(...) ((void)0)
#endif

namespace shenmue::archive_data {
namespace {

namespace fs = std::filesystem;

struct DcFile {
    std::string path;
    std::uint64_t size;
    std::uint32_t index;
};

std::mutex g_mutex;
std::deque<DcFile> g_files;

std::string_view withoutLeadingSlashes(std::string_view path) noexcept {
    while (!path.empty() && (path.front() == '/' || path.front() == '\\'))
        path.remove_prefix(1);
    return path;
}

char normalizedPathCharacter(std::string_view path, std::size_t index) noexcept {
    path = withoutLeadingSlashes(path);
    char value = path[index] == '\\' ? '/' : path[index];
    value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    if (index + 1 == path.size() && path.size() >= 4 && path[path.size() - 4] == '.' &&
        static_cast<char>(std::tolower(static_cast<unsigned char>(path[path.size() - 3]))) == 'm' &&
        static_cast<char>(std::tolower(static_cast<unsigned char>(path[path.size() - 2]))) == 't' &&
        static_cast<char>(std::tolower(static_cast<unsigned char>(path[path.size() - 1]))) == '3') {
        value = '5';
    }
    return value;
}

struct NormalizedPathHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view path) const noexcept {
        path = withoutLeadingSlashes(path);
        std::size_t hash = 2166136261U;
        for (std::size_t index = 0; index < path.size(); ++index) {
            hash ^= static_cast<unsigned char>(normalizedPathCharacter(path, index));
            hash *= 16777619U;
        }
        return hash;
    }
};

struct NormalizedPathEqual {
    using is_transparent = void;

    bool operator()(std::string_view left, std::string_view right) const noexcept {
        left = withoutLeadingSlashes(left);
        right = withoutLeadingSlashes(right);
        if (left.size() != right.size())
            return false;
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (normalizedPathCharacter(left, index) != normalizedPathCharacter(right, index))
                return false;
        }
        return true;
    }
};

std::unordered_map<std::string, DcFile *, NormalizedPathHash, NormalizedPathEqual> g_byKey;

// Case-insensitive, slash-normalised key. The engine requests ".MT3" while the
// disc stores ".MT5", so normalise that too.
std::string normalizeKey(const char *input) {
    std::string key;
    if (!input) {
        return key;
    }
    const char *cursor = input;
    while (*cursor == '/' || *cursor == '\\') {
        ++cursor;
    }
    for (; *cursor; ++cursor) {
        char value = *cursor == '\\' ? '/' : *cursor;
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
    }
    if (key.size() >= 4 && key.compare(key.size() - 4, 4, ".mt3") == 0) {
        key.back() = '5';
    }
    return key;
}

DcFile *lookup(const char *path) {
    if (g_files.empty() || !path) {
        return nullptr;
    }
    const auto found = g_byKey.find(std::string_view(path));
    return found == g_byKey.end() ? nullptr : found->second;
}

// Flat manifest written by tools/dc/extract.py: "<size>\\t<relative path>".
// Loading this avoids a recursive stat storm over PSPLink's USB host filesystem.
bool loadManifest(const fs::path &root) {
    std::ifstream input(root / "manifest.txt");
    if (!input) {
        return false;
    }
    std::string line;
    std::uint32_t index = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::uint64_t size = std::strtoull(line.c_str(), nullptr, 10);
        const std::string relative = line.substr(tab + 1);
        g_files.push_back(DcFile{(root / relative).string(), size, index++});
        g_byKey.emplace(normalizeKey(relative.c_str()), &g_files.back());
    }
    return !g_files.empty();
}

} // namespace

std::uint32_t d3t_HashData(const void *data, std::uint32_t size) {
    return shendk::HashData(data, size);
}

std::uint32_t d3t_GenerateHash(const char *path) {
    return path ? shendk::GeneratePathHash(path) : 0;
}

char *FilepathCleaner(char *path) {
    if (!path) {
        return nullptr;
    }
    const std::string cleaned = shendk::CleanFilePath(path);
    std::memcpy(path, cleaned.c_str(), cleaned.size() + 1);
    return path;
}

bool ArchiveData_LoadArchives(const char *root, const char *, std::uint32_t, ArchiveDataLoadResult *result) {
    std::scoped_lock lock(g_mutex);
    g_files.clear();
    g_byKey.clear();

    // Only manifest-based loading: never recursively walk a device filesystem
    // (PSPLink's host0: or a FAT stick), which can block for a very long time.
    const char *candidates[] = {
        root,
        std::getenv("SHEN_ROOT"),
        "host0:/DISC1/extracted",
        "ms0:/PSP/GAME/Shenmue/data",
        "ms0:/PSP/GAME/Shenmue",
    };
    bool loaded = false;
    for (const char *candidate : candidates) {
        if (!candidate || !*candidate) {
            continue;
        }
        if (loadManifest(fs::path(candidate))) {
            loaded = true;
            SHEN_PSP_LOG("dc assets: root=%s entries=%u", candidate, static_cast<unsigned>(g_files.size()));
            break;
        }
        SHEN_PSP_LOG("dc assets: no manifest at %s", candidate);
        g_files.clear();
        g_byKey.clear();
    }
    if (!loaded) {
        SHEN_PSP_LOG("dc assets: none found");
    }

    if (result) {
        result->archiveCount = g_files.empty() ? 0 : 1;
        result->entryCount = static_cast<std::uint32_t>(g_files.size());
        result->packedSize = 0;
    }
    return loaded;
}

void ArchiveData_ClearArchives() {
    std::scoped_lock lock(g_mutex);
    g_files.clear();
    g_byKey.clear();
}

bool ArchiveData_HasArchives() {
    std::scoped_lock lock(g_mutex);
    return !g_files.empty();
}

std::uint64_t ArchiveData_GetPathHash(const char *path) {
    return shendk::GetPathHash(normalizeKey(path));
}

std::uint64_t ArchiveData_GetFileSize(const char *path) {
    std::scoped_lock lock(g_mutex);
    const DcFile *const file = lookup(path);
    return file ? file->size : 0;
}

bool ArchiveData_IsPackedFile(const char *) {
    return false;
}

bool ArchiveData_GetScenarioOverridePath(const char *, char *, std::size_t) {
    return false;
}

void ArchiveData_GetSourceDesc(const char *path, ArchiveSourceDesc &source) {
    std::scoped_lock lock(g_mutex);
    DcFile *const file = lookup(path);
    if (!file) {
        source = EMPTY_ARCHIVE_SOURCE;
        return;
    }
    source = {};
    source.sourceIndex = file->index;
    source.sourceOffset = 0;
    source.fileSize = static_cast<std::uint32_t>(file->size);
    source.flags = 0;
    source.remapEntry = file;
}

bool ArchiveData_GetSourceBackingPath(const ArchiveSourceDesc &source, char *path, std::size_t capacity) {
    const auto *const file = static_cast<const DcFile *>(source.remapEntry);
    if (!file || file->path.size() >= capacity) {
        return false;
    }
    std::memcpy(path, file->path.c_str(), file->path.size() + 1);
    return true;
}

bool ArchiveData_Read(const char *path, std::uint64_t offset, void *destination, std::uint64_t size) {
    if (!destination) {
        return false;
    }
    std::scoped_lock lock(g_mutex);
    const DcFile *const file = lookup(path);
    if (!file) {
        return false;
    }
#if defined(SHENMUE_HOST_PSP)
    const SceUID handle = sceIoOpen(file->path.c_str(), PSP_O_RDONLY, 0);
    if (handle < 0) {
        SHEN_PSP_LOG("dc assets: read open failed path=%s result=%d", file->path.c_str(), static_cast<int>(handle));
        return false;
    }

    bool success = offset <= static_cast<std::uint64_t>(INT32_MAX);
    if (success) {
        const SceOff position = sceIoLseek(handle, static_cast<SceOff>(offset), PSP_SEEK_SET);
        success = position >= 0 && static_cast<std::uint64_t>(position) == offset;
    }

    auto *cursor = static_cast<std::byte *>(destination);
    std::uint64_t remaining = size;
    while (success && remaining != 0) {
        const SceSize request = static_cast<SceSize>(remaining > 0x100000U ? 0x100000U : remaining);
        const int bytesRead = sceIoRead(handle, cursor, request);
        if (bytesRead <= 0 || static_cast<SceSize>(bytesRead) > request) {
            success = false;
            break;
        }
        cursor += bytesRead;
        remaining -= static_cast<std::uint32_t>(bytesRead);
    }
    sceIoClose(handle);
    if (!success)
        SHEN_PSP_LOG("dc assets: read failed path=%s offset=%llu size=%llu", file->path.c_str(),
                     static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size));
    return success;
#else
    std::ifstream input(file->path, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(static_cast<char *>(destination), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
#endif
}

} // namespace shenmue::archive_data
