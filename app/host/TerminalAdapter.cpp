#include "TerminalAdapter.hpp"

#include <QStringList>
#include <QSysInfo>

namespace roclint::host {

namespace {

bool isBorder(QChar c) {
    const char16_t u = c.unicode();
    return u == u'|' || u == u'\u2502' || u == u'\u2503' || u == u'\u2551' || u == u'\u258C';
}

bool isDelimiter(QChar c) {
    for (const char16_t d : TerminalAdapter::kDelimiters) {
        if (c.unicode() == d) {
            return true;
        }
    }
    return false;
}

// Strips leading/trailing whitespace and box-drawing borders from one buffer line.
QString stripFrame(QStringView line) {
    qsizetype begin = 0;
    qsizetype end = line.size();
    while (begin < end && (line[begin].isSpace() || isBorder(line[begin]))) {
        ++begin;
    }
    while (end > begin && (line[end - 1].isSpace() || isBorder(line[end - 1]))) {
        --end;
    }
    return line.mid(begin, end - begin).toString();
}

// The macOS login shell uses "user@host directory % ". Plain zsh also
// uses "hostname% ". Require a shell-shaped prefix to exclude percentages.
bool isZshPrompt(QStringView prefix) {
    if (prefix.isEmpty()) return true;
    if (!prefix.back().isSpace()) {
        static const QString host = QSysInfo::machineHostName().section(QLatin1Char('.'), 0, 0);
        return !host.isEmpty() && prefix.compare(host, Qt::CaseInsensitive) == 0;
    }
    const auto login = prefix.left(prefix.indexOf(QLatin1Char(' ')));
    const auto at = login.indexOf(QLatin1Char('@'));
    return at > 0 && at + 1 < login.size();
}

// Offset (within line) of the first delimiter that is followed by a space or ends the line.
qsizetype findDelimiter(QStringView line) {
    for (qsizetype i = 0; i < line.size(); ++i) {
        const bool zsh = line[i] == QLatin1Char('%') && isZshPrompt(line.first(i));
        if (!zsh && !isDelimiter(line[i])) {
            continue;
        }
        if (i + 1 == line.size() || line[i + 1] == QLatin1Char(' ') || line[i + 1] == QChar(0x00A0)) {
            return i;
        }
    }
    return -1;
}

} // namespace

std::optional<TerminalDraft> TerminalAdapter::extractDraft(QStringView buffer, qsizetype caretOffset) {
    if (buffer.isEmpty()) {
        return std::nullopt;
    }
    if (caretOffset < 0 || caretOffset > buffer.size()) {
        caretOffset = buffer.size();
    }

    // Walk up from the caret line, collecting line starts, until a delimiter line is found.
    qsizetype lineEnd = caretOffset;
    qsizetype lineStart = buffer.lastIndexOf(QLatin1Char('\n'), caretOffset > 0 ? caretOffset - 1 : 0);
    lineStart = (lineStart < 0 || lineStart >= caretOffset) ? 0 : lineStart + 1;
    if (caretOffset > 0 && buffer[caretOffset - 1] == QLatin1Char('\n')) {
        lineStart = caretOffset;
    }

    for (int scanned = 0; scanned <= kMaxDraftLines; ++scanned) {
        const QStringView line = buffer.mid(lineStart, lineEnd - lineStart);
        const qsizetype at = findDelimiter(line);
        if (at >= 0) {
            const qsizetype delimiterOffset = lineStart + at;
            qsizetype draftStart = delimiterOffset + 1;
            if (draftStart < caretOffset && (buffer[draftStart] == QLatin1Char(' ') || buffer[draftStart] == QChar(0x00A0))) {
                ++draftStart;
            }
            const QStringView raw = buffer.mid(draftStart, caretOffset - draftStart);
            QStringList lines;
            for (const auto part : raw.split(QLatin1Char('\n'))) {
                lines.append(stripFrame(part));
            }
            while (!lines.isEmpty() && lines.last().isEmpty()) {
                lines.removeLast();
            }
            TerminalDraft draft;
            draft.text = lines.join(QLatin1Char('\n'));
            draft.delimiter = buffer[delimiterOffset];
            draft.delimiterOffset = delimiterOffset;
            return draft;
        }
        if (lineStart == 0) {
            break;
        }
        lineEnd = lineStart - 1;
        const qsizetype previous = lineEnd > 0 ? buffer.lastIndexOf(QLatin1Char('\n'), lineEnd - 1) : -1;
        lineStart = previous < 0 ? 0 : previous + 1;
    }
    return std::nullopt;
}

std::vector<TerminalAdapter::Key> TerminalAdapter::replacementKeys(bool vimMode) {
    if (vimMode) {
        return {Key::Escape, Key::AppendAtLineEnd, Key::ClearLine, Key::Paste};
    }
    return {Key::LineEnd, Key::ClearLine, Key::Paste};
}

} // namespace roclint::host
