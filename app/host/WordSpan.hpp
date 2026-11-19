#pragma once

#include <QString>
#include <optional>
#include <utility>

namespace roclint::host {
// UTF-16 offsets shared by observation geometry and the frozen model request.
// Geometry must attest this exact span; caret bounds are never a word estimate.
inline std::optional<std::pair<int, int>> wordSpanAtCaret(const QString& text, int caret) {
    if (caret < 0 || caret > text.size()) return std::nullopt;
    const auto word = [](QChar c) { return c.isLetterOrNumber() || c == '_' || c == '-'; };
    int start = caret, end = caret;
    while (end < text.size() && word(text[end])) ++end;
    while (start > 0 && word(text[start - 1])) --start;
    if (start == end) {
        while (start > 0) {
            const auto previous = text[start - 1];
            if (previous == QLatin1Char('\n') || word(previous) ||
                (!previous.isSpace() && !previous.isPunct())) break;
            --start;
        }
        end = start;
        while (start > 0 && word(text[start - 1])) --start;
    }
    if (start == end) return std::nullopt;
    return std::pair{start, end};
}
}
