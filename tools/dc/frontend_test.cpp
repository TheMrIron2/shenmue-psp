#include "s1/dreamcast_frontend.hpp"
#include "s1/pvr_decoder.hpp"
#include "psp/frontend_renderer.hpp"
#include "psp/psp_renderer.hpp"
#include "psp/texture_layout.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>

namespace dc = shenmue::s1::dreamcast_frontend;
namespace {
struct Submission {
    std::array<shenmue::psp::TextureQuadPoint, 4> points;
    float depth;
    int left, top, right, bottom;
};
std::vector<Submission> submissions;
std::array<int, 4> clearViewport;

void CheckTable(const std::vector<char> &binary, std::size_t offset, const dc::Layout *layouts, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        const auto &r = layouts[index];
        const std::array<std::int16_t, 5> expected{r.centerX, r.centerY, r.width, r.height, r.depth};
        for (std::size_t field = 0; field < expected.size(); ++field) {
            const std::size_t at = offset + index * 10 + field * 2;
            assert(at + 2 <= binary.size());
            const auto actual = static_cast<std::int16_t>(static_cast<unsigned char>(binary[at]) |
                                                          (static_cast<unsigned char>(binary[at + 1]) << 8));
            assert(actual == expected[field]);
        }
    }
}
}

namespace shenmue::psp {
void SetFrameClearViewport(int left, int top, int right, int bottom) {
    clearViewport = {left, top, right, bottom};
}
void QueueTextureQuad(std::uint32_t, const std::array<TextureQuadPoint, 4> &points, std::uint32_t,
                      float depth, int left, int top, int right, int bottom) {
    submissions.push_back({points, depth, left, top, right, bottom});
}
}

int main(int argc, char **argv) {
    assert(argc == 3);
    for (int argument = 1; argument < argc; ++argument) {
        std::ifstream source(argv[argument], std::ios::binary);
        const std::vector<char> binary{std::istreambuf_iterator<char>(source), {}};
        const bool usa = argument == 2;
        if (usa)
            for (std::size_t phase = 0; phase < dc::kTitlePhaseTicks.size(); ++phase) {
                const std::size_t at = 0x28cad4 + phase * 2;
                const auto ticks = static_cast<std::int16_t>(static_cast<unsigned char>(binary[at]) |
                                                           (static_cast<unsigned char>(binary[at+1]) << 8));
                assert(ticks == dc::kTitlePhaseTicks[phase]);
            }
        CheckTable(binary, usa ? 0x28cae8 : 0x29b1ce, dc::kTitleLayout.data(), dc::kTitleLayout.size());
        CheckTable(binary, usa ? 0x28cb96 : 0x29b27c, dc::kMenuLayout.data(), dc::kMenuLayout.size());
    }
    for (std::size_t pixelBytes : {2U, 4U}) {
        std::vector<bool> occupied(512 * 256 * pixelBytes);
        for (std::size_t y = 0; y < 256; ++y) {
            for (std::size_t x = 0; x < 512; ++x) {
                const std::size_t offset = shenmue::psp::SwizzledPixelOffset(512, x, y, pixelBytes);
                assert(offset + pixelBytes <= occupied.size());
                for (std::size_t byte = 0; byte < pixelBytes; ++byte) {
                    assert(!occupied[offset + byte]);
                    occupied[offset + byte] = true;
                }
            }
        }
        assert(std::all_of(occupied.begin(), occupied.end(), [](bool value) { return value; }));
    }
    assert(shenmue::psp::PackPvrPixel16(255, 0, 0, 255, 0) == 0x801f);
    assert(shenmue::psp::PackPvrPixel16(0, 255, 0, 255, 1) == 0x07e0);
    assert(shenmue::psp::PackPvrPixel16(0, 0, 255, 255, 2) == 0xff00);
    // Check native packing against every original 16-bit colour through the actual PVR decoder.
    for (std::uint8_t format = 0; format < 3; ++format) {
        std::vector<std::uint8_t> pvr(16 + 65536 * 2);
        pvr[8] = format; pvr[9] = 9;
        pvr[12] = 0; pvr[13] = 1; pvr[14] = 0; pvr[15] = 1;
        for (unsigned value = 0; value < 65536; ++value) {
            pvr[16 + value * 2] = value & 255;
            pvr[17 + value * 2] = value >> 8;
        }
        std::vector<std::uint8_t> rgba(65536 * 4);
        shenmue::s1::PVR_DecodeMain(rgba.data(), pvr.data());
        for (unsigned value = 0; value < 65536; ++value) {
            const unsigned at = ((255 - value / 256) * 256 + value % 256) * 4;
            const auto packed = shenmue::psp::PackPvrPixel16(rgba[at], rgba[at+1], rgba[at+2], rgba[at+3], format);
            const unsigned expected = format == 0 ? ((value >> 10) & 31) | (value & 0x83e0) | ((value & 31) << 10) :
                format == 1 ? ((value >> 11) & 31) | (value & 0x07e0) | ((value & 31) << 11) :
                ((value >> 8) & 15) | (value & 0xf0f0) | ((value & 15) << 8);
            assert(packed == expected);
        }
    }
    // USA controller timing and Start gating, including countdown boundary ticks.
    dc::TitleController controller;
    assert(!dc::AcceptTitleStart(controller, 0x0800));
    for (int tick = 0; tick < 63; ++tick) dc::AdvanceTitleController(controller);
    assert(controller.alpha == 252 && controller.step == 16 && controller.countdown == 60);
    dc::AdvanceTitleController(controller);
    assert(controller.alpha == 255 && controller.step == -16);
    for (int tick = 0; tick < 60; ++tick) dc::AdvanceTitleController(controller);
    assert(controller.phase == 1 && controller.countdown == 0);
    dc::AdvanceTitleController(controller);
    assert(controller.phase == 2 && controller.countdown == 240 && controller.alpha == 0);
    assert(!dc::AcceptTitleStart(controller, 0x0400));
    dc::TitleController accepted = controller;
    assert(dc::AcceptTitleStart(accepted, 0x0800) && accepted.phase == -1);
    dc::AdvanceTitleController(accepted);
    assert(accepted.phase == -1);
    dc::AdvanceTitleController(controller);
    for (int tick = 0; tick < 240; ++tick) dc::AdvanceTitleController(controller);
    assert(controller.phase == 2 && controller.countdown == 0);
    dc::AdvanceTitleController(controller);
    assert(controller.phase == 3 && controller.alpha == 0 && controller.countdown == 60);
    assert(dc::TitleClearColour(controller) == 0xffffffffU);
    for (const auto expected : {std::array<int, 4>{3,9,256,128}, {4,7,512,128}, {5,8,256,256}, {7,10,128,128}, {8,11,256,256}}) {
        dc::ResetTitleController(controller, expected[0]);
        for (int tick = 0; tick < 64; ++tick) dc::AdvanceTitleController(controller);
        dc::DrawList logo;
        dc::DrawTitlePhase(logo, controller);
        assert(logo.count == 1 && logo.sprites[0].texture == expected[1]);
        assert(logo.sprites[0].layout.width == expected[2] && logo.sprites[0].layout.height == expected[3]);
        assert(logo.sprites[0].colour == 0xffffffffU);
        const int duration = dc::kTitlePhaseTicks[expected[0]];
        for (int tick = 0; tick <= duration; ++tick) dc::AdvanceTitleController(controller);
        assert(controller.phase == (expected[0] == 8 ? 1 : expected[0] + 1));
    }
    dc::ResetTitleController(controller, 6);
    dc::DrawList handoff;
    dc::DrawTitlePhase(handoff, controller);
    assert(handoff.count == 0 && dc::TitleClearColour(controller) == 0xff000000U);
    dc::ResetTitleController(controller, 2);
    controller.animation.blink = 0;
    dc::DrawList returned;
    dc::DrawTitlePhase(returned, controller);
    assert(controller.countdown == 240 && returned.count == 7);
    assert(returned.sprites[0].colour == 0xffffffffU);
    dc::Animation animation;
    dc::DrawList title;
    dc::DrawTitle(title, animation);
    assert(title.count == 7 && title.sprites[2].layout.centerY == 209);
    for (int tick = 0; tick < 16; ++tick) dc::Tick(animation, true);
    dc::DrawList blink;
    dc::DrawTitle(blink, animation);
    assert(blink.count == 6);
    assert(std::none_of(blink.sprites.begin(), blink.sprites.begin() + blink.count, [](const auto &s) { return s.texture == 4; }));
    for (int item = 0; item < 4; ++item) {
        dc::DrawList menu;
        dc::DrawMainMenu(menu, animation, item, true, true);
        assert(menu.count == 19);
        assert(menu.sprites[6 + item].texture == 15);
        const std::uint16_t expectedHelp[] = {24, 26, 25, 27};
        assert(std::any_of(menu.sprites.begin(), menu.sprites.begin() + menu.count,
                          [&](const auto &s) { return s.texture == expectedHelp[item]; }));
        dc::DrawList empty;
        dc::DrawMainMenu(empty, animation, item, false, false);
        const bool noFiles = std::any_of(empty.sprites.begin(), empty.sprites.begin() + empty.count,
                                        [](const auto &s) { return s.texture == 32; });
        assert(noFiles == (item == 1 || item == 2));
    }
    for (int current = 0; current < 4; ++current) {
        assert(dc::SelectMenuItem(current, 0x8000) == 2);
        assert(dc::SelectMenuItem(current, 0x4000) == 1);
        assert(dc::SelectMenuItem(current, 0x1000) == 0);
        assert(dc::SelectMenuItem(current, 0x2000) == 3);
    }
    submissions.clear();
    shenmue::psp::frontend::Submit(title, false);
    assert(submissions.size() == 7);
    assert((clearViewport == std::array<int, 4>{59, 0, 421, 272}));
    const auto &background = submissions[0];
    assert(background.left == 59 && background.right == 421 && background.top == 0 && background.bottom == 272);
    assert(background.points[0].x < background.left && background.points[3].x > background.right);
    assert(std::fabs(submissions[2].points[0].y - (209 - 64) * 272.0f / 480.0f) < 0.001f);
    submissions.clear();
    shenmue::psp::frontend::Submit(title, true);
    assert(submissions[0].left == 0 && submissions[0].right == 480);
    assert((clearViewport == std::array<int, 4>{0, 0, 480, 272}));
    assert(std::fabs(submissions[2].points[0].x - 48.0f) < 0.001f);
    title.sprites[1].angle = 16384.0f;
    submissions.clear();
    shenmue::psp::frontend::Submit(title, false);
    const auto &phoenix = submissions[1];
    assert(phoenix.points[0].x > phoenix.points[2].x);
    assert(phoenix.points[0].y < phoenix.points[1].y);
}
