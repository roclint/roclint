#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace roclint::host {

// Versioned engineering defaults; release thresholds still require C08 measurements.
struct GraniteResidencyPolicyV1 {
    static constexpr std::int64_t kBalancedMappedMs = 5 * 60 * 1000;
    static constexpr std::int64_t kBalancedUnloadMs = 30 * 60 * 1000;
    static constexpr std::int64_t kLowMemoryUnloadMs = 30 * 1000;
    static constexpr int kPollMs = 5000;

    static bool validPreset(std::string_view preset) {
        return preset == "KEEP_WARM" || preset == "BALANCED" || preset == "LOW_MEMORY";
    }

    // Never promote residency automatically. A semantic request or explicit Load warms it.
    static std::optional<std::string_view> desiredTier(std::string_view preset,
        std::string_view current, std::int64_t idleMs, bool busy, bool lowMemory) {
        if (busy || idleMs < 0 || !validPreset(preset) || current == "UNLOADED") return std::nullopt;
        if (current != "HOT" && current != "IDLE_MAPPED") return std::nullopt;
        if (lowMemory || (preset == "LOW_MEMORY" && idleMs >= kLowMemoryUnloadMs) ||
            (preset == "BALANCED" && idleMs >= kBalancedUnloadMs)) return "UNLOADED";
        if (preset == "BALANCED" && current == "HOT" && idleMs >= kBalancedMappedMs) return "IDLE_MAPPED";
        return std::nullopt;
    }
};

} // namespace roclint::host
