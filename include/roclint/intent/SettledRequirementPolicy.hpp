#pragma once

// Presence checks for explicit quantities in rewritten prompts. These checks do
// not establish semantic equivalence of complete requirements or authorize a
// classifier bypass: a quantity in the draft is not evidence of no conflict.
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::intent {

inline constexpr const char* kSettledRequirementPolicyVersion = "settled_requirement.v2";

namespace quantity_detail {
struct Quantity {
    std::string source;
    std::string key;
};

[[nodiscard]] inline std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// Exact decimal arithmetic: digits * 10^power in seconds, bytes or percent.
// No floating-point rounding or tolerance may turn a changed requirement into
// a match. Extremely large exponents retain literal-only matching.
[[nodiscard]] inline std::string canonical(std::string number, const std::string& unit) {
    const auto literal = [&]() { return "literal:" + lower(number) + ":" + unit; };
    std::int64_t power = 0;
    auto mantissa = std::string_view(number);
    if (const auto e = number.find_first_of("eE"); e != std::string::npos) {
        auto exponent = std::string_view(number).substr(e + 1);
        if (exponent.starts_with('+')) exponent.remove_prefix(1);
        const auto parsed = std::from_chars(exponent.data(), exponent.data() + exponent.size(), power);
        if (parsed.ec != std::errc{} || parsed.ptr != exponent.data() + exponent.size() ||
            power < -10000 || power > 10000) return literal();
        mantissa = mantissa.substr(0, e);
    }
    bool negative = mantissa.starts_with('-');
    if (mantissa.starts_with('-') || mantissa.starts_with('+')) mantissa.remove_prefix(1);
    if (const auto dot = mantissa.find('.'); dot != std::string_view::npos)
        power -= static_cast<std::int64_t>(mantissa.size() - dot - 1);
    std::string digits;
    for (const char c : mantissa) if (c != '.') digits.push_back(c);

    std::string dimension;
    std::uint64_t factor = 1;
    if (unit == "%") dimension = "percent";
    else if (unit == "ms" || unit.starts_with("millisecond")) {
        dimension = "seconds";
        power -= 3;
    } else if (unit.starts_with("sec")) dimension = "seconds";
    else {
        dimension = "bytes";
        if (unit == "kb") factor = 1000;
        else if (unit == "kib") factor = 1024;
        else if (unit == "mb") factor = 1000000;
        else if (unit == "mib") factor = 1048576;
        else if (unit == "gb") factor = 1000000000;
        else if (unit == "gib") factor = 1073741824;
    }
    std::uint64_t carry = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        const auto product = static_cast<std::uint64_t>(*it - '0') * factor + carry;
        *it = static_cast<char>('0' + product % 10);
        carry = product / 10;
    }
    while (carry > 0) {
        digits.insert(digits.begin(), static_cast<char>('0' + carry % 10));
        carry /= 10;
    }
    const auto nonzero = digits.find_first_not_of('0');
    if (nonzero == std::string::npos) return dimension + ":0";
    digits.erase(0, nonzero);
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
        ++power;
    }
    return dimension + ":" + (negative ? "-" : "") + digits + "e" + std::to_string(power);
}

[[nodiscard]] inline std::vector<Quantity> parse(std::string_view text) {
    static const std::regex quantity(
        R"(([+-]?(?:[0-9]+(?:\.[0-9]+)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)\s*(ms\b|milliseconds?\b|seconds?\b|secs?\b|kb\b|kib\b|mb\b|mib\b|gb\b|gib\b|%))",
        std::regex::icase);
    const std::string source(text);
    std::vector<Quantity> quantities;
    for (std::sregex_iterator it(source.begin(), source.end(), quantity), end; it != end; ++it) {
        const auto begin = static_cast<std::size_t>(it->position());
        // Do not extract a suffix of an identifier, version, or signed quantity.
        if (begin > 0) {
            const auto prior = static_cast<unsigned char>(source[begin - 1]);
            if (std::isalnum(prior) != 0 || prior == '_' || prior == '.' || prior == '+' || prior == '-')
                continue;
        }
        const auto after = begin + static_cast<std::size_t>(it->length());
        if (after < source.size()) {
            const auto next = static_cast<unsigned char>(source[after]);
            if (std::isalnum(next) != 0 || next == '_' || next == '%') continue;
        }
        quantities.push_back({it->str(), canonical((*it)[1].str(), lower((*it)[2].str()))});
    }
    return quantities;
}
} // namespace quantity_detail

// Verbatim recognized tokens. In particular, decades such as 1990s are not time
// units. Token extraction alone never establishes that a prompt is settled.
[[nodiscard]] inline std::vector<std::string> exactRequirementTokens(std::string_view text) {
    std::vector<std::string> tokens;
    for (const auto& quantity : quantity_detail::parse(text)) {
        if (std::find(tokens.begin(), tokens.end(), quantity.source) == tokens.end())
            tokens.push_back(quantity.source);
    }
    return tokens;
}

// Return original quantities with no exact value/dimension match in the candidate.
// Whitespace, decimal notation, unit aliases and exact supported conversions may
// differ. This checks presence only; semantic review still checks roles and scope.
[[nodiscard]] inline std::vector<std::string> droppedExactRequirements(std::string_view original,
                                                                      std::string_view candidate) {
    const auto candidates = quantity_detail::parse(candidate);
    std::vector<std::string> dropped;
    for (const auto& quantity : quantity_detail::parse(original)) {
        const bool present = std::any_of(candidates.begin(), candidates.end(), [&](const auto& other) {
            return quantity.key == other.key;
        });
        if (!present && std::find(dropped.begin(), dropped.end(), quantity.source) == dropped.end())
            dropped.push_back(quantity.source);
    }
    return dropped;
}

} // namespace roclint::core::intent
