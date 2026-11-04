#pragma once

#include "s1/dreamcast_frontend.hpp"

#include <cstddef>
#include <cstdint>

namespace shenmue::psp::frontend {

bool LoadTextures(const std::byte *bytes, std::size_t size, std::int32_t *surfaces, std::size_t capacity);
void ReleaseTextures();
std::uint32_t TextureHandle(std::size_t index);
void Submit(const s1::dreamcast_frontend::DrawList &list, bool widescreen);

} // namespace shenmue::psp::frontend
