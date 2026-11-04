#include "psp/psp_renderer.hpp"
#include "psp/boot_log.hpp"

#include "d3t/dx11_renderer.hpp"
#include "d3t/texture.hpp"
#include "s1/boot.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(SHENMUE_HOST_PSP)
#include <pspdisplay.h>
#include <pspgu.h>
#include <psputils.h>
#endif

namespace shenmue::psp {
namespace {

// A single synthetic device context stands in for the D3D11 device context so
// the engine's render-state bookkeeping (which writes into `renderState`) keeps
// working while nothing is rasterised.
d3t::D3TDeviceContext g_nullContext{};

// Scratch destination for mapped constant-buffer writes. The null backend never
// reads it back, but callers memcpy into the returned pointer, so it must be
// valid and large enough for the largest single upload.
alignas(64) std::byte g_constantScratch[0x10000];

std::uint32_t g_nextTextureHandle = 0;

#if defined(SHENMUE_HOST_PSP)
constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 272;
constexpr int kFramebufferWidth = 512;
constexpr std::size_t kDisplayListWords = 64 * 1024;
constexpr std::size_t kTexturedQuadCapacity = 256;
alignas(16) std::array<unsigned int, kDisplayListWords> g_displayList{};
void *g_drawBuffer = nullptr;
void *g_displayBuffer = nullptr;
void *g_depthBuffer = nullptr;
bool g_guInitialized = false;
std::uint32_t g_presentCount = 0;
std::uint32_t g_texturedPresentLogs = 0;
std::uint32_t g_texturedQueueLogs = 0;

struct TexturedQuad {
    const void *pixels;
    std::uint32_t textureWidth;
    std::uint32_t textureHeight;
    std::uint32_t textureFormat;
    std::array<TextureQuadPoint, 4> points;
    std::uint32_t color;
    float depth;
    std::size_t sequence;
    int clipLeft, clipTop, clipRight, clipBottom;
    bool swizzled;
};
std::array<TexturedQuad, kTexturedQuadCapacity> g_texturedQuads{};
std::size_t g_texturedQuadCount = 0;
std::uint32_t g_textureQueueWarnings = 0;
std::array<int, 4> g_clearViewport{0, 0, kScreenWidth, kScreenHeight};

struct TexturedVertex {
    float u;
    float v;
    unsigned int color;
    float x;
    float y;
    float z;
};

bool InitializeNativeGU() {
    if (g_guInitialized)
        return true;

    const int displayModeResult = sceDisplaySetMode(PSP_DISPLAY_MODE_LCD, kScreenWidth, kScreenHeight);
    if (displayModeResult < 0) {
        psp::BootLog("GU: sceDisplaySetMode failed result=%d", displayModeResult);
        return false;
    }

    const int initResult = sceGuInit();
    if (initResult < 0) {
        psp::BootLog("GU: sceGuInit failed result=%d", initResult);
        return false;
    }

    g_drawBuffer = guGetStaticVramBuffer(kFramebufferWidth, kScreenHeight, GU_PSM_8888);
    g_displayBuffer = guGetStaticVramBuffer(kFramebufferWidth, kScreenHeight, GU_PSM_8888);
    g_depthBuffer = guGetStaticVramBuffer(kFramebufferWidth, kScreenHeight, GU_PSM_4444);
    const std::uintptr_t drawOffset = reinterpret_cast<std::uintptr_t>(g_drawBuffer);
    const std::uintptr_t displayOffset = reinterpret_cast<std::uintptr_t>(g_displayBuffer);
    const std::uintptr_t depthOffset = reinterpret_cast<std::uintptr_t>(g_depthBuffer);
    const std::size_t colourBytes = static_cast<std::size_t>(kFramebufferWidth) * kScreenHeight * 4U;
    const std::size_t depthBytes = static_cast<std::size_t>(kFramebufferWidth) * kScreenHeight * 2U;
    const std::uintptr_t vramBytes = sceGeEdramGetSize();
    if (drawOffset + colourBytes > vramBytes || displayOffset + colourBytes > vramBytes || depthOffset + depthBytes > vramBytes) {
        psp::BootLog("GU: framebuffer allocation failed offsets=%08x,%08x,%08x edram=%08x", static_cast<unsigned>(drawOffset),
                     static_cast<unsigned>(displayOffset), static_cast<unsigned>(depthOffset), static_cast<unsigned>(vramBytes));
        return false;
    }

    const int startResult = sceGuStart(GU_DIRECT, g_displayList.data());
    if (startResult < 0) {
        psp::BootLog("GU: initial display list failed result=%d", startResult);
        return false;
    }
    sceGuDrawBuffer(GU_PSM_8888, g_drawBuffer, kFramebufferWidth);
    sceGuDispBuffer(kScreenWidth, kScreenHeight, g_displayBuffer, kFramebufferWidth);
    sceGuDepthBuffer(g_depthBuffer, kFramebufferWidth);
    sceGuOffset(2048 - (kScreenWidth / 2), 2048 - (kScreenHeight / 2));
    sceGuViewport(2048, 2048, kScreenWidth, kScreenHeight);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuFrontFace(GU_CW);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_SMOOTH);
    sceGuClearDepth(0);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    void *displayTop = nullptr;
    int displayWidth = 0;
    int displayFormat = -1;
    const int displayResult = sceDisplayGetFrameBuf(&displayTop, &displayWidth, &displayFormat, PSP_DISPLAY_SETBUF_IMMEDIATE);
    int activeMode = -1;
    int activeWidth = 0;
    int activeHeight = 0;
    const int activeModeResult = sceDisplayGetMode(&activeMode, &activeWidth, &activeHeight);

    g_guInitialized = true;
    psp::BootLog("GU: initialized %dx%d framebuffer width=%d offsets=%08x,%08x,%08x edram=%08x listBytes=%u", kScreenWidth, kScreenHeight,
                 kFramebufferWidth, static_cast<unsigned>(drawOffset), static_cast<unsigned>(displayOffset), static_cast<unsigned>(depthOffset),
                 static_cast<unsigned>(vramBytes),
                 static_cast<unsigned>(g_displayList.size() * sizeof(g_displayList[0])));
    psp::BootLog("GU: active display result=%d top=%p width=%d format=%d", displayResult, displayTop, displayWidth, displayFormat);
    psp::BootLog("GU: display mode result=%d mode=%d size=%dx%d", activeModeResult, activeMode, activeWidth, activeHeight);
    return true;
}
#endif

} // namespace

void CreateConstantBuffers() {}
void CreateVertexBuffers() {}
void CreateNewSRVTexture() {}
void CreateUserDefinedAnnotations() {}

std::uint32_t CreateNewTexture2D() {
    return ++g_nextTextureHandle;
}

D3TDeviceContext *DXGetPipelineContext(std::int32_t) {
    return &g_nullContext;
}

void WaitForCommandListFinish(D3TDeviceContext *) {}

std::int64_t DX11_ResetMappedResources(D3TDeviceContext *) {
    return 0;
}

void DX11_UnmapAndSyncShaderBindings(D3TDeviceContext *) {}

void UnmapAndClearViews(D3TDeviceContext *, std::uint32_t, std::int64_t, const float *) {}

void *MapTempConstantBufferForWriteDiscard(D3TDeviceContext *, std::int32_t, std::uint32_t, std::int32_t) {
    return g_constantScratch;
}

void *GetMappedConstantBuffer(D3TDeviceContext *, std::int64_t, std::uint32_t) {
    return g_constantScratch;
}

std::int64_t DX11_SetupShadersForDXAndSetViewports(D3TDeviceContext *) {
    return 0;
}

void DirectXUpdateWindowDimensions(std::uint32_t width, std::uint32_t height) {
    d3t::g_originalScreenWidth = width;
    d3t::g_originalScreenHeight = height;
}

void QueueTexturedTextureQuad(std::uint32_t textureHandle, float x, float y, float width, float height, std::uint32_t color) {
    QueueTexturedTextureRegionQuad(textureHandle, x, y, width, height, 0.0f, 0.0f, 1.0f, 1.0f, color);
}

void SetFrameClearViewport(int left, int top, int right, int bottom) {
#if defined(SHENMUE_HOST_PSP)
    g_clearViewport = {std::clamp(left, 0, kScreenWidth), std::clamp(top, 0, kScreenHeight),
                       std::clamp(right, 0, kScreenWidth), std::clamp(bottom, 0, kScreenHeight)};
#else
    (void)left; (void)top; (void)right; (void)bottom;
#endif
}

void QueueTexturedTextureRegionQuad(
    std::uint32_t textureHandle, float x, float y, float width, float height, float u0, float v0, float u1, float v1, std::uint32_t color) {
    if (width <= 0.0f || height <= 0.0f || u1 <= u0 || v1 <= v0)
        return;
    const std::array<TextureQuadPoint, 4> points{{
        {x, y, u0, v0}, {x + width, y, u1, v0}, {x, y + height, u0, v1}, {x + width, y + height, u1, v1},
    }};
    QueueTextureQuad(textureHandle, points, color, 0.0f, 0, 0, 480, 272);
}

void QueueTextureQuad(std::uint32_t textureHandle, const std::array<TextureQuadPoint, 4> &points, std::uint32_t colour,
                      float depth, int clipLeft, int clipTop, int clipRight, int clipBottom) {
#if defined(SHENMUE_HOST_PSP)
    if (textureHandle == 0 || g_texturedQuadCount >= g_texturedQuads.size() || clipLeft >= clipRight || clipTop >= clipBottom)
        return;
    d3t::D3TTextureBackendView view{};
    if (!d3t::GetTexture2DBackendView(textureHandle, &view) || !view.texture || (view.format != 2 && view.format != 30 && view.format != d3t::kPspTexture5551 &&
        view.format != d3t::kPspTexture5650 && view.format != d3t::kPspTexture4444) || view.width == 0 ||
        view.height == 0 || view.width > 512 || view.height > 512) {
        if (g_textureQueueWarnings++ < 16)
            BootLog("GU: skipped texture=%08x pixels=%p size=%ux%u format=%u", textureHandle, view.texture, view.width, view.height, view.format);
        return;
    }
    const std::size_t sequence = g_texturedQuadCount;
    g_texturedQuads[g_texturedQuadCount++] = {view.texture, view.width, view.height, view.format, points, colour, depth, sequence,
        clipLeft, clipTop, clipRight, clipBottom, view.swizzled};
    if (g_texturedQueueLogs++ < 40)
        BootLog("GU: queued texture=%08x size=%ux%u swizzled=%u depth=%.1f clip=%d,%d-%d,%d", textureHandle,
                view.width, view.height, view.swizzled ? 1U : 0U, depth, clipLeft, clipTop, clipRight, clipBottom);
#else
    (void)textureHandle; (void)points; (void)colour; (void)depth;
    (void)clipLeft; (void)clipTop; (void)clipRight; (void)clipBottom;
#endif
}

void QueueTexturedSurfaceQuad(std::int32_t surface, float x, float y, float width, float height, std::uint32_t color) {
#if defined(SHENMUE_HOST_PSP)
    if (surface < 0)
        return;
    QueueTexturedTextureQuad(d3t::GetPVRSurfaceTextureHandle(surface), x, y, width, height, color);
#else
    (void)surface;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)color;
#endif
}

void PresentFrame(std::int32_t) {
#if defined(SHENMUE_HOST_PSP)
    if (!InitializeNativeGU()) {
        g_texturedQuadCount = 0;
        return;
    }

    const int startResult = sceGuStart(GU_DIRECT, g_displayList.data());
    if (startResult < 0) {
        psp::BootLog("GU: frame list failed frame=%u result=%d", static_cast<unsigned>(g_presentCount), startResult);
        return;
    }
    const std::uint32_t clearColor = s1::g_ClearColour;
    sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
    if (g_clearViewport != std::array<int, 4>{0, 0, kScreenWidth, kScreenHeight}) {
        sceGuClearColor(0xff000000U);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sceGuScissor(g_clearViewport[0], g_clearViewport[1], g_clearViewport[2] - g_clearViewport[0],
                     g_clearViewport[3] - g_clearViewport[1]);
    }
    sceGuClearColor(clearColor);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
    if (g_texturedQuadCount != 0) {
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuEnable(GU_BLEND);
        std::sort(g_texturedQuads.begin(), g_texturedQuads.begin() + g_texturedQuadCount, [](const TexturedQuad &a, const TexturedQuad &b) {
            return a.depth != b.depth ? a.depth < b.depth : a.sequence < b.sequence;
        });
        for (std::size_t index = 0; index < g_texturedQuadCount; ++index) {
            const TexturedQuad &quad = g_texturedQuads[index];
            sceGuScissor(quad.clipLeft, quad.clipTop, quad.clipRight - quad.clipLeft, quad.clipBottom - quad.clipTop);
            const int format = quad.textureFormat == d3t::kPspTexture5551 ? GU_PSM_5551 :
                quad.textureFormat == d3t::kPspTexture5650 ? GU_PSM_5650 :
                quad.textureFormat == d3t::kPspTexture4444 ? GU_PSM_4444 :
                quad.textureFormat == 30 ? GU_PSM_DXT5 : GU_PSM_8888;
            sceGuTexMode(format, 0, 0, quad.swizzled ? GU_TRUE : GU_FALSE);
            const auto &p = quad.points;
            const bool axisAligned = p[0].y == p[1].y && p[2].y == p[3].y && p[0].x == p[2].x && p[1].x == p[3].x;
            const std::array<std::size_t, 6> corners = axisAligned ? std::array<std::size_t, 6>{0, 3, 0, 0, 0, 0}
                                                                 : std::array<std::size_t, 6>{0, 1, 2, 1, 3, 2};
            const int vertexCount = axisAligned ? 2 : 6;
            auto *const vertices = static_cast<TexturedVertex *>(sceGuGetMemory(vertexCount * sizeof(TexturedVertex)));
            if (!vertices)
                continue;
            for (int vertex = 0; vertex < vertexCount; ++vertex) {
                const TextureQuadPoint &point = p[corners[static_cast<std::size_t>(vertex)]];
                vertices[vertex] = {point.u * quad.textureWidth, point.v * quad.textureHeight, quad.color, point.x, point.y, 0.0f};
            }
            const std::int64_t textureBytes = d3t::GetStride(static_cast<std::int32_t>(quad.textureWidth),
                                                             static_cast<std::int32_t>(quad.textureHeight), quad.textureFormat);
            if (!quad.swizzled && textureBytes > 0)
                sceKernelDcacheWritebackRange(quad.pixels, static_cast<unsigned>(textureBytes));
            sceGuDisable(GU_TEXTURE_2D);
            sceGuTexImage(0, static_cast<int>(quad.textureWidth), static_cast<int>(quad.textureHeight), static_cast<int>(quad.textureWidth),
                          quad.pixels);
            sceGuEnable(GU_TEXTURE_2D);
            sceGuDrawArray(axisAligned ? GU_SPRITES : GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, vertexCount, nullptr, vertices);
        }
        sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
        sceGuDisable(GU_BLEND);
        sceGuDisable(GU_TEXTURE_2D);
    }
    const int listBytes = sceGuFinish();
    const int syncResult = sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    if (syncResult < 0) {
        psp::BootLog("GU: frame sync failed frame=%u result=%d", static_cast<unsigned>(g_presentCount), syncResult);
    }
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    if (g_presentCount < 8 || (g_texturedQuadCount != 0 && g_texturedPresentLogs < 40)) {
        psp::BootLog("GU: presented frame=%u listBytes=%d sync=%d clear=%08x texturedQuads=%u",
                     static_cast<unsigned>(g_presentCount), listBytes, syncResult, static_cast<unsigned>(clearColor),
                     static_cast<unsigned>(g_texturedQuadCount));
        if (g_texturedQuadCount != 0)
            ++g_texturedPresentLogs;
    }
    g_texturedQuadCount = 0;
    g_clearViewport = {0, 0, kScreenWidth, kScreenHeight};
    ++g_presentCount;
#endif
}

void DumpFramePng(const char *) {}

} // namespace shenmue::psp
