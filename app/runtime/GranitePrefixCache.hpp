#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace roclint::runtime {

// Returns the token prefix that can be reused from a previous prompt. At least the
// final prompt token is always re-evaluated: sampling needs fresh logits for the
// token that immediately precedes generation.
template <class Token>
[[nodiscard]] std::size_t reusablePromptPrefix(const std::vector<Token>& cached,
                                               const std::vector<Token>& current) noexcept {
    if (current.empty() || cached.empty()) return 0;
    const auto limit = std::min(cached.size(), current.size());
    std::size_t prefix = 0;
    while (prefix < limit && cached[prefix] == current[prefix]) ++prefix;
    // Never report the whole prompt: the last token must be decoded to produce the
    // logits used by the first sampling step.
    return std::min(prefix, current.size() - 1);
}

} // namespace roclint::runtime
