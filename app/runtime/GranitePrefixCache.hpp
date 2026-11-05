#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace roclint::runtime {

// Returns the token prefix that can be reused from a previous prompt. At least the
// final prompt token is always re-evaluated: sampling needs fresh logits for the
// token that immediately precedes generation. When chunkSize > 1, only whole
// prefill chunks are reused so the last chunk is recomputed exactly as in a cold
// prefill (avoiding small-batch logit drift).
template <class Token>
[[nodiscard]] std::size_t reusablePromptPrefix(const std::vector<Token>& cached,
                                               const std::vector<Token>& current,
                                               std::size_t chunkSize = 1) noexcept {
    if (current.empty() || cached.empty()) return 0;
    const auto limit = std::min(cached.size(), current.size());
    std::size_t prefix = 0;
    while (prefix < limit && cached[prefix] == current[prefix]) ++prefix;
    if (chunkSize > 1) prefix = (prefix / chunkSize) * chunkSize;
    if (chunkSize > 1) prefix = std::min(prefix, ((current.size() - 1) / chunkSize) * chunkSize);
    // Never report the whole prompt: the last token must be decoded to produce the
    // logits used by the first sampling step.
    return std::min(prefix, current.size() - 1);
}

} // namespace roclint::runtime
