#include "roclint/rewrite/CanonicalIntent.hpp"

#include "roclint/analysis/SourceSpan.hpp"
#include "roclint/core/Json.hpp"
#include "roclint/core/crypto/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace roclint::core::rewrite {
namespace {

struct Clause {
    std::string text{};
    std::size_t startByte{0};
};

struct ActionMatch {
    IntentAction action{IntentAction::MODIFY};
    std::size_t verbStart{0};
    std::size_t verbLength{0};
    bool negated{false};
};

struct QuoteSpan {
    std::size_t start{0};
    std::size_t end{0};
};

const std::regex backtickRe(R"(`([^`\r\n]{1,120})`)", std::regex::optimize);
const std::regex codeTokenRe(
    R"(\b(?:[A-Za-z0-9_./\\-]+\.(?:cpp|cc|cxx|hpp|hh|h|c|py|js|ts|tsx|jsx|qml|html|css|scss|json|yaml|yml|md|txt|xml|rs|go|java|swift|sql|sh|ps1|bat)|[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\b)",
    std::regex::optimize);
const std::regex headingRe(R"(^\s*#{1,6}\s+(.+?)\s*$)", std::regex::optimize);
const std::regex durationRe(
    R"(\b([A-Za-z0-9.]+)\s*(milliseconds?|ms|seconds?|secs?|s|minutes?|mins?|m|hours?|hrs?|h)\b)",
    std::regex::icase | std::regex::optimize);
const std::regex valueAfterRe(
    R"(\b(?:to|as|=|must be|must remain|should be|should remain|cannot be|shall be|will be|set to)\s+(.+))",
    std::regex::icase | std::regex::optimize);

const std::set<std::string> kActionWords{
    "add", "append", "create", "delete", "drop", "eliminate", "enable", "ensure", "expand", "include",
    "insert", "keep", "limit", "modify", "move", "omit", "preserve", "refactor", "remove", "rename",
    "replace", "require", "retain", "rewrite", "set", "strip", "update", "change", "adjust", "hide",
    "collapse", "convert", "allow", "permit"};

const std::set<std::string> kPropertyWords{
    "color", "width", "height", "size", "timeout", "duration", "interval", "latency", "format",
    "extension", "filename", "path", "scope", "severity", "permission", "access", "order", "ordering",
    "case", "casing", "indentation", "spacing", "position", "alignment", "visibility", "state",
    "behavior", "performance", "memory", "throughput", "error", "status", "schema", "field", "key",
    "value", "count", "limit", "threshold", "recipient", "destination", "output", "input", "endpoint",
    "api", "name", "heading"};

const std::set<std::string> kStopWords{
    "a", "an", "the", "of", "for", "to", "in", "on", "at", "and", "or", "with", "by", "from", "as", "is",
    "are", "be", "this", "that", "these", "those", "every", "all", "each", "any", "its", "their", "it"};

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string trimClause(std::string_view text) {
    auto value = trim(text);
    while (!value.empty()) {
        const char back = value.back();
        if (back == '.' || back == '!' || back == '?' || back == ';' || back == ':' || back == ',') value.pop_back();
        else break;
    }
    return trim(value);
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return out;
}

std::string collapseWhitespace(std::string_view text) {
    std::string out;
    bool pending = false;
    for (const char character : text) {
        if (std::isspace(static_cast<unsigned char>(character))) {
            pending = !out.empty();
            continue;
        }
        if (pending) {
            out.push_back(' ');
            pending = false;
        }
        out.push_back(character);
    }
    return trim(out);
}

bool isWordChar(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_' || character == '-';
}

[[maybe_unused]] std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    for (const char character : text) {
        if (isWordChar(character)) current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        else if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

std::string cleanTarget(std::string_view text) {
    auto value = collapseWhitespace(text);
    while (value.size() >= 2) {
        if (value.starts_with("the ")) value.erase(0, 4);
        else if (value.starts_with("a ")) value.erase(0, 2);
        else if (value.starts_with("an ")) value.erase(0, 3);
        else break;
    }
    while (!value.empty()) {
        const char back = value.back();
        if (back == '.' || back == ',' || back == ';' || back == ':' || back == '!' || back == '?') value.pop_back();
        else break;
    }
    return trim(value);
}

[[maybe_unused]] std::size_t findPhrase(const std::string& lowerText, std::string_view phrase) {
    return lowerText.find(phrase);
}

bool isNegated(const std::string& lowerText, std::size_t verbStart) {
    const auto prefix = lowerText.substr(0, std::min(verbStart, lowerText.size()));
    for (const auto* phrase : {"do not", "don't", "does not", "must not", "should not", "never", "without"}) {
        if (prefix.find(phrase) != std::string::npos) return true;
    }
    return false;
}

std::optional<ActionMatch> detectAction(const std::string& lowerText) {
    struct Candidate {
        IntentAction action;
        const char* phrase;
        bool negatedTarget;
    };
    static const Candidate kCandidates[] = {
        {IntentAction::PRESERVE, "do not change", false},
        {IntentAction::PRESERVE, "do not modify", false},
        {IntentAction::PRESERVE, "do not alter", false},
        {IntentAction::PRESERVE, "do not remove", false},
        {IntentAction::PRESERVE, "do not touch", false},
        {IntentAction::PRESERVE, "leave", false},
        {IntentAction::PRESERVE, "preserve", false},
        {IntentAction::PRESERVE, "retain", false},
        {IntentAction::PRESERVE, "keep", false},
        {IntentAction::PRESERVE, "unchanged", false},
        {IntentAction::PRESERVE, "must remain", false},
        {IntentAction::PRESERVE, "must stay", false},
        {IntentAction::REMOVE, "remove", false},
        {IntentAction::REMOVE, "delete", false},
        {IntentAction::REMOVE, "drop", false},
        {IntentAction::REMOVE, "eliminate", false},
        {IntentAction::REMOVE, "omit", false},
        {IntentAction::REMOVE, "hide", false},
        {IntentAction::REMOVE, "strip", false},
        {IntentAction::ADD, "add", false},
        {IntentAction::ADD, "include", false},
        {IntentAction::ADD, "insert", false},
        {IntentAction::ADD, "append", false},
        {IntentAction::ADD, "create", false},
        {IntentAction::ADD, "introduce", false},
        {IntentAction::ADD, "enable", false},
        {IntentAction::ADD, "allow", false},
        {IntentAction::ADD, "permit", false},
        {IntentAction::MODIFY, "rename", false},
        {IntentAction::MODIFY, "change", false},
        {IntentAction::MODIFY, "update", false},
        {IntentAction::MODIFY, "replace", false},
        {IntentAction::MODIFY, "refactor", false},
        {IntentAction::MODIFY, "move", false},
        {IntentAction::MODIFY, "set", false},
        {IntentAction::MODIFY, "adjust", false},
        {IntentAction::MODIFY, "modify", false},
        {IntentAction::MODIFY, "rewrite", false},
        {IntentAction::MODIFY, "convert", false},
        {IntentAction::MODIFY, "expand", false},
        {IntentAction::MODIFY, "collapse", false},
        {IntentAction::CONSTRAIN, "must", false},
        {IntentAction::CONSTRAIN, "shall", false},
        {IntentAction::CONSTRAIN, "should", false},
        {IntentAction::CONSTRAIN, "only", false},
        {IntentAction::CONSTRAIN, "never", false},
        {IntentAction::CONSTRAIN, "cannot", false},
        {IntentAction::CONSTRAIN, "can't", false},
        {IntentAction::CONSTRAIN, "limit", false},
        {IntentAction::CONSTRAIN, "maximum", false},
        {IntentAction::CONSTRAIN, "minimum", false},
        {IntentAction::CONSTRAIN, "at most", false},
        {IntentAction::CONSTRAIN, "within", false},
        {IntentAction::CONSTRAIN, "require", false},
        {IntentAction::CONSTRAIN, "ensure", false},
        {IntentAction::CONSTRAIN, "use", false},
    };

    std::optional<ActionMatch> best;
    for (const auto& candidate : kCandidates) {
        const std::string phrase(candidate.phrase);
        std::size_t position = 0;
        while ((position = lowerText.find(phrase, position)) != std::string::npos) {
            const bool leftBoundary = position == 0 || !isWordChar(lowerText[position - 1]);
            const auto right = position + phrase.size();
            const bool rightBoundary = right >= lowerText.size() || !isWordChar(lowerText[right]);
            if (leftBoundary && rightBoundary) {
                if (!best.has_value() || position < best->verbStart) {
                    best = ActionMatch{candidate.action, position, phrase.size(), isNegated(lowerText, position)};
                }
                break;
            }
            ++position;
        }
    }
    if (!best.has_value()) return best;
    if (best->negated) {
        if (best->action == IntentAction::ADD) best->action = IntentAction::CONSTRAIN;
        else if (best->action == IntentAction::REMOVE || best->action == IntentAction::MODIFY)
            best->action = IntentAction::PRESERVE;
    }
    return best;
}

std::vector<Clause> splitClauses(std::string_view text) {
    std::vector<Clause> clauses;
    std::size_t start = 0;
    auto push = [&](std::size_t end) {
        if (end <= start) return;
        auto contentStart = start;
        while (contentStart < end && std::isspace(static_cast<unsigned char>(text[contentStart]))) ++contentStart;
        const auto sentence = trimClause(text.substr(contentStart, end - contentStart));
        if (!sentence.empty()) clauses.push_back({sentence, contentStart});
    };
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        bool boundary = false;
        if (character == '\n' || character == '\r' || character == ';') {
            boundary = true;
        } else if (character == '!' || character == '?') {
            boundary = index + 1 >= text.size() || std::isspace(static_cast<unsigned char>(text[index + 1]));
        } else if (character == '.') {
            boundary = index + 1 >= text.size() || std::isspace(static_cast<unsigned char>(text[index + 1]));
        }
        if (!boundary) continue;
        push(index);
        start = index + 1;
    }
    if (start < text.size()) {
        push(text.size());
    }
    return clauses;
}

std::vector<QuoteSpan> quotedSpans(std::string_view text) {
    std::vector<QuoteSpan> spans;
    std::size_t index = 0;
    while (index < text.size()) {
        const char character = text[index];
        if (character != '"' && character != '\'') {
            ++index;
            continue;
        }
        if (character == '\'' && index > 0 && index + 1 < text.size() &&
            std::isalnum(static_cast<unsigned char>(text[index - 1])) &&
            std::isalnum(static_cast<unsigned char>(text[index + 1]))) {
            ++index;
            continue;
        }
        std::size_t delimiterLength = 1;
        if (character == '"' && index + 2 < text.size() && text.substr(index, 3) == "\"\"\"") delimiterLength = 3;
        const auto delimiter = text.substr(index, delimiterLength);
        const auto close = text.find(delimiter, index + delimiterLength);
        if (close == std::string_view::npos) {
            spans.push_back({index, text.size()});
            break;
        }
        spans.push_back({index, close + delimiterLength});
        index = close + delimiterLength;
    }
    return spans;
}

std::string maskQuotedData(std::string_view text) {
    std::string masked(text);
    const auto mask = [&](std::size_t begin, std::size_t end) {
        // Preserve offsets and sentence boundaries outside the literal. Quoted
        // verbs, punctuation and identifiers cannot become executable intents.
        std::fill(masked.begin() + begin, masked.begin() + end, ' ');
    };
    for (std::size_t begin = 0; (begin = text.find("```", begin)) != std::string_view::npos;) {
        const auto close = text.find("```", begin + 3);
        const auto end = close == std::string_view::npos ? text.size() : close + 3;
        mask(begin, end);
        begin = end;
    }
    // Inline code can name a symbol/path/signature, but a backticked prose
    // example must not contribute actions. Keep actual code references intact.
    static const std::regex inlineReference(
        R"(^(?:[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*(?:\([^`\r\n]*\))?|[A-Za-z0-9_./\\:-]+)$)",
        std::regex::optimize);
    for (std::size_t begin = 0; (begin = masked.find('`', begin)) != std::string::npos;) {
        const auto close = masked.find('`', begin + 1);
        if (close == std::string::npos) {
            mask(begin, masked.size());
            break;
        }
        if (!std::regex_match(masked.substr(begin + 1, close - begin - 1), inlineReference)) mask(begin, close + 1);
        begin = close + 1;
    }
    // Quotes inside a fenced example cannot open a quote spanning subsequent
    // instructions outside that fence.
    for (const auto& span : quotedSpans(masked)) {
        // Preserve quoted identifiers/paths, especially explicit scope and file
        // bounds. Erasing "Missing" from 'in "Missing"' would widen to global.
        const auto literal = std::string_view(masked).substr(span.start, span.end - span.start);
        const bool codeReference = literal.size() >= 2 && literal.front() == literal.back() &&
            std::regex_match(std::string(literal.substr(1, literal.size() - 2)), inlineReference);
        if (!codeReference) mask(span.start, span.end);
    }
    return masked;
}

bool looksLikeCodeToken(const std::string& token) {
    if (token.find('_') != std::string::npos || token.find("::") != std::string::npos ||
        token.find('.') != std::string::npos || token.find('/') != std::string::npos ||
        token.find('\\') != std::string::npos) return true;
    for (std::size_t index = 1; index < token.size(); ++index) {
        if (std::isupper(static_cast<unsigned char>(token[index])) &&
            std::islower(static_cast<unsigned char>(token[index - 1]))) return true;
    }
    return false;
}

std::string firstCodeTarget(const std::string& text) {
    std::smatch match;
    if (std::regex_search(text, match, backtickRe)) return cleanTarget(match[1].str());
    for (std::sregex_iterator it(text.begin(), text.end(), codeTokenRe), end; it != end; ++it) {
        const auto token = it->str();
        if (looksLikeCodeToken(token) || token.find('.') != std::string::npos) return token;
    }
    if (std::regex_search(text, match, headingRe)) return cleanTarget(match[1].str());
    return {};
}

std::string phraseAfterVerb(const std::string& clause, const ActionMatch& action) {
    const auto begin = std::min(clause.size(), action.verbStart + action.verbLength);
    auto phrase = cleanTarget(std::string_view(clause).substr(begin));
    // A qualified identifier at the action's object boundary is an exact name,
    // not the prose following it. Never reduce explicit unsupported syntax to
    // a supported prefix (templates, signatures, member access or operators).
    static const std::regex qualifiedPrefix(
        R"(^(?:(?:all|both)\s+)?([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)+)(?=$|[\s,;]))",
        std::regex::optimize);
    std::smatch matched;
    if (std::regex_search(phrase, matched, qualifiedPrefix)) {
        const auto remainder = trim(std::string_view(phrase).substr(matched.length()));
        static const std::regex declaratorTail(R"(^(?:const|volatile|noexcept|requires|override|final)\b)",
                                               std::regex::optimize);
        if (!std::regex_search(remainder, declaratorTail) &&
            (remainder.empty() || std::isalpha(static_cast<unsigned char>(remainder.front())) ||
             remainder.front() == ',' || remainder.front() == ';')) return matched[1].str();
    }
    for (const auto* marker : {" in ", " on ", " for ", " with ", " to ", " under ", " except ", " unless ",
                               " if ", " when ", " only if "}) {
        const auto position = lower(phrase).find(marker);
        if (position != std::string::npos) phrase = cleanTarget(phrase.substr(0, position));
    }
    return phrase;
}

std::string subjectBeforeVerb(const std::string& clause, const ActionMatch& action) {
    auto phrase = cleanTarget(std::string_view(clause).substr(0, action.verbStart));
    const auto cut = phrase.find_last_of(" ,;:");
    if (cut != std::string::npos) phrase = cleanTarget(phrase.substr(cut + 1));
    return phrase;
}

std::string detectProperty(const std::string& text, IntentAction action, const std::string& target) {
    const auto lowerText = lower(text);
    const auto targetLower = lower(target);
    for (const auto& property : kPropertyWords) {
        // A field elsewhere in the task describes behavior, not the target's
        // AST kind. Only an explicit field noun in the target can constrain it.
        if (property == "field") {
            static const std::regex fieldNoun(R"((?:^|\s)fields?(?:$|\s))", std::regex::optimize);
            if (!std::regex_search(targetLower, fieldNoun)) continue;
        }
        if (lowerText.find(property) != std::string::npos || targetLower.find(property) != std::string::npos)
            return property;
    }
    if (action == IntentAction::ADD || action == IntentAction::REMOVE) return "membership";
    if (action == IntentAction::PRESERVE) return "invariant";
    if (lowerText.find("rename") != std::string::npos || lowerText.find("name") != std::string::npos) return "name";
    if (lowerText.find("must be") != std::string::npos || lowerText.find("should be") != std::string::npos)
        return "requirement";
    return "behavior";
}

std::string cutValue(std::string_view text) {
    auto value = cleanTarget(text);
    for (const auto* marker : {" in ", " on ", " for ", " with ", " except ", " unless ", " only if ",
                               " if ", " when ", " under ", " within "}) {
        const auto position = lower(value).find(marker);
        if (position != std::string::npos) value = cleanTarget(value.substr(0, position));
    }
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\'') ||
                              (value.front() == '`' && value.back() == '`'))) {
        value = value.substr(1, value.size() - 2);
    }
    return trim(value);
}

std::string normalizeDuration(std::string value) {
    std::smatch match;
    if (!std::regex_search(value, match, durationRe)) return value;
    const auto numberText = lower(match[1].str());
    double number = 0;
    try {
        number = std::stod(numberText);
    } catch (...) {
        return value;
    }
    const auto unit = lower(match[2].str());
    double seconds = number;
    if (unit == "ms" || unit.starts_with("milli")) seconds = number / 1000.0;
    else if (unit == "m" || unit.starts_with("min")) seconds = number * 60.0;
    else if (unit == "h" || unit.starts_with("hour") || unit.starts_with("hr")) seconds = number * 3600.0;
    const auto rendered = std::to_string(seconds);
    auto trimmed = rendered;
    while (trimmed.size() > 1 && trimmed.back() == '0') trimmed.pop_back();
    if (!trimmed.empty() && trimmed.back() == '.') trimmed.pop_back();
    return trimmed + " seconds";
}

std::string extractValue(const std::string& clause, IntentAction action) {
    std::smatch match;
    if (std::regex_search(clause, match, valueAfterRe)) {
        auto value = cutValue(match[1].str());
        if (!value.empty()) return normalizeDuration(value);
    }
    switch (action) {
        case IntentAction::ADD: return "present";
        case IntentAction::REMOVE: return "absent";
        case IntentAction::PRESERVE: return "unchanged";
        case IntentAction::CONSTRAIN: return "required";
        case IntentAction::MODIFY: return "changed";
    }
    return {};
}

std::string extractCondition(const std::string& clause) {
    const auto lowerClause = lower(clause);
    std::size_t first = std::string::npos;
    for (const auto* marker : {" only if ", " only when ", " except ", " unless ", " provided ", " if ",
                               " when ", " while ", " in ", " on ", " for ", " with ",
                               " within ", " inside ", " from "}) {
        const auto position = lowerClause.find(marker);
        first = std::min(first, position);
    }
    // Keep the entire trailing context. Choosing 'if' ahead of an earlier file
    // clause would discard an explicit bound and permit a global lookup.
    return first == std::string::npos ? std::string{} : cleanTarget(std::string_view(clause).substr(first + 1));
}

std::string normalizedNodeKey(const CanonicalIntent& intent) {
    return std::string(toString(intent.action)) + '\x1f' + intent.target + '\x1f' + intent.property + '\x1f' +
           intent.value + '\x1f' + intent.condition + '\x1f' + std::to_string(intent.source.startByte) + ':' +
           std::to_string(intent.source.endByte);
}

} // namespace

const char* toString(IntentAction action) noexcept {
    switch (action) {
        case IntentAction::ADD: return "ADD";
        case IntentAction::REMOVE: return "REMOVE";
        case IntentAction::MODIFY: return "MODIFY";
        case IntentAction::CONSTRAIN: return "CONSTRAIN";
        case IntentAction::PRESERVE: return "PRESERVE";
    }
    return "MODIFY";
}

std::string CanonicalIntentExtractor::makeNodeId(const CanonicalIntent& intent) {
    const auto digest = crypto::Sha256::hexDigest(normalizedNodeKey(intent));
    return "nid_" + digest.substr(0, 24);
}

std::string CanonicalIntentExtractor::toJson(const std::vector<CanonicalIntent>& intents) {
    auto array = core::json::Value::array();
    for (const auto& intent : intents) {
        auto object = core::json::Value::object();
        object.set("action", core::json::Value::string(toString(intent.action)));
        object.set("target", core::json::Value::string(intent.target));
        object.set("property", core::json::Value::string(intent.property));
        object.set("value", core::json::Value::string(intent.value));
        object.set("disposition", core::json::Value::string(intent.disposition));
        object.set("condition", core::json::Value::string(intent.condition));
        object.set("node_id", core::json::Value::string(intent.nodeId));
        object.set("source_start", core::json::Value::number(static_cast<double>(intent.source.startByte)));
        object.set("source_end", core::json::Value::number(static_cast<double>(intent.source.endByte)));
        array.push(std::move(object));
    }
    return core::json::serialize(array);
}

std::vector<CanonicalIntent> CanonicalIntentExtractor::extract(std::string_view promptText) {
    std::vector<CanonicalIntent> intents;
    if (promptText.empty() || promptText.size() > kMaxPromptBytes) return intents;

    const auto instructionText = maskQuotedData(promptText);
    std::set<std::string> seen;
    for (const auto& clause : splitClauses(instructionText)) {
        const auto lowerClause = lower(clause.text);
        const auto action = detectAction(lowerClause);
        if (!action.has_value()) continue;

        std::string target;
        if (action->action == IntentAction::CONSTRAIN &&
            (lowerClause.find("must") != std::string::npos || lowerClause.find("should") != std::string::npos ||
             lowerClause.find("shall") != std::string::npos)) {
            target = subjectBeforeVerb(clause.text, *action);
        }
        if (target.empty()) target = phraseAfterVerb(clause.text, *action);
        if (target.empty() || kActionWords.contains(lower(target))) target = firstCodeTarget(clause.text);
        if (target.empty()) continue;
        if (kActionWords.contains(lower(target))) continue;

        CanonicalIntent intent;
        intent.action = action->action;
        intent.target = cleanTarget(target);
        intent.property = detectProperty(clause.text, intent.action, intent.target);
        intent.value = extractValue(clause.text, intent.action);
        intent.condition = extractCondition(clause.text);
        intent.disposition = "explicit_prompt";

        const auto targetSpan = analysis::findUniqueSpan(promptText, intent.target);
        if (targetSpan.status == analysis::SpanMatchStatus::ExactMatch && targetSpan.span.has_value())
            intent.source = *targetSpan.span;
        else
            intent.source = {clause.startByte, std::min(promptText.size(), clause.startByte + clause.text.size())};
        intent.nodeId = makeNodeId(intent);

        const auto key = intent.nodeId + '\x1f' + intent.condition;
        if (!seen.insert(key).second) continue;
        intents.push_back(std::move(intent));
        if (intents.size() >= kMaxIntents) break;
    }
    std::sort(intents.begin(), intents.end(), [](const CanonicalIntent& left, const CanonicalIntent& right) {
        if (left.source.startByte != right.source.startByte) return left.source.startByte < right.source.startByte;
        return left.nodeId < right.nodeId;
    });
    return intents;
}

} // namespace roclint::core::rewrite
