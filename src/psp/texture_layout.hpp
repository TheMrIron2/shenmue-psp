#pragma once

#include <cstddef>
#include <cstdint>

namespace shenmue::psp {

constexpr std::size_t SwizzledPixelOffset(std::size_t width, std::size_t x, std::size_t y, std::size_t pixelBytes) {
    const std::size_t blockWidth = 16 / pixelBytes;
    return ((y / 8) * (width / blockWidth) + x / blockWidth) * 128 + (y & 7) * 16 + (x % blockWidth) * pixelBytes;
}

constexpr std::uint16_t PackPvrPixel16(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a, std::uint8_t pvrFormat) {
    if (pvrFormat == 0)
        return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
    if (pvrFormat == 1)
        return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
    return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
}

} // namespace shenmue::psp
