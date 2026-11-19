#pragma once
#include "roclint/bridge/GraniteAdapter.hpp"

namespace roclint::bridge {
// A bounded check of a proposed interview detour. Clean keeps the original
// intent question; protected suppresses it; repair still needs sentence auditing.
struct WordSurfaceCheck {
    bool handled{false};
    WordRefinementResponse response{};
};
[[nodiscard]] std::string buildWordSurfaceCheckPrompt(const WordRefinementRequest& request);
[[nodiscard]] const std::string& wordSurfaceCheckResponseSchema();
[[nodiscard]] core::Result<WordSurfaceCheck> parseWordSurfaceCheckResponse(
    std::string_view response, const WordRefinementRequest& request);
}
