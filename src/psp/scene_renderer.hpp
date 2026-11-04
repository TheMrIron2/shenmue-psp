#pragma once

#include "common/math.hpp"
#include <cstdint>

namespace shenmue::psp {

struct SceneMaterial {
    std::uint32_t texture{};
    bool blend{};
    bool alphaTest{};
    float alphaReference{};
    float opacity{1.0f};
};

bool QueueSceneTriangles(const Matrix4x4 *vertices, std::uint32_t count, const Matrix4x4 &modelView,
                         const Matrix4x4 &projection, const SceneMaterial &material, bool widescreen);
void RenderQueuedScene();
void ResetSceneQueue();

} // namespace shenmue::psp
