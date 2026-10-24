#include "roclint/core/prompt_state/TextNormalization.hpp"

namespace roclint::core {

std::string normalizeLineEndings(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool hasCanonicalLineEndings(std::string_view text) noexcept {
    return text.find('\r') == std::string_view::npos;
}

} // namespace roclint::core
