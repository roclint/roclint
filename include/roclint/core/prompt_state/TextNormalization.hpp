#pragma once

#include <string>
#include <string_view>

namespace roclint::core {

// C01 canonical text: converts CRLF and bare CR to LF. No other normalization is applied.
[[nodiscard]] std::string normalizeLineEndings(std::string_view text);

[[nodiscard]] bool hasCanonicalLineEndings(std::string_view text) noexcept;

} // namespace roclint::core
