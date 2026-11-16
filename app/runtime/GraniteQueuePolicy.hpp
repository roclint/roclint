#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace roclint::runtime {

// The host enforces a monotonic deadline for every native request. A request can
// wait in the single local worker's FIFO behind another generation, so the helper
// must not grant itself a fresh full budget after that wait. The host reports the
// remaining budget it will still accept when it submits a request; the helper
// takes the earlier of its own operation budget and that remaining budget.
//
// This cannot extend any caller's deadline. It only lets the helper reject work
// that is already obsolete before the GPU/CPU starts generating it.
inline std::chrono::steady_clock::time_point effectiveNativeDeadline(
    std::chrono::steady_clock::time_point receivedAt, std::chrono::milliseconds ownBudget,
    std::optional<std::chrono::milliseconds> callerRemaining) noexcept {
    const auto ownDeadline = receivedAt + std::max(ownBudget, std::chrono::milliseconds(0));
    if (!callerRemaining.has_value()) return ownDeadline;
    const auto remaining = std::max(*callerRemaining, std::chrono::milliseconds(0));
    return std::min(ownDeadline, receivedAt + remaining);
}

} // namespace roclint::runtime
