#pragma once

#include <QString>

namespace roclint::host {

// Pure preconditions for a range write. The Windows accessibility bridge uses these before
// selecting or pasting, so a failed selection or a stale snapshot can never trigger a
// destructive full-field recovery based on old text.
struct RangeInjectionPolicy {
    // Offsets are UTF-16 indexes into the frozen field text.
    [[nodiscard]] static bool offsetsValid(const QString& expectedCurrentText, int startOffset, int endOffset) {
        return startOffset >= 0 && endOffset > startOffset && endOffset <= expectedCurrentText.size();
    }

    [[nodiscard]] static bool rangeMatches(const QString& expectedCurrentText, int startOffset, int endOffset,
                                           const QString& expectedRangeText) {
        if (expectedRangeText.isEmpty() || !offsetsValid(expectedCurrentText, startOffset, endOffset)) return false;
        return expectedCurrentText.mid(startOffset, endOffset - startOffset) == expectedRangeText;
    }

    // The field identity and exact contents must still match the frozen snapshot.
    [[nodiscard]] static bool preconditionsHold(quint64 frozenFieldToken, quint64 currentFieldToken,
                                                const QString& expectedCurrentText, const QString& currentText) {
        return frozenFieldToken != 0 && frozenFieldToken == currentFieldToken &&
               expectedCurrentText == currentText;
    }

    // A full-field fallback is allowed only when no write happened at all. A partial write or
    // an external edit keeps the actual text and aborts the operation.
    [[nodiscard]] static bool fallbackAllowed(const QString& expectedCurrentText, const QString& currentText,
                                              bool rangeWriteAttempted, bool rangeWriteSucceeded) {
        if (rangeWriteSucceeded) return false;
        if (!rangeWriteAttempted) return expectedCurrentText == currentText;
        return expectedCurrentText == currentText;
    }

    // Exact readback is the only success condition.
    [[nodiscard]] static bool readbackMatches(const QString& expectedFinalText, const QString& currentText) {
        return currentText == expectedFinalText;
    }
};

} // namespace roclint::host
