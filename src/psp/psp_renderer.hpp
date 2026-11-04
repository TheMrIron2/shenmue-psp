#pragma once

// PSP compatibility renderer for the recovered engine.
//
// The recovered renderer is written against D3D11 (`src/d3t/dx11_renderer.cpp`),
// whose entire implementation lives behind `#if defined(_WIN32)`. For the first
// PSP milestone we expose the same entry points used by the game and implement
// them as no-ops so the real boot order can run unmodified. The native GU
// backend begins with retained TEXN surfaces and GU textured quads. The full
// scene renderer still needs native geometry and material submission.

#include "d3t/buffer.hpp"
#include "d3t/dx11_renderer.hpp"

#include <array>
#include <cstdint>

namespace shenmue::psp {

using d3t::D3TDeviceContext;

void CreateConstantBuffers();
void CreateVertexBuffers();
void CreateNewSRVTexture();
void CreateUserDefinedAnnotations();
std::uint32_t CreateNewTexture2D();

D3TDeviceContext *DXGetPipelineContext(std::int32_t mode);
void WaitForCommandListFinish(D3TDeviceContext *context);
void SetFrameClearViewport(int left, int top, int right, int bottom);
struct TextureQuadPoint {
    float x, y, u, v;
};
void QueueTextureQuad(std::uint32_t textureHandle, const std::array<TextureQuadPoint, 4> &points, std::uint32_t colour,
                      float depth, int clipLeft, int clipTop, int clipRight, int clipBottom);
void QueueTexturedTextureQuad(std::uint32_t textureHandle, float x, float y, float width, float height, std::uint32_t color = 0xffffffffU);
void QueueTexturedTextureRegionQuad(std::uint32_t textureHandle, float x, float y, float width, float height, float u0, float v0, float u1, float v1,
    std::uint32_t color = 0xffffffffU);
std::int64_t DX11_ResetMappedResources(D3TDeviceContext *context);
void DX11_UnmapAndSyncShaderBindings(D3TDeviceContext *context);
void UnmapAndClearViews(D3TDeviceContext *context, std::uint32_t texture2D, std::int64_t unused, const float *clearValue);
void *MapTempConstantBufferForWriteDiscard(D3TDeviceContext *context, std::int32_t stageMask, std::uint32_t registerIndex, std::int32_t bufferSize);
void *GetMappedConstantBuffer(D3TDeviceContext *context, std::int64_t unused, std::uint32_t size);
std::int64_t DX11_SetupShadersForDXAndSetViewports(D3TDeviceContext *context);

void DirectXUpdateWindowDimensions(std::uint32_t width, std::uint32_t height);
void QueueTexturedSurfaceQuad(std::int32_t surface, float x, float y, float width, float height, std::uint32_t color = 0xffffffffU);
void PresentFrame(std::int32_t renderMode);
void DumpFramePng(const char *filename);

} // namespace shenmue::psp
