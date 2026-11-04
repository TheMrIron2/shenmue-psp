#include "s1/dreamcast_frontend.hpp"

#include <algorithm>

namespace shenmue::s1::dreamcast_frontend {

void DrawList::Add(std::uint16_t texture, Layout layout, std::uint32_t colour, float scale, float angle) {
    if (count < sprites.size())
        sprites[count++] = {texture, layout, colour, scale, angle};
}

void Tick(Animation &animation, bool title) {
    animation.blink = (animation.blink + 1U) & 31U;
    if (title) {
        animation.phoenixAngle += 18.20444107055664f;
        if (animation.phoenixAngle >= 65536.0f)
            animation.phoenixAngle -= 65536.0f;
    } else {
        animation.pulse += animation.pulseStep;
        if (animation.pulse > 255) {
            animation.pulse = 255;
            animation.pulseStep = -16;
        } else if (animation.pulse < 0) {
            animation.pulse = 0;
            animation.pulseStep = 16;
        }
    }
}

// USA display callback 0c183dc8; title input 0c1858e4.
void ResetTitleController(TitleController &state, std::int32_t phase) {
    state.phase = phase;
    state.countdown = kTitlePhaseTicks[static_cast<std::size_t>(phase)];
    state.alpha = 0;
    state.step = 16;
    state.animation.phoenixAngle = 0;
}

void AdvanceTitleController(TitleController &state) {
    if (state.phase < 0 || state.phase >= static_cast<std::int32_t>(kTitlePhaseTicks.size()))
        return;
    if ((state.step & 0x8000) != 0) {
        if (--state.countdown < 0) {
            if (++state.phase > 8) {
                state.phase = 1;
                state.animation.phoenixAngle = 0;
            }
            state.countdown = kTitlePhaseTicks[static_cast<std::size_t>(state.phase)];
            state.alpha = 0;
            state.step = 16;
        }
    } else if (state.phase == 2) {
        state.step = -16;
    } else {
        state.alpha += 4;
        if (state.alpha > 255) {
            state.alpha = 255;
            state.step = -16;
        }
    }
    state.animation.blink = (state.animation.blink + 1U) & 31U;
    if (state.phase <= 2)
        state.animation.phoenixAngle += 18.20444107055664f;
}

bool AcceptTitleStart(TitleController &state, std::uint16_t pressed) {
    if (state.phase != 2 || (pressed & 0x0800U) == 0)
        return false;
    state.phase = -1;
    state.alpha = 0;
    state.step = 16;
    state.animation.phoenixAngle = 0;
    return true;
}

std::uint32_t TitleClearColour(const TitleController &state) {
    return state.phase == 3 || state.phase == 4 || state.phase == 5 || state.phase == 7 || state.phase == 8
        ? 0xffffffffU : 0xff000000U;
}

void DrawTitlePhase(DrawList &list, const TitleController &state) {
    const std::uint32_t colour = (static_cast<std::uint32_t>(state.alpha) << 24U) | 0x00ffffffU;
    if (state.phase >= 0 && state.phase <= 2) {
        DrawTitle(list, state.animation, state.phase == 0 || state.phase == 2);
        if (state.phase != 2)
            for (std::size_t index = 0; index < list.count; ++index)
                list.sprites[index].colour = colour;
        return;
    }
    // USA logo draw 0c1838f4. Phase 6 belongs to the real scene handoff.
    switch (state.phase) {
    case 3: list.Add(9, {320, 240, 256, 128, -5}, colour); break;
    case 4: list.Add(7, {320, 240, 512, 128, -5}, colour); break;
    case 5: list.Add(8, {320, 240, 256, 256, -5}, colour); break;
    case 7: list.Add(10, {320, 240, 128, 128, -5}, colour); break;
    case 8: list.Add(11, {320, 240, 256, 256, -5}, colour); break;
    }
}

// Recovered directional selection: USA 0c183540 / PAL 0c184298.
std::int32_t SelectMenuItem(std::int32_t current, std::uint16_t directions) {
    if ((directions & 0x8000U) && current != 2) return 2;
    if ((directions & 0x4000U) && current != 1) return 1;
    if ((directions & 0x1000U) && current != 0) return 0;
    if ((directions & 0x2000U) && current != 3) return 3;
    return current;
}

// Recovered title draw: USA 0c18396e / PAL 0c1847dc.
void DrawTitle(DrawList &list, const Animation &animation, bool showPrompt) {
    for (std::uint16_t index = 0; index < kTitleLayout.size(); ++index) {
        if (index == 4 && (!showPrompt || animation.blink >= 16))
            continue;
        list.Add(index, kTitleLayout[index], 0xffffffffU, 1.0f, index == 1 ? animation.phoenixAngle : 0.0f);
    }
}

// Recovered main-menu draw: USA 0c1841ac / PAL 0c185088.
void DrawMainMenu(DrawList &list, const Animation &animation, std::int32_t selection, bool hasSave, bool hasResume,
                  bool alternateHelp, bool pal) {
    constexpr std::array<std::uint16_t, 6> fixedTextures{13, 14, 20, 22, 21, 23};
    constexpr std::array<std::array<std::int16_t, 3>, 9> help{{
        {24, 512, 32}, {26, 512, 64}, {25, 512, 64}, {27, 512, 32},
        {28, 512, 32}, {29, 512, 32}, {31, 256, 64}, {30, 512, 32}, {32, 512, 32},
    }};
    const std::uint32_t pulse = static_cast<std::uint32_t>(std::clamp(animation.pulse, 0, 255));
    const std::uint32_t glowColour = 0x2f000000U | pulse | (pulse << 8U) | (pulse << 16U);
    selection = std::clamp(selection, 0, 3);
    for (std::size_t index = 0; index < kMenuLayout.size(); ++index) {
        Layout layout = kMenuLayout[index];
        std::uint16_t texture;
        if (index < fixedTextures.size()) {
            texture = fixedTextures[index];
        } else if (index < 10) {
            texture = index - 6 == static_cast<std::size_t>(selection) ? 15 : 16;
        } else {
            const bool noFiles = (selection == 1 && !hasSave) || (selection == 2 && !hasResume);
            const auto &message = help[noFiles ? 8 : static_cast<std::size_t>(selection + (alternateHelp ? 4 : 0))];
            texture = static_cast<std::uint16_t>(message[0]);
            layout.width = message[1];
            layout.height = message[2];
        }
        list.Add(texture, layout);
        if (texture == 15)
            list.Add(texture, layout, glowColour);
    }
    constexpr std::array<std::uint16_t, 4> sharedTextures{12, 17, 18, 19};
    for (std::size_t index = 0; index < sharedTextures.size(); ++index) {
        Layout layout = kSharedLayout[index];
        if (index == 0 && pal)
            layout.depth = -10;
        const float scale = index == 0 ? 1.0f : 1.3f;
        list.Add(sharedTextures[index], layout, 0xffffffffU, scale);
        if (index != 0)
            list.Add(sharedTextures[index], layout, glowColour, scale);
    }
}

} // namespace shenmue::s1::dreamcast_frontend
