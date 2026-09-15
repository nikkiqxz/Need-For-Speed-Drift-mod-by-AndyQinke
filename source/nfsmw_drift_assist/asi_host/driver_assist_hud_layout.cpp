#include "driver_assist_hud.hpp"

#include <algorithm>
#include <cmath>

namespace nfsmw_drift_asi::driver_assist_hud {
namespace {

long ScaleRound(float value, float scale) noexcept {
    return static_cast<long>(std::lround(value * scale));
}

}  // namespace

Layout MakeLayout(std::uint32_t width, std::uint32_t height) noexcept {
    Layout layout{};
    if (width == 0 || height == 0) return layout;
    const float scale = std::max(
        0.5f, std::min(static_cast<float>(width) / 1920.0f,
                       static_cast<float>(height) / 1080.0f));
    const long outerWidth = std::max<long>(42, ScaleRound(82.875f, scale));
    const long outerHeight = std::max<long>(39, ScaleRound(78.0f, scale));
    const long rightMargin = ScaleRound(272.0f, scale);
    const long bottomMargin = ScaleRound(42.0f, scale);
    const long left = std::max<long>(
        0, static_cast<long>(width) - rightMargin - outerWidth);
    const long top = std::max<long>(
        0, static_cast<long>(height) - bottomMargin - outerHeight);
    layout.outer = {left, top, left + outerWidth, top + outerHeight};
    const long border = std::max<long>(2, ScaleRound(3.0f, scale));
    const long gap = std::max<long>(1, ScaleRound(2.0f, scale));
    const long contentHeight = outerHeight - 2 * border - 2 * gap;
    const long rowHeight = std::max<long>(1, contentHeight / 3);
    for (int row = 0; row < 3; ++row) {
        const long rowTop = top + border + row * (rowHeight + gap);
        const long rowBottom = row == 2
            ? top + outerHeight - border
            : rowTop + rowHeight;
        layout.rows[row] = {
            left + border, rowTop, left + outerWidth - border, rowBottom};
    }
    layout.pixel = std::max(
        1, static_cast<int>(std::lround(2.0f * scale)));
    return layout;
}

}  // namespace nfsmw_drift_asi::driver_assist_hud
