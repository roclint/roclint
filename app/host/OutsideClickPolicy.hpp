#pragma once

#include <chrono>

namespace roclint::host {

inline constexpr auto kOutsideClickSettleWindow = std::chrono::milliseconds(400);

// Pure policy shared by the platform monitor and its tests. The initial settle
// window suppresses the click that opened the HUD; clicks inside the card or in
// the same host application keep it open.
[[nodiscard]] constexpr bool shouldDismissOutsideClick(bool pointerInsideCard,
                                                       bool sameHostApplication,
                                                       std::chrono::milliseconds sinceInstall) noexcept {
    return !pointerInsideCard && !sameHostApplication && sinceInstall >= kOutsideClickSettleWindow;
}

} // namespace roclint::host
