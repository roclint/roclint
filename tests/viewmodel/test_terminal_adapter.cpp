#include "TerminalAdapter.hpp"
#include "support/check.hpp"

#include <iostream>

using roclint::host::TerminalAdapter;

namespace {

QString draftOf(const QString& buffer, qsizetype caret = -1) {
    const auto d = TerminalAdapter::extractDraft(buffer, caret);
    ROCLINT_CHECK(d.has_value());
    return d->text;
}

void testClaudeCodeChevronAfterOutput() {
    const QString buffer = QString::fromUtf8(
        "Running tests...\n"
        "3 passed, 1 failed > see log\n"
        "\n"
        "\xE2\x9D\xAF fix the failing test in the parser");
    const auto d = TerminalAdapter::extractDraft(buffer, buffer.size());
    ROCLINT_CHECK(d.has_value());
    ROCLINT_CHECK(d->text == "fix the failing test in the parser");
    ROCLINT_CHECK(d->delimiter == QChar(0x276F));
    ROCLINT_CHECK(buffer.mid(d->delimiterOffset, 1) == QString(QChar(0x276F)));
    std::cout << "  [PASS] Chevron prompt: stdout above the prompt line is excluded.\n";
}

void testBoxedInputWithBorders() {
    const QString buffer = QString::fromUtf8(
        "\xE2\x97\x8F I updated the config.\n"
        "\xE2\x95\xAD\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x95\xAE\n"
        "\xE2\x94\x82 > refactor the auth module            \xE2\x94\x82\n"
        "\xE2\x94\x82   and keep the public API              \xE2\x94\x82");
    ROCLINT_CHECK(draftOf(buffer) == "refactor the auth module\nand keep the public API");
    std::cout << "  [PASS] Boxed multi-line input: borders and padding are removed.\n";
}

void testShellDollarPrompt() {
    const QString buffer = QStringLiteral("total 4\ndrwxr-xr-x 2 kirby\nkirby@box:~/src$ codex \"add caching\"");
    ROCLINT_CHECK(draftOf(buffer) == "codex \"add caching\"");
    std::cout << "  [PASS] Shell '$' prompt with a path prefix.\n";
}

void testPowerShellAngleAndInlineGreaterThan() {
    const QString buffer = QStringLiteral("PS C:\\Users\\kirby> echo a > out.txt");
    ROCLINT_CHECK(draftOf(buffer) == "echo a > out.txt");
    std::cout << "  [PASS] The first delimiter on the line wins; '>' inside the draft is kept.\n";
}

void testCaretInTheMiddle() {
    const QString buffer = QStringLiteral("> rename getCwd everywhere");
    const qsizetype caret = buffer.indexOf(QStringLiteral(" everywhere"));
    ROCLINT_CHECK(draftOf(buffer, caret) == "rename getCwd");
    std::cout << "  [PASS] Only the text before the caret is the draft.\n";
}

void testNoDelimiterAndEmptyDraft() {
    ROCLINT_CHECK(!TerminalAdapter::extractDraft(QStringLiteral("just output\nno prompt here"), -1).has_value());
    ROCLINT_CHECK(!TerminalAdapter::extractDraft(QString(), 0).has_value());
    ROCLINT_CHECK(draftOf(QStringLiteral("done\n$ ")).isEmpty());
    // A '>' that isn't followed by a space is not a delimiter.
    ROCLINT_CHECK(!TerminalAdapter::extractDraft(QStringLiteral("a->b"), -1).has_value());
    std::cout << "  [PASS] Missing delimiters and empty drafts are reported honestly.\n";
}

void testDelimiterTooFarAboveIsIgnored() {
    QString buffer = QStringLiteral("> old prompt\n");
    for (int i = 0; i < TerminalAdapter::kMaxDraftLines + 2; ++i) {
        buffer += QStringLiteral("output line %1\n").arg(i);
    }
    buffer += QStringLiteral("still output");
    ROCLINT_CHECK(!TerminalAdapter::extractDraft(buffer, -1).has_value());
    std::cout << "  [PASS] A delimiter beyond the scan window is not used.\n";
}

void testReplacementKeysNeverSetValue() {
    using K = TerminalAdapter::Key;
    const auto readline = TerminalAdapter::replacementKeys(false);
    ROCLINT_CHECK((readline == std::vector<K>{K::LineEnd, K::ClearLine, K::Paste}));
    const auto vim = TerminalAdapter::replacementKeys(true);
    ROCLINT_CHECK((vim == std::vector<K>{K::Escape, K::AppendAtLineEnd, K::ClearLine, K::Paste}));
    std::cout << "  [PASS] Terminal replacement clears the line and pastes.\n";
}

} // namespace

int main() {
    std::cout << "[Test TerminalAdapter] Starting...\n";
    testClaudeCodeChevronAfterOutput();
    testBoxedInputWithBorders();
    testShellDollarPrompt();
    testPowerShellAngleAndInlineGreaterThan();
    testCaretInTheMiddle();
    testNoDelimiterAndEmptyDraft();
    testDelimiterTooFarAboveIsIgnored();
    testReplacementKeysNeverSetValue();
    std::cout << "[Test TerminalAdapter] All tests PASSED.\n";
    return 0;
}
