#pragma once

#include <cstddef>

namespace shenmue {

// Bounded string length. POSIX `strnlen` is not available in the PSP (newlib)
// C library; provide a portable equivalent used by the UI layout code.
inline std::size_t strnlen(const char *text, std::size_t maxLength) noexcept {
    std::size_t length = 0;
    while (length < maxLength && text[length] != '\0') {
        ++length;
    }
    return length;
}

} // namespace shenmue
