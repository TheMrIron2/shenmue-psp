#include "../../src/psp/scene_renderer.cpp"
#include <cassert>
#include <cstdio>

int main() {
    using namespace shenmue;
    using namespace shenmue::psp;
    Matrix4x4 identity{{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}}};
    Matrix4x4 vertices[3]{};
    vertices[0].rows[0] = {1,2,3,0};
    vertices[0].rows[1] = {0,0,0.25f,0.75f};
    vertices[0].rows[2] = {1,0.5f,0,1};
    SceneMaterial material{};
    material.opacity = 0.5f;
    assert(!QueueSceneTriangles(nullptr, 3, identity, identity, material, false));
    assert(!QueueSceneTriangles(vertices, 2, identity, identity, material, false));
    assert(g_vertexCount == 0 && g_drawCount == 0);
    assert(QueueSceneTriangles(vertices, 3, identity, identity, material, false));
    const auto &vertex = g_vertices[0];
    assert(vertex.x == 1 && vertex.y == 2 && vertex.z == 3);
    assert(vertex.u == 0.25f && vertex.v == 0.75f && vertex.colour == 0x800080ffU);
    const auto &projection = g_draws[0].projection;
    // Engine near=1 and far=0 become GE clip depths -1 and +1.
    assert(projection.rows[2].z + projection.rows[3].z == -1);
    assert(projection.rows[3].z == 1);
    assert(reinterpret_cast<std::uintptr_t>(&projection) % 16 == 0);
    for (unsigned i = 1; i < kDrawCapacity; ++i)
        assert(QueueSceneTriangles(vertices, 3, identity, identity, material, false));
    assert(!QueueSceneTriangles(vertices, 3, identity, identity, material, false));
    ResetSceneQueue();
    g_vertexCount = kVertexCapacity - 2;
    assert(!QueueSceneTriangles(vertices, 3, identity, identity, material, false));
    assert(g_drawCount == 0);
    ResetSceneQueue();
    assert(g_vertexCount == 0 && g_drawCount == 0);
    std::puts("Native scene conversion, depth, alignment and queue bounds passed");
}
