#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace shenmue::s1::dreamcast_frontend {

struct Layout {
    std::int16_t centerX, centerY, width, height, depth;
};
static_assert(sizeof(Layout) == 10);

struct Sprite {
    std::uint16_t texture;
    Layout layout;
    std::uint32_t colour{0xffffffffU};
    float scale{1.0f};
    float angle{}; // Dreamcast angle units, one turn = 65536.
};

struct DrawList {
    std::array<Sprite, 48> sprites{};
    std::size_t count{};
    void Add(std::uint16_t texture, Layout layout, std::uint32_t colour = 0xffffffffU, float scale = 1.0f, float angle = 0.0f);
};

struct Animation {
    std::uint32_t blink{};
    float phoenixAngle{};
    std::int32_t pulse{};
    std::int32_t pulseStep{16};
};

struct TitleController {
    std::int32_t phase{1};
    std::int32_t countdown{60};
    std::int32_t alpha{};
    std::int32_t step{16};
    Animation animation{};
};

inline constexpr std::array<std::int16_t, 10> kTitlePhaseTicks{150, 60, 240, 60, 90, 60, 60, 60, 60, 150};
void ResetTitleController(TitleController &state, std::int32_t phase = 1);
void AdvanceTitleController(TitleController &state);
bool AcceptTitleStart(TitleController &state, std::uint16_t pressed);
void DrawTitlePhase(DrawList &list, const TitleController &state);
std::uint32_t TitleClearColour(const TitleController &state);

// USA 0c29cae8 / PAL 0c2ab1ce. Full texture dimensions include padding.
inline constexpr std::array<Layout, 7> kTitleLayout{{
    {320, 240, 1024, 512, -5}, {320, 240, 512, 512, -4}, {320, 209, 512, 128, -3},
    {310, 297, 512, 64, -3}, {320, 380, 512, 32, -3}, {302, 430, 256, 32, -3}, {424, 433, 64, 64, -3},
}};
// USA 0c29cb96 / PAL 0c2ab27c.
inline constexpr std::array<Layout, 11> kMenuLayout{{
    {320, 50, 512, 64, -4}, {323, 411, 512, 128, -4}, {310, 136, 256, 32, -3},
    {194, 224, 256, 32, -3}, {441, 224, 128, 32, -3}, {320, 308, 128, 32, -3},
    {325, 141, 128, 128, -4}, {200, 228, 128, 128, -4}, {450, 228, 128, 128, -4},
    {325, 311, 128, 128, -4}, {319, 410, 256, 32, -3},
}};
inline constexpr std::array<Layout, 4> kSharedLayout{{
    {320, 240, 1024, 512, -5}, {536, 429, 32, 32, -2}, {574, 429, 32, 32, -2}, {612, 429, 32, 32, -2},
}};

void Tick(Animation &animation, bool title);
std::int32_t SelectMenuItem(std::int32_t current, std::uint16_t directions);
void DrawTitle(DrawList &list, const Animation &animation, bool showPrompt = true);
void DrawMainMenu(DrawList &list, const Animation &animation, std::int32_t selection, bool hasSave, bool hasResume,
                  bool alternateHelp = false, bool pal = false);

} // namespace shenmue::s1::dreamcast_frontend
