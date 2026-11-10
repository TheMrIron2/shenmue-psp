#include "psp/scene_renderer.hpp"
#include "psp/boot_log.hpp"
#include "d3t/texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#if defined(SHENMUE_HOST_PSP)
#include <pspgu.h>
#include <psputils.h>
#endif

namespace shenmue::psp {
namespace {
struct Vertex {
    float u, v;
    std::uint32_t colour;
    float x, y, z;
};
struct Draw {
    std::uint32_t first, count;
    alignas(16) Matrix4x4 modelView, projection;
    SceneMaterial material;
    bool widescreen;
};
constexpr std::size_t kVertexCapacity = 32768;
constexpr std::size_t kDrawCapacity = 512;
alignas(64) std::array<Vertex, kVertexCapacity> g_vertices;
std::array<Draw, kDrawCapacity> g_draws;
std::uint32_t g_vertexCount{}, g_drawCount{};
#if defined(SHENMUE_HOST_PSP)
std::uint32_t g_loggedFrames{};
#endif

std::uint32_t Colour(const Vector4 &colour, float opacity) {
    const auto byte = [](float value) { return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return byte(colour.x) | (byte(colour.y) << 8U) | (byte(colour.z) << 16U) | (byte(colour.w * opacity) << 24U);
}
}

bool QueueSceneTriangles(const Matrix4x4 *vertices, std::uint32_t count, const Matrix4x4 &modelView,
                         const Matrix4x4 &projection, const SceneMaterial &material, bool widescreen) {
    if (!vertices || !count || count % 3 || count > kVertexCapacity - g_vertexCount || g_drawCount == kDrawCapacity)
        return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const Matrix4x4 &source = vertices[i];
        g_vertices[g_vertexCount + i] = {source.rows[1].z, source.rows[1].w, Colour(source.rows[2], material.opacity),
                                        source.rows[0].x, source.rows[0].y, source.rows[0].z};
    }
    Draw &draw = g_draws[g_drawCount++];
    draw = {g_vertexCount, count, modelView, projection, material, widescreen};
    // Convert the engine's reversed 0..1 depth projection to GE's -1..1 clip convention.
    for (unsigned row = 0; row < 4; ++row)
        draw.projection.rows[row].z = projection.rows[row].w - 2.0f * projection.rows[row].z;
    g_vertexCount += count;
    return true;
}

void ResetSceneQueue() {
    g_vertexCount = g_drawCount = 0;
}

void RenderQueuedScene() {
#if defined(SHENMUE_HOST_PSP)
    if (!g_drawCount)
        return;
    sceKernelDcacheWritebackRange(g_vertices.data(), g_vertexCount * sizeof(Vertex));
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    // Preserve the visible portions of triangles crossing the near plane.
    sceGuEnable(GU_CLIP_PLANES);
    sceGuDisable(GU_CULL_FACE);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexScale(1, 1);
    sceGuTexOffset(0, 0);
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (std::uint32_t index = 0; index < g_drawCount; ++index) {
            const Draw &draw = g_draws[index];
            if (draw.material.blend != (pass != 0))
                continue;
            const int width = draw.widescreen ? 480 : 362;
            sceGuScissor((480 - width) / 2, 0, width, 272);
            sceGuViewport(2048, 2048, width, 272);
            sceGuSetMatrix(GU_PROJECTION, reinterpret_cast<const ScePspFMatrix4 *>(&draw.projection));
            sceGuSetMatrix(GU_VIEW, reinterpret_cast<const ScePspFMatrix4 *>(&draw.modelView));
            alignas(16) static const Matrix4x4 identity{{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}}};
            sceGuSetMatrix(GU_MODEL, reinterpret_cast<const ScePspFMatrix4 *>(&identity));
            sceGuDepthMask(draw.material.blend ? GU_TRUE : GU_FALSE);
            if (draw.material.blend) {
                sceGuEnable(GU_BLEND);
                sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
            } else sceGuDisable(GU_BLEND);
            if (draw.material.alphaTest) {
                sceGuEnable(GU_ALPHA_TEST);
                sceGuAlphaFunc(GU_GREATER, static_cast<int>(std::clamp(draw.material.alphaReference, 0.0f, 1.0f) * 255), 255);
            } else sceGuDisable(GU_ALPHA_TEST);
            d3t::D3TTextureBackendView texture{};
            if (draw.material.texture && d3t::GetTexture2DBackendView(draw.material.texture, &texture) && texture.texture) {
                const int format = texture.format == d3t::kPspTexture5551 ? GU_PSM_5551 :
                    texture.format == d3t::kPspTexture5650 ? GU_PSM_5650 :
                    texture.format == d3t::kPspTexture4444 ? GU_PSM_4444 : GU_PSM_8888;
                sceGuTexMode(format, 0, 0, texture.swizzled ? GU_TRUE : GU_FALSE);
                sceGuTexImage(0, texture.width, texture.height, texture.width, texture.texture);
                sceGuEnable(GU_TEXTURE_2D);
            } else sceGuDisable(GU_TEXTURE_2D);
            sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                           draw.count, nullptr, g_vertices.data() + draw.first);
        }
    }
    if (g_loggedFrames++ < 8)
        BootLog("GU scene: draws=%u vertices=%u", g_drawCount, g_vertexCount);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CLIP_PLANES);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDepthMask(GU_FALSE);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuScissor(0, 0, 480, 272);
#endif
}

} // namespace shenmue::psp
