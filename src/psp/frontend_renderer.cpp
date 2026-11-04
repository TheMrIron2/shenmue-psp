#include "psp/frontend_renderer.hpp"

#include "psp/boot_log.hpp"
#include "psp/psp_renderer.hpp"
#include "s1/texture_loader.hpp"
#include "s1/texture_cache.hpp"
#include "s1/d3t/d3t_texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace shenmue::psp::frontend {
namespace {
struct Texture {
    std::int32_t surface{-1};
    std::uint32_t handle{};
};
std::array<Texture, 33> g_textures{};
}

void ReleaseTextures() {
    for (Texture &texture : g_textures) {
        if (texture.surface >= 0)
            s1::PVR_FreeAllocatedSurfaceByIndex(texture.surface);
        texture = {};
    }
}

bool LoadTextures(const std::byte *bytes, std::size_t size, std::int32_t *surfaces, std::size_t capacity) {
    ReleaseTextures();
    std::fill_n(surfaces, capacity, -1);
    std::size_t offset = 0;
    std::size_t index = 0;
    while (offset + 16 <= size && index < g_textures.size() && index < capacity) {
        std::uint32_t tag, chunkSize;
        std::memcpy(&tag, bytes + offset, 4);
        std::memcpy(&chunkSize, bytes + offset + 4, 4);
        if (tag != 0x4e584554U || chunkSize < 44 || chunkSize > size - offset)
            break;
        const std::int32_t surface = s1::LoadTEXN(const_cast<std::byte *>(bytes + offset), true, 512, s1::PspTextureStorage::Swizzled16);
        if (surface < 0) {
            BootLog("DC frontend: texture load failed index=%u offset=%u", static_cast<unsigned>(index), static_cast<unsigned>(offset));
            break;
        }
        g_textures[index] = {surface, s1::GetPVRSurfaceTextureHandle(surface)};
        surfaces[index++] = surface;
        offset += chunkSize;
    }
    if (index != g_textures.size()) {
        BootLog("DC frontend: incomplete archive textures=%u expected=%u", static_cast<unsigned>(index), static_cast<unsigned>(g_textures.size()));
        ReleaseTextures();
        std::fill_n(surfaces, capacity, -1);
        return false;
    }
    BootLog("DC frontend: retained title/menu archive textures=%u", static_cast<unsigned>(index));
    return true;
}

std::uint32_t TextureHandle(std::size_t index) {
    return index < g_textures.size() ? g_textures[index].handle : 0;
}

void Submit(const s1::dreamcast_frontend::DrawList &list, bool widescreen) {
    const float scaleY = 272.0f / 480.0f;
    const float scaleX = widescreen ? 480.0f / 640.0f : scaleY;
    const float left = (480.0f - 640.0f * scaleX) * 0.5f;
    const int clipLeft = static_cast<int>(std::ceil(left));
    const int clipRight = static_cast<int>(std::floor(left + 640.0f * scaleX));
    SetFrameClearViewport(clipLeft, 0, clipRight, 272);
    for (std::size_t index = 0; index < list.count; ++index) {
        const auto &sprite = list.sprites[index];
        const float radians = sprite.angle * (6.283185307179586f / 65536.0f);
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        const float halfWidth = sprite.layout.width * sprite.scale * 0.5f;
        const float halfHeight = sprite.layout.height * sprite.scale * 0.5f;
        std::array<TextureQuadPoint, 4> points{};
        for (std::size_t corner = 0; corner < points.size(); ++corner) {
            const float x = (corner & 1U) ? halfWidth : -halfWidth;
            const float y = (corner & 2U) ? halfHeight : -halfHeight;
            points[corner] = {left + (sprite.layout.centerX + x * cosine - y * sine) * scaleX,
                              (sprite.layout.centerY + x * sine + y * cosine) * scaleY,
                              (corner & 1U) ? 1.0f : 0.0f, (corner & 2U) ? 1.0f : 0.0f};
        }
        QueueTextureQuad(TextureHandle(sprite.texture), points, sprite.colour, sprite.layout.depth, clipLeft, 0, clipRight, 272);
    }
}

} // namespace shenmue::psp::frontend
