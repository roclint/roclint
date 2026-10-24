#pragma once

#include <QString>
#include <QStringView>

#include <optional>
#include <vector>

namespace roclint::host {

// The unsubmitted input of a CLI agent (Claude Code, Codex CLI, a shell) inside a terminal buffer
// that mixes program output with the line being typed.
struct TerminalDraft {
    QString text;                 // The draft, with box-drawing borders and padding removed
    QChar delimiter;              // The prompt delimiter that starts the draft
    qsizetype delimiterOffset{-1}; // Offset of the delimiter in the buffer
};

class TerminalAdapter {
public:
    // Prompt delimiters, in no particular order: Claude Code and zsh themes use U+276F, many CLIs
    // use '>', POSIX shells use '$'.
    static constexpr char16_t kDelimiters[] = {u'\u276F', u'>', u'$'};
    // How far above the caret line a multi-line draft may start.
    static constexpr int kMaxDraftLines = 20;

    // Scans backward from caretOffset (end of buffer when out of range) to the nearest line that
    // holds a prompt delimiter followed by a space or the end of the line, and returns the text
    // between that delimiter and the caret. Returns nullopt when no delimiter is found.
    [[nodiscard]] static std::optional<TerminalDraft> extractDraft(QStringView buffer, qsizetype caretOffset);

    enum class Key {
        Escape,
        AppendAtLineEnd, // vim 'A'
        LineEnd,         // readline Ctrl+E
        ClearLine,       // readline Ctrl+U (unix-line-discard)
        Paste            // platform paste (Ctrl+V / Cmd+V) of the replacement text
    };

    // Keys that clear the draft and paste the replacement. SetValue is never used on terminals.
    [[nodiscard]] static std::vector<Key> replacementKeys(bool vimMode);
};

} // namespace roclint::host
