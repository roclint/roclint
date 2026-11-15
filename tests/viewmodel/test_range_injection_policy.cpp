#include "RangeInjectionPolicy.hpp"
#include "support/check.hpp"

#include <iostream>

using roclint::host::RangeInjectionPolicy;

namespace {

void testExactUnicodeOffsets() {
    const QString text = QString::fromUtf8("Hello café 🚀 world");
    const int start = text.indexOf(QString::fromUtf8("🚀"));
    ROCLINT_CHECK(start > 0);
    ROCLINT_CHECK(RangeInjectionPolicy::offsetsValid(text, start, start + 2));
    ROCLINT_CHECK(RangeInjectionPolicy::rangeMatches(text, start, start + 2, QString::fromUtf8("🚀")));
    ROCLINT_CHECK(!RangeInjectionPolicy::rangeMatches(text, start, start + 1, QString::fromUtf8("🚀")));

    // The same UTF-16 offsets must remain valid for a BMP replacement of different display width.
    const QString expected = text.left(start) + QString::fromUtf8("✨") + text.mid(start + 2);
    ROCLINT_CHECK(RangeInjectionPolicy::readbackMatches(expected, expected));
}

void testStaleAndPartialWritesAbort() {
    const QString frozen = QStringLiteral("Hello world");
    const QString changed = QStringLiteral("Hello brave world");
    ROCLINT_CHECK(RangeInjectionPolicy::preconditionsHold(7, 7, frozen, frozen));
    ROCLINT_CHECK(!RangeInjectionPolicy::preconditionsHold(7, 8, frozen, frozen));   // focus moved
    ROCLINT_CHECK(!RangeInjectionPolicy::preconditionsHold(7, 7, frozen, changed)); // external edit
    ROCLINT_CHECK(!RangeInjectionPolicy::preconditionsHold(0, 0, frozen, frozen));  // missing identity

    // A partial write is never a valid fallback precondition.
    const QString partial = QStringLiteral("Hello Roc");
    ROCLINT_CHECK(!RangeInjectionPolicy::fallbackAllowed(frozen, partial, true, false));
    ROCLINT_CHECK(!RangeInjectionPolicy::fallbackAllowed(frozen, partial, false, false));
    ROCLINT_CHECK(RangeInjectionPolicy::fallbackAllowed(frozen, frozen, false, false));
    ROCLINT_CHECK(!RangeInjectionPolicy::fallbackAllowed(frozen, frozen, false, true));
}

void testRangeBounds() {
    const QString text = QStringLiteral("abc");
    ROCLINT_CHECK(!RangeInjectionPolicy::offsetsValid(text, -1, 1));
    ROCLINT_CHECK(!RangeInjectionPolicy::offsetsValid(text, 2, 2));
    ROCLINT_CHECK(!RangeInjectionPolicy::offsetsValid(text, 2, 1));
    ROCLINT_CHECK(!RangeInjectionPolicy::offsetsValid(text, 0, 4));
    ROCLINT_CHECK(RangeInjectionPolicy::offsetsValid(text, 0, 3));
    ROCLINT_CHECK(!RangeInjectionPolicy::rangeMatches(text, 0, 3, QString()));
}

} // namespace

int main() {
    std::cout << "[RangeInjectionPolicy] Running tests...\n";
    testExactUnicodeOffsets();
    testStaleAndPartialWritesAbort();
    testRangeBounds();
    std::cout << "[RangeInjectionPolicy] All tests PASSED.\n";
    return 0;
}
