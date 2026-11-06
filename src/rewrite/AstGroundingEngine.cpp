#include "roclint/rewrite/AstGroundingEngine.hpp"

#include "roclint/core/crypto/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace roclint::core::rewrite {
namespace {

// Exact lookups only. FTS/fuzzy fallback is deliberately disabled for grounding.
constexpr std::size_t kQueryLimit = 64;
constexpr std::size_t kScopeQueryLimit = 64;
constexpr std::size_t kParameterChildLimit = 64;

// Prompt rendering bounds. The C10 ceiling is 350 tokens. The pinned tokenizer
// is a byte-level BPE, so every token covers at least one UTF-8 byte and a
// complete 350-byte document is a valid 350-token worst-case bound. The
// signature budget is caller-controlled (default 300 bytes) and remains a
// separate sub-limit; it can never widen the total document budget.
constexpr std::size_t kMaximumCandidateLabelBytes = 160;
constexpr std::size_t kMaximumCandidateBudgetBytes = AstGroundingEngine::kMaximumPromptBytes;

const std::regex scopeRe(
    R"(\b(?:in|within|inside)\s+((?:`[^`]+`|\*\*[^*]+\*\*|__[^_]+__|"[^"]+"|'[^']+'|[A-Za-z_][A-Za-z0-9_:<>]*)))",
    std::regex::icase | std::regex::optimize);

const std::regex fileRe(
    R"(\bfile\s+((?:`[^`]+`|\*\*[^*]+\*\*|__[^_]+__|"[^"]+"|'[^']+'|[A-Za-z0-9_./\\:-]+)))",
    std::regex::icase | std::regex::optimize);
const std::regex filePrepositionRe(R"(\b(?:in|within|inside|from|at)\s*$)",
                                    std::regex::icase | std::regex::optimize);

std::string trimText(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

// Removes code-formatting wrappers while preserving the token itself. Paths use
// this form too, so a trailing dot in a filename is not mistaken for punctuation.
std::string stripWrappers(std::string_view raw) {
    auto text = trimText(raw);
    bool changed = true;
    while (changed) {
        changed = false;
        if (text.size() >= 2 && text.front() == '`' && text.back() == '`') {
            text = trimText(std::string_view(text).substr(1, text.size() - 2));
            changed = true;
        } else if (text.size() >= 4 && text.starts_with("**") && text.ends_with("**")) {
            text = trimText(std::string_view(text).substr(2, text.size() - 4));
            changed = true;
        } else if (text.size() >= 4 && text.starts_with("__") && text.ends_with("__")) {
            text = trimText(std::string_view(text).substr(2, text.size() - 4));
            changed = true;
        } else if (text.size() >= 2 &&
                   ((text.front() == '"' && text.back() == '"') ||
                    (text.front() == '\'' && text.back() == '\''))) {
            text = trimText(std::string_view(text).substr(1, text.size() - 2));
            changed = true;
        }
    }
    return text;
}

// Strips code-formatting wrappers that can survive canonical extraction. The
// intent keeps its original text; only symbol matching uses this normalized form.
std::string normalizeIdentifier(std::string_view raw) {
    auto text = stripWrappers(raw);
    while (!text.empty() && (text.back() == '.' || text.back() == ',' || text.back() == ';' ||
                             text.back() == '!' || text.back() == '?')) {
        text.pop_back();
    }
    return trimText(text);
}

// File constraints are workspace-relative and exact. Only separator spelling is
// normalized; no suffix, case-folded or absolute-path matching is performed.
std::string normalizeFilePath(std::string_view raw) {
    auto text = stripWrappers(raw);
    std::replace(text.begin(), text.end(), '\\', '/');
    while (text.starts_with("./")) text.erase(0, 2);
    return text;
}

std::string targetName(std::string_view normalizedTarget) {
    const auto separator = normalizedTarget.rfind("::");
    return std::string(separator == std::string_view::npos ? normalizedTarget
                                                           : normalizedTarget.substr(separator + 2));
}

std::string directoryOf(std::string_view path) {
    const auto separator = path.find_last_of("/\\");
    return std::string(separator == std::string_view::npos ? std::string_view{} : path.substr(0, separator));
}

bool isUtf8Continuation(unsigned char value) noexcept {
    return (value & 0xC0U) == 0x80U;
}

// Returns the length of the valid UTF-8 sequence at `offset`, or 0 when the
// bytes are invalid (truncated, overlong, surrogate or out of range).
std::size_t utf8SequenceLength(std::string_view text, std::size_t offset) noexcept {
    if (offset >= text.size()) return 0;
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80U) return 1;

    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if ((first & 0xE0U) == 0xC0U) {
        length = 2;
        codepoint = first & 0x1FU;
    } else if ((first & 0xF0U) == 0xE0U) {
        length = 3;
        codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
        length = 4;
        codepoint = first & 0x07U;
    } else {
        return 0;
    }
    if (offset + length > text.size()) return 0;
    for (std::size_t index = 1; index < length; ++index) {
        const auto next = static_cast<unsigned char>(text[offset + index]);
        if (!isUtf8Continuation(next)) return 0;
        codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 2 && codepoint < 0x80U) || (length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return 0;
    }
    return length;
}

// Keeps rendered prompt text valid UTF-8 and valid XML 1.0. Invalid bytes and
// control characters that XML forbids are replaced rather than emitted.
std::string sanitizeXmlText(std::string_view text) {
    std::string sanitized;
    sanitized.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        const auto length = utf8SequenceLength(text, offset);
        if (length == 0) {
            sanitized.push_back('?');
            ++offset;
            continue;
        }
        if (length == 1) {
            const auto value = static_cast<unsigned char>(text[offset]);
            if (value < 0x20U && value != '\t' && value != '\n' && value != '\r')
                sanitized.push_back('?');
            else
                sanitized.push_back(text[offset]);
            ++offset;
            continue;
        }
        sanitized.append(text.substr(offset, length));
        offset += length;
    }
    return sanitized;
}

std::string escapeXml(std::string_view text) {
    const auto sanitized = sanitizeXmlText(text);
    std::string escaped;
    escaped.reserve(sanitized.size() + 16);
    for (const char character : sanitized) {
        switch (character) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

// Length of an escaped prefix that does not split an XML entity or a UTF-8
// code point. Escaped text only contains ASCII entities and UTF-8 data.
std::size_t xmlSafePrefixLength(std::string_view escaped, std::size_t limit) {
    auto cut = std::min(limit, escaped.size());
    while (cut > 0 && cut < escaped.size() &&
           isUtf8Continuation(static_cast<unsigned char>(escaped[cut]))) {
        --cut;
    }
    if (cut == 0) return 0;
    const auto ampersand = escaped.rfind('&', cut - 1);
    if (ampersand != std::string_view::npos) {
        const auto semicolon = escaped.find(';', ampersand + 1);
        if (semicolon == std::string_view::npos || semicolon >= cut) cut = ampersand;
    }
    while (cut > 0 && cut < escaped.size() &&
           isUtf8Continuation(static_cast<unsigned char>(escaped[cut]))) {
        --cut;
    }
    return cut;
}

// Start of an escaped suffix that does not split an XML entity or a UTF-8 code
// point.
std::size_t xmlSafeSuffixStart(std::string_view escaped, std::size_t start) {
    auto safe = std::min(start, escaped.size());
    while (safe < escaped.size() &&
           isUtf8Continuation(static_cast<unsigned char>(escaped[safe]))) {
        ++safe;
    }
    if (safe == 0) return 0;
    const auto ampersand = escaped.rfind('&', safe - 1);
    if (ampersand != std::string_view::npos) {
        const auto semicolon = escaped.find(';', ampersand + 1);
        if (semicolon == std::string_view::npos) return escaped.size();
        if (semicolon >= safe) safe = semicolon + 1;
    }
    while (safe < escaped.size() &&
           isUtf8Continuation(static_cast<unsigned char>(escaped[safe]))) {
        ++safe;
    }
    return std::min(safe, escaped.size());
}

// Compacts escaped text to `budget` bytes while preserving its beginning and
// end. Keeping both ends makes long identifiers, paths and signatures useful
// evidence instead of dropping the whole intent. No entity or code point is
// split, and the result never exceeds the budget. Truncated output always
// carries the explicit "..." marker: a budget too small for the marker plus at
// least one content byte yields an empty string, and callers must omit the
// field or record instead of emitting an unmarked prefix that could pass for a
// complete identifier.
std::string truncateEscapedMiddle(const std::string& escaped, std::size_t budget,
                                  std::size_t headWeight = 2, std::size_t tailWeight = 1) {
    if (escaped.size() <= budget) return escaped;
    constexpr std::string_view kMarker = "...";
    if (budget < kMarker.size() + 1) return {};

    const auto content = budget - kMarker.size();
    const auto totalWeight = headWeight + tailWeight;
    auto headTarget = content * headWeight / totalWeight;
    if (headTarget == 0) headTarget = 1;
    const auto tailTarget = content - headTarget;

    const auto prefix = escaped.substr(0, xmlSafePrefixLength(escaped, headTarget));
    const auto suffixStart = xmlSafeSuffixStart(escaped, escaped.size() - tailTarget);
    const auto suffix = escaped.substr(suffixStart);
    // Padding around a real target is not identifying content. At a tight
    // budget both retained ends may consist only of whitespace, which must not
    // turn a marker-only identity into apparently useful Verified evidence.
    if (trimText(prefix).empty() && trimText(suffix).empty()) return {};
    return prefix + std::string(kMarker) + suffix;
}

// Returns `escaped` unchanged when it fits, otherwise a middle-compacted form
// with an explicit marker. Empty means the value cannot be represented within
// the budget as either the complete text or a useful marked value; callers must
// omit it rather than emit an empty attribute, a marker alone, or an unmarked
// prefix that could be mistaken for a complete identifier.
std::string compactEscapedText(const std::string& escaped, std::size_t budget) {
    if (escaped.empty()) return {};
    if (escaped.size() <= budget) return escaped;
    constexpr std::string_view kMarker = "...";
    if (budget < kMarker.size() + 1) return {};
    auto compact = truncateEscapedMiddle(escaped, budget);
    if (compact.size() <= kMarker.size()) return {};
    return compact;
}

// Deterministically shares a byte budget across fields. Once a field is fully
// included its remaining weight is redistributed to the other fields.
struct BudgetField {
    const std::string* text{nullptr};
    std::size_t weight{1};
    std::size_t allocation{0};
};

void allocateFieldBudget(std::vector<BudgetField>& fields, std::size_t budget) {
    for (auto& field : fields) field.allocation = 0;
    auto remaining = budget;
    while (remaining > 0) {
        std::size_t weightSum = 0;
        for (const auto& field : fields) {
            if (field.allocation < field.text->size()) weightSum += field.weight;
        }
        if (weightSum == 0) break;

        std::size_t distributed = 0;
        for (auto& field : fields) {
            if (field.allocation >= field.text->size()) continue;
            const auto share = remaining * field.weight / weightSum;
            if (share == 0) continue;
            const auto addition = std::min(share, field.text->size() - field.allocation);
            field.allocation += addition;
            remaining -= addition;
            distributed += addition;
        }
        if (distributed != 0) continue;

        BudgetField* chosen = nullptr;
        for (auto& field : fields) {
            if (field.allocation >= field.text->size()) continue;
            if (chosen == nullptr || field.weight > chosen->weight) chosen = &field;
        }
        if (chosen == nullptr) break;
        ++chosen->allocation;
        --remaining;
    }
}

const char* groundingName(GroundingStatus status) noexcept {
    switch (status) {
        case GroundingStatus::Ungrounded: return "UNGROUNDED";
        case GroundingStatus::Verified: return "VERIFIED";
        case GroundingStatus::Ambiguous: return "AMBIGUOUS";
        case GroundingStatus::NotFoundInScope: return "NOT_FOUND_IN_SCOPE";
        case GroundingStatus::TypeMismatch: return "TYPE_MISMATCH";
    }
    return "UNGROUNDED";
}

bool requiresExistingTarget(IntentAction action) {
    return action == IntentAction::MODIFY || action == IntentAction::REMOVE || action == IntentAction::PRESERVE ||
           action == IntentAction::CONSTRAIN;
}

bool isFunctionKind(storage::AstNodeKind kind) {
    return kind == storage::AstNodeKind::Function || kind == storage::AstNodeKind::Method;
}

bool isScopeKind(storage::AstNodeKind kind) {
    return kind == storage::AstNodeKind::Namespace || kind == storage::AstNodeKind::Class ||
           kind == storage::AstNodeKind::Struct || kind == storage::AstNodeKind::Enum ||
           kind == storage::AstNodeKind::Function || kind == storage::AstNodeKind::Method;
}

// Parsed target syntax. Explicit signature templates or nested parentheses are
// unsupported and fail closed rather than being treated as exact names.
struct TargetForm {
    std::string normalized{};
    std::string base{};
    std::string name{};
    std::string signature{};
    bool hasSignature{false};
    bool unsupportedTemplate{false};
    bool unsupportedSignature{false};
};

TargetForm parseTarget(std::string_view raw) {
    TargetForm target;
    target.normalized = normalizeIdentifier(raw);
    if (target.normalized.empty()) return target;

    target.base = target.normalized;
    if (target.normalized.back() == ')') {
        const auto open = target.normalized.rfind('(');
        if (open != std::string::npos && open > 0) {
            const auto base = trimText(std::string_view(target.normalized).substr(0, open));
            const auto inside = std::string_view(target.normalized).substr(open + 1, target.normalized.size() - open - 2);
            if (!base.empty() && inside.find('(') == std::string_view::npos &&
                inside.find(')') == std::string_view::npos) {
                target.base = base;
                target.signature = std::string(inside);
                target.hasSignature = true;
            } else if (!base.empty()) {
                target.base = base;
                target.unsupportedSignature = true;
            }
        }
    }

    // Any remaining parenthesis syntax (unclosed lists, trailing qualifiers,
    // function-pointer types) is outside the documented signature form.
    if (!target.hasSignature && !target.unsupportedSignature &&
        target.normalized.find('(') != std::string::npos) {
        target.unsupportedSignature = true;
    }
    if (target.base.find('<') != std::string::npos || target.base.find('>') != std::string::npos)
        target.unsupportedTemplate = true;
    target.name = targetName(target.base);
    return target;
}

struct ConditionConstraints {
    std::string scope{};
    std::string file{};
    bool hasFile{false};
    bool unsupportedTemplate{false};
};

ConditionConstraints parseCondition(std::string_view condition) {
    ConditionConstraints constraints;
    std::string remaining(condition);

    std::smatch match;
    if (std::regex_search(remaining, match, fileRe)) {
        constraints.file = normalizeFilePath(match[1].str());
        constraints.hasFile = !constraints.file.empty();
        // Remove the file clause before looking for an enclosing scope so a
        // directory named "in" cannot be mistaken for a scope marker.
        auto begin = static_cast<std::size_t>(match.position(0));
        auto count = static_cast<std::size_t>(match.length(0));
        // Consume the file clause's own preposition as well. Leaving "in"
        // behind would join it to trailing prose ("in for readability") and
        // invent an explicit scope named "for".
        const auto prefix = remaining.substr(0, begin);
        std::smatch preposition;
        if (std::regex_search(prefix, preposition, filePrepositionRe)) {
            const auto prepStart = static_cast<std::size_t>(preposition.position());
            count += begin - prepStart;
            begin = prepStart;
        }
        remaining.erase(begin, count);
    }
    if (std::regex_search(remaining, match, scopeRe)) {
        constraints.scope = normalizeIdentifier(match[1].str());
        constraints.unsupportedTemplate =
            constraints.scope.find('<') != std::string::npos ||
            constraints.scope.find('>') != std::string::npos;
    }
    return constraints;
}

struct ParameterConstraint {
    bool active{false};
    bool byIndex{false};
    std::uint16_t index{0};
    std::string name{};
};

bool isIdentifierText(std::string_view text) {
    if (text.empty()) return false;
    const auto first = static_cast<unsigned char>(text.front());
    if (!(std::isalpha(first) || text.front() == '_')) return false;
    for (std::size_t index = 1; index < text.size(); ++index) {
        const auto character = static_cast<unsigned char>(text[index]);
        if (!(std::isalnum(character) || text[index] == '_')) return false;
    }
    return true;
}

bool isGenericParameterState(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char character : value)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return lowered == "present" || lowered == "absent" || lowered == "changed" ||
           lowered == "unchanged" || lowered == "required";
}

ParameterConstraint parseParameterConstraint(const CanonicalIntent& intent) {
    ParameterConstraint constraint;
    if (intent.property != "parameter") return constraint;

    const auto value = normalizeIdentifier(intent.value);
    if (value.empty()) return constraint;
    if (value.front() == '#') {
        if (value.size() < 2) return constraint;
        std::uint32_t index = 0;
        for (std::size_t position = 1; position < value.size(); ++position) {
            const auto character = static_cast<unsigned char>(value[position]);
            if (!std::isdigit(character)) return constraint;
            index = index * 10U + static_cast<std::uint32_t>(value[position] - '0');
            if (index > 65535U) return constraint;
        }
        if (index == 0) return constraint;
        constraint.active = true;
        constraint.byIndex = true;
        constraint.index = static_cast<std::uint16_t>(index);
        return constraint;
    }
    // Canonical extraction uses these words as generic action states. They are not
    // treated as an explicit parameter-name constraint, preserving the existing
    // property/kind mismatch behavior for those intents.
    if (isGenericParameterState(value)) return constraint;
    if (!isIdentifierText(value)) return constraint;
    constraint.active = true;
    constraint.name = value;
    return constraint;
}

struct QueryOutcome {
    std::vector<storage::AstSymbol> symbols{};
    bool truncated{false};
    // False when the index reports that the current snapshot coverage is
    // incomplete. Such a result cannot prove uniqueness or absence.
    bool complete{true};
    bool failed{false};
};

QueryOutcome takeQuery(Result<storage::SymbolQueryResult> result) {
    QueryOutcome outcome;
    if (!result.ok()) {
        outcome.failed = true;
        return outcome;
    }
    outcome.truncated = result.value().truncated;
    outcome.complete = result.value().complete;
    outcome.symbols = std::move(result.value().symbols);
    return outcome;
}

struct QueryTrace {
    bool truncated{false};
    bool failed{false};
    // Any query that reported incomplete coverage makes the whole candidate
    // universe non-authoritative.
    bool coverageIncomplete{false};

    void absorb(const QueryOutcome& outcome) noexcept {
        truncated = truncated || outcome.truncated;
        failed = failed || outcome.failed;
        coverageIncomplete = coverageIncomplete || !outcome.complete;
    }
    [[nodiscard]] bool incomplete() const noexcept {
        return truncated || failed || coverageIncomplete;
    }
};

void deduplicate(std::vector<storage::AstSymbol>& symbols) {
    std::sort(symbols.begin(), symbols.end(), [](const storage::AstSymbol& left,
                                                 const storage::AstSymbol& right) {
        return left.symbolId < right.symbolId;
    });
    symbols.erase(std::unique(symbols.begin(), symbols.end(),
                              [](const storage::AstSymbol& left, const storage::AstSymbol& right) {
                                  return left.symbolId == right.symbolId;
                              }),
                  symbols.end());
}

std::string compactSyntax(std::string_view text) {
    std::string compact;
    compact.reserve(text.size());
    for (const char character : text) {
        if (!std::isspace(static_cast<unsigned char>(character))) compact.push_back(character);
    }
    return compact;
}

bool isQualifierSuffix(std::string_view remainder) {
    for (const auto* qualifier : {"const", "volatile", "noexcept", "override", "final", "&", "&&",
                                  "throw", "requires"}) {
        if (remainder.starts_with(qualifier)) return true;
    }
    return false;
}

// Matches the requested declared type syntax against the indexed syntactic
// signatureKey. Parameter names/defaults are intentionally not accepted, and a
// key with a qualifier suffix still matches a signature that omits qualifiers.
bool signatureMatches(const storage::AstSymbol& symbol, std::string_view signature) {
    if (symbol.signatureKey.empty()) return false;
    const auto key = compactSyntax(symbol.signatureKey);
    const auto expected = compactSyntax(symbol.qualifiedName + "(" + std::string(signature) + ")");
    if (key == expected) return true;
    if (key.size() <= expected.size() || !key.starts_with(expected)) return false;
    return isQualifierSuffix(std::string_view(key).substr(expected.size()));
}

bool parameterMatches(const storage::AstSymbol& symbol, const ParameterConstraint& constraint) {
    if (symbol.kind != storage::AstNodeKind::Parameter) return false;
    if (constraint.byIndex) return symbol.parameterIndex == constraint.index;
    return symbol.name == constraint.name;
}

bool withinAnyScope(const storage::AstSymbol& symbol,
                    const std::vector<storage::AstSymbol>& scopes) {
    for (const auto& scope : scopes) {
        if (symbol.qualifiedName.starts_with(scope.qualifiedName + "::")) return true;
    }
    return false;
}

// A qualified target is absolute by default, but an explicitly named scope also
// supports the existing relative form `Scope::member`, which resolves to
// `resolved_scope::member`. The relative form is accepted only when the target
// begins with that scope's simple name; arbitrary suffixes never qualify.
bool qualifiedTargetAllowed(const storage::AstSymbol& symbol, const TargetForm& target,
                            bool explicitScope,
                            const std::vector<storage::AstSymbol>& scopes) {
    if (symbol.qualifiedName == target.base) return true;
    if (!explicitScope) return false;
    for (const auto& scope : scopes) {
        if (target.base == scope.name + "::" + target.name &&
            symbol.qualifiedName == scope.qualifiedName + "::" + target.name) {
            return true;
        }
    }
    return false;
}

int rank(const storage::AstSymbol& symbol, const CanonicalIntent& intent, std::string_view activeFile,
         std::string_view normalizedTarget, const std::vector<storage::AstSymbol>& scopes) {
    int score = 0;
    if (!activeFile.empty() && symbol.filePath == activeFile) score += 1000;
    if (!activeFile.empty() && directoryOf(symbol.filePath) == directoryOf(activeFile)) score += 100;
    if (symbol.qualifiedName == normalizedTarget) score += 200;
    if (!scopes.empty() && withinAnyScope(symbol, scopes)) score += 80;
    if (intent.property == "parameter" && symbol.kind == storage::AstNodeKind::Parameter) score += 50;
    if (intent.property == "field" && symbol.kind == storage::AstNodeKind::Field) score += 50;
    if (intent.property == "name" && (symbol.kind == storage::AstNodeKind::Function ||
                                      symbol.kind == storage::AstNodeKind::Method ||
                                      symbol.kind == storage::AstNodeKind::Parameter ||
                                      symbol.kind == storage::AstNodeKind::Field)) score += 20;
    return score;
}

AstGroundingEvidence evidenceFor(const storage::AstSymbol& symbol, GroundingStatus status) {
    AstGroundingEvidence evidence;
    evidence.status = status;
    evidence.qualifiedTarget = symbol.qualifiedName;
    evidence.targetKind = symbol.kind;
    evidence.filePath = symbol.filePath;
    evidence.fileHash = symbol.fileHash;
    evidence.fileSpan = symbol.span;
    evidence.line = symbol.startLine;
    evidence.signature = symbol.signature;
    return evidence;
}

std::string candidateLabel(const storage::AstSymbol& symbol) {
    return symbol.qualifiedName + " @ " + symbol.filePath + ":" + std::to_string(symbol.startLine) + ":" +
           std::to_string(symbol.startColumn);
}

std::vector<std::string> candidateLabels(const std::vector<storage::AstSymbol>& symbols,
                                         std::size_t limit = AstGroundingEngine::kMaximumCandidates) {
    std::vector<std::string> labels;
    labels.reserve(std::min(limit, symbols.size()));
    for (const auto& symbol : symbols) {
        if (labels.size() >= limit) break;
        labels.push_back(candidateLabel(symbol));
    }
    std::sort(labels.begin(), labels.end());
    labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
    return labels;
}

std::string explicitReason(const TargetForm& target, const ConditionConstraints& condition,
                           const ParameterConstraint& parameter) {
    if (condition.hasFile) return "file";
    if (target.hasSignature) return "signature";
    if (parameter.active) return "parameter";
    if (!condition.scope.empty()) return "scope";
    if (target.base.find("::") != std::string::npos) return "qualified";
    return {};
}

std::string ambiguousReason(const TargetForm& target, const ConditionConstraints& condition,
                            const ParameterConstraint& parameter, std::string_view activeFile,
                            const std::vector<storage::AstSymbol>& candidates) {
    auto reason = explicitReason(target, condition, parameter);
    if (!reason.empty()) return reason;
    if (!activeFile.empty()) {
        for (const auto& candidate : candidates) {
            if (candidate.filePath == activeFile) return "active-file";
        }
    }
    return {};
}

std::string selectedReason(const TargetForm& target, const ConditionConstraints& condition,
                           const ParameterConstraint& parameter, std::string_view activeFile,
                           const storage::AstSymbol& selected, bool hadMultiple) {
    auto reason = explicitReason(target, condition, parameter);
    if (!reason.empty()) return reason;
    if (!activeFile.empty() && selected.filePath == activeFile && hadMultiple) return "active-file";
    return "unique";
}

std::string requestedLabel(const ConditionConstraints& condition, const TargetForm& target) {
    if (!condition.scope.empty()) return condition.scope + "::" + target.name;
    return target.base;
}

// Verified evidence is rendered first so a tight document always contains the
// strongest available grounding. Equal-status intents keep their input order.
int renderPriority(const CanonicalIntent& intent) {
    if (!intent.grounding.has_value()) return 5;
    switch (intent.grounding->status) {
        case GroundingStatus::Verified: return 0;
        case GroundingStatus::TypeMismatch: return 1;
        case GroundingStatus::NotFoundInScope: return 2;
        case GroundingStatus::Ambiguous: return 3;
        case GroundingStatus::Ungrounded: return 4;
    }
    return 5;
}

std::string renderVerifiedBlock(const CanonicalIntent& intent,
                                const AstGroundingEvidence& evidence,
                                std::size_t blockBudget,
                                std::size_t signatureBudgetRemaining,
                                std::size_t& signatureUsed) {
    const std::string action = toString(intent.action);
    const std::string identity = escapeXml(intent.target);
    const std::string symbol = escapeXml(evidence.qualifiedTarget);
    // The target attribute is the required identity; the grounded symbol is
    // optional metadata, never a substitute for a missing requested target. A
    // Verified record must never be emitted with `target=""` or a
    // whitespace-only target, so such a record is omitted entirely.
    if (trimText(intent.target).empty()) return {};

    const std::string file = escapeXml(evidence.filePath);
    const std::string signature = escapeXml(evidence.signature);
    const std::string line = std::to_string(evidence.line);

    bool symbolField = !trimText(evidence.qualifiedTarget).empty() && symbol != identity;
    bool signatureField = !signature.empty() && signatureBudgetRemaining > 0;
    bool locationField = !file.empty() || evidence.line > 0;

    const std::string head = "  <intent action=\"" + action + "\" target=\"";
    std::string mid = "\" status=\"" + std::string(groundingName(evidence.status)) + "\"";
    if (!evidence.complete) mid += " complete=\"false\"";
    mid += ">";
    const std::string tail = "</intent>\n";
    const std::string symbolTagBytes = "<grounded_symbol></grounded_symbol>";
    const std::string locationHead = "<location file=\"\" line=\"";
    const std::string locationTail = "\"/>";
    const std::string signatureTagBytes = "<signature></signature>";

    const auto fixedCost = [&]() {
        auto fixed = head.size() + mid.size() + tail.size();
        if (symbolField) fixed += symbolTagBytes.size();
        if (locationField) fixed += locationHead.size() + line.size() + locationTail.size();
        if (signatureField) fixed += signatureTagBytes.size();
        return fixed;
    };
    const auto dropOptional = [&]() {
        // Optional evidence is sacrificed in this fixed order before the
        // required target identity is omitted.
        if (signatureField) {
            signatureField = false;
            return true;
        }
        if (symbolField) {
            symbolField = false;
            return true;
        }
        if (locationField) {
            locationField = false;
            return true;
        }
        return false;
    };

    const auto minimalFixed = head.size() + mid.size() + tail.size();
    if (minimalFixed > blockBudget) return {};
    const auto minimalContent = blockBudget - minimalFixed;
    // The target may be complete or explicitly marked, but must be useful even
    // when every optional field has been dropped. Otherwise the record cannot
    // carry an identity and is omitted.
    if (compactEscapedText(identity, minimalContent).empty()) return {};

    while (true) {
        const auto fixed = fixedCost();
        if (fixed > blockBudget) {
            if (!dropOptional()) return {};
            continue;
        }

        std::vector<BudgetField> fields;
        fields.push_back({&identity, 4});
        if (symbolField) fields.push_back({&symbol, 4});
        if (locationField) fields.push_back({&file, 4});
        if (signatureField) fields.push_back({&signature, 3});
        allocateFieldBudget(fields, blockBudget - fixed);

        const auto identityAllocation = fields[0].allocation;
        std::size_t fieldIndex = 1;
        auto symbolAllocation = std::size_t{0};
        if (symbolField) symbolAllocation = fields[fieldIndex++].allocation;
        auto fileAllocation = std::size_t{0};
        if (locationField) fileAllocation = fields[fieldIndex++].allocation;
        auto signatureAllocation = std::size_t{0};
        if (signatureField) {
            signatureAllocation = std::min(fields[fieldIndex++].allocation,
                                           signatureBudgetRemaining);
        }

        const auto renderedIdentity = compactEscapedText(identity, identityAllocation);
        // Optional fields are dropped before the required target identity is
        // omitted. A marked target is already useful evidence, so optionals may
        // share the remaining budget with it.
        if (renderedIdentity.empty()) {
            if (!dropOptional()) return {};
            continue;
        }
        if (symbolField && compactEscapedText(symbol, symbolAllocation).empty()) {
            symbolField = false;
            continue;
        }
        if (locationField && !file.empty() &&
            compactEscapedText(file, fileAllocation).empty()) {
            locationField = false;
            continue;
        }
        if (signatureField &&
            compactEscapedText(signature, signatureAllocation).empty()) {
            signatureField = false;
            continue;
        }

        std::string block = head;
        block += renderedIdentity;
        block += mid;
        if (symbolField) {
            block += "<grounded_symbol>";
            block += compactEscapedText(symbol, symbolAllocation);
            block += "</grounded_symbol>";
        }
        if (locationField) {
            block += "<location file=\"";
            if (!file.empty()) block += compactEscapedText(file, fileAllocation);
            block += "\" line=\"";
            block += line;
            block += "\"/>";
        }
        if (signatureField) {
            const auto renderedSignature =
                compactEscapedText(signature, signatureAllocation);
            block += "<signature>";
            block += renderedSignature;
            block += "</signature>";
            signatureUsed += renderedSignature.size();
        }
        block += tail;
        return block;
    }
}

std::string renderCandidateBlock(const CanonicalIntent& intent,
                                 const AstGroundingEvidence* evidence,
                                 std::size_t blockBudget,
                                 std::size_t& candidateUsed) {
    const std::string action = toString(intent.action);
    const std::string target = escapeXml(intent.target);

    if (evidence == nullptr) {
        const std::string head = "  <intent action=\"" + action + "\" target=\"";
        const std::string tail = "\" status=\"UNGROUNDED\" />\n";
        if (head.size() + tail.size() > blockBudget) return {};
        const auto renderedTarget =
            compactEscapedText(target, blockBudget - head.size() - tail.size());
        if (!target.empty() && renderedTarget.empty()) return {};
        return head + renderedTarget + tail;
    }

    std::string mid = "\" status=\"" + std::string(groundingName(evidence->status)) + "\"";
    if (!evidence->complete) mid += " complete=\"false\"";
    mid += ">";
    const std::string tail = "</intent>\n";
    const std::string head = "  <intent action=\"" + action + "\" target=\"";
    const auto fixed = head.size() + mid.size() + tail.size();
    if (fixed > blockBudget) return {};

    const auto contentBudget = blockBudget - fixed;
    auto targetAllocation = std::min(target.size(), contentBudget / 3);
    if (targetAllocation == 0 && !target.empty() && contentBudget > 0) targetAllocation = 1;

    std::string block = head;
    const auto renderedTarget = compactEscapedText(target, targetAllocation);
    // A non-empty target that cannot be marked within its allocation is omitted
    // rather than emitted as an empty or unmarked partial identity.
    if (!target.empty() && renderedTarget.empty()) return {};
    block += renderedTarget;
    block += mid;

    auto remaining = contentBudget - renderedTarget.size();
    for (std::size_t index = 0; index < evidence->candidates.size(); ++index) {
        if (candidateUsed >= kMaximumCandidateBudgetBytes) break;
        const auto escaped = escapeXml(evidence->candidates[index]);
        if (escaped.empty()) continue;
        constexpr std::size_t kCandidateTagBytes = 23; // <candidate></candidate>
        if (remaining <= kCandidateTagBytes) break;
        const auto remainingCandidates = evidence->candidates.size() - index;
        auto allowance = std::min(escaped.size(), kMaximumCandidateLabelBytes);
        allowance = std::min(allowance, std::max<std::size_t>(1, remaining / remainingCandidates));
        allowance = std::min(allowance, remaining - kCandidateTagBytes);
        if (allowance == 0) break;
        const auto rendered = compactEscapedText(escaped, allowance);
        if (rendered.empty()) break;
        std::string candidate = "<candidate>";
        candidate += rendered;
        candidate += "</candidate>";
        if (candidate.size() > remaining) break;
        block += candidate;
        remaining -= candidate.size();
        candidateUsed += rendered.size();
    }
    block += tail;
    return block;
}

} // namespace

std::vector<CanonicalIntent> AstGroundingEngine::groundIntents(const std::vector<CanonicalIntent>& raw,
                                                               std::string_view activeFile,
                                                               const storage::CodebaseIndex& index) {
    auto grounded = raw;

    const auto beforeInfo = index.snapshotInfo();
    // Snapshot completeness gates every decision in this batch. An unavailable
    // snapshot is treated the same way as an explicitly incomplete one: there
    // is no candidate universe that could prove uniqueness or absence.
    const bool snapshotIncomplete =
        !beforeInfo.ok() || !beforeInfo.value().available || !beforeInfo.value().complete;

    for (std::size_t intentIndex = 0; intentIndex < grounded.size(); ++intentIndex) {
        auto& intent = grounded[intentIndex];
        if (intent.grounding.has_value()) continue;

        const auto target = parseTarget(intent.target);
        if (target.name.empty()) continue;

        const auto condition = parseCondition(intent.condition);
        const auto parameter = parseParameterConstraint(intent);

        if (target.unsupportedTemplate || condition.unsupportedTemplate || target.unsupportedSignature) {
            AstGroundingEvidence evidence;
            evidence.status = GroundingStatus::Ambiguous;
            evidence.complete = false;
            evidence.qualifiedTarget = target.base.empty() ? target.normalized : target.base;
            intent.grounding = std::move(evidence);
            continue;
        }

        QueryTrace trace;
        std::vector<storage::AstSymbol> scopeCandidates;
        const bool explicitScope = !condition.scope.empty();
        if (explicitScope) {
            auto exact = takeQuery(index.findQualified(condition.scope, kScopeQueryLimit));
            trace.absorb(exact);
            scopeCandidates = std::move(exact.symbols);
            if (scopeCandidates.empty() && !exact.failed) {
                auto fallback =
                    takeQuery(index.findByName(condition.scope, {}, {}, kScopeQueryLimit, false));
                trace.absorb(fallback);
                scopeCandidates = std::move(fallback.symbols);
            }
            scopeCandidates.erase(
                std::remove_if(scopeCandidates.begin(), scopeCandidates.end(), [](const auto& candidate) {
                    return !isScopeKind(candidate.kind);
                }),
                scopeCandidates.end());
            deduplicate(scopeCandidates);
        }
        const bool scopeMissing = explicitScope && scopeCandidates.empty();

        std::vector<storage::AstSymbol> candidates;
        if (!scopeMissing) {
            const bool qualifiedBase = target.base.find("::") != std::string::npos;
            const std::string_view fileFilter = condition.hasFile ? std::string_view(condition.file)
                                                                  : std::string_view{};
            if (explicitScope && !qualifiedBase) {
                // Query the requested child directly under each resolved scope. A
                // fixed-window child enumeration could miss late members and must
                // never be treated as proof that the target is absent.
                for (const auto& scopeSymbol : scopeCandidates) {
                    auto outcome = takeQuery(index.findByName(target.name, scopeSymbol.symbolId, fileFilter,
                                                              kQueryLimit, false));
                    trace.absorb(outcome);
                    candidates.insert(candidates.end(),
                                      std::make_move_iterator(outcome.symbols.begin()),
                                      std::make_move_iterator(outcome.symbols.end()));
                }
            } else if (qualifiedBase) {
                QueryOutcome absolute;
                if (condition.hasFile) {
                    absolute = takeQuery(index.findByName(target.name, {},
                                                           std::string_view(condition.file), kQueryLimit, false));
                } else {
                    absolute = takeQuery(index.findQualified(target.base, kQueryLimit));
                }
                trace.absorb(absolute);
                candidates = std::move(absolute.symbols);

                if (explicitScope) {
                    // Preserve the documented relative interpretation
                    // `Engine::run` under `in Engine` without ever widening the
                    // search beyond the resolved scope candidates.
                    for (const auto& scopeSymbol : scopeCandidates) {
                        auto relative = takeQuery(index.findByName(target.name, scopeSymbol.symbolId,
                                                                   fileFilter, kQueryLimit, false));
                        trace.absorb(relative);
                        candidates.insert(candidates.end(),
                                          std::make_move_iterator(relative.symbols.begin()),
                                          std::make_move_iterator(relative.symbols.end()));
                    }
                }
            } else {
                auto outcome = takeQuery(index.findByName(target.name, {}, fileFilter, kQueryLimit, false));
                trace.absorb(outcome);
                candidates = std::move(outcome.symbols);
            }
        }

        // Explicit constraints are filters applied before ranking. Focus never
        // widens or overrides them.
        if (condition.hasFile) {
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                            [&](const auto& candidate) {
                                                return candidate.filePath != condition.file;
                                            }),
                             candidates.end());
        }
        if (target.base.find("::") != std::string::npos) {
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                            [&](const auto& candidate) {
                                                return !qualifiedTargetAllowed(candidate, target,
                                                                               explicitScope,
                                                                               scopeCandidates);
                                            }),
                             candidates.end());
        }
        if (explicitScope && !scopeCandidates.empty()) {
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                            [&](const auto& candidate) {
                                                return !withinAnyScope(candidate, scopeCandidates);
                                            }),
                             candidates.end());
        }
        deduplicate(candidates);

        if (target.hasSignature) {
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                            [&](const auto& candidate) {
                                                return !signatureMatches(candidate, target.signature);
                                            }),
                             candidates.end());
        }

        if (parameter.active) {
            std::vector<storage::AstSymbol> parameters;
            for (const auto& candidate : candidates) {
                if (candidate.kind == storage::AstNodeKind::Parameter) {
                    if (parameterMatches(candidate, parameter)) parameters.push_back(candidate);
                    continue;
                }
                if (!isFunctionKind(candidate.kind)) continue;

                if (parameter.byIndex) {
                    auto outcome = takeQuery(index.children(candidate.symbolId, kParameterChildLimit));
                    trace.absorb(outcome);
                    for (auto& child : outcome.symbols) {
                        if (child.kind == storage::AstNodeKind::Parameter &&
                            child.parameterIndex == parameter.index)
                            parameters.push_back(std::move(child));
                    }
                } else {
                    auto outcome = takeQuery(index.findByName(parameter.name, candidate.symbolId,
                                                              condition.hasFile ? std::string_view(condition.file)
                                                                                : std::string_view{},
                                                              kQueryLimit, false));
                    trace.absorb(outcome);
                    for (auto& child : outcome.symbols) {
                        if (child.kind == storage::AstNodeKind::Parameter &&
                            child.name == parameter.name)
                            parameters.push_back(std::move(child));
                    }
                }
            }
            deduplicate(parameters);
            candidates = std::move(parameters);
        }

        const auto reason = explicitReason(target, condition, parameter);
        const auto requested = requestedLabel(condition, target);
        const bool explicitConstraint = condition.hasFile || target.hasSignature || parameter.active;

        if (candidates.empty()) {
            AstGroundingEvidence evidence;
            const bool incompleteUniverse = snapshotIncomplete || trace.coverageIncomplete;
            const bool authoritative = !incompleteUniverse && !trace.truncated && !trace.failed;
            evidence.complete = authoritative;
            evidence.qualifiedTarget = requested;
            evidence.matchReason = reason;

            if (!authoritative) {
                // An incomplete snapshot or query coverage cannot prove
                // uniqueness or absence. ADD-style non-existence claims stay
                // Ungrounded (never a false absence); every other request is
                // bounded ambiguity with whatever candidates were visible.
                const bool addOnly = intent.action == IntentAction::ADD && !explicitScope &&
                                     !explicitConstraint &&
                                     target.base.find("::") == std::string::npos;
                if (addOnly && incompleteUniverse) {
                    evidence.status = GroundingStatus::Ungrounded;
                    evidence.matchReason.clear();
                } else {
                    evidence.status = GroundingStatus::Ambiguous;
                    if (!scopeCandidates.empty()) evidence.candidates = candidateLabels(scopeCandidates);
                }
            } else if (explicitScope) {
                if (scopeMissing || scopeCandidates.size() == 1) {
                    evidence.status = GroundingStatus::NotFoundInScope;
                } else {
                    // The scope itself could not be narrowed to one symbol, and no
                    // child resolved it. Never widen this to a global lookup.
                    evidence.status = GroundingStatus::Ambiguous;
                    evidence.candidates = candidateLabels(scopeCandidates);
                }
            } else if (explicitConstraint || target.base.find("::") != std::string::npos ||
                       requiresExistingTarget(intent.action)) {
                evidence.status = GroundingStatus::NotFoundInScope;
            } else {
                evidence.status = GroundingStatus::Ungrounded;
                evidence.matchReason.clear();
            }
            intent.grounding = std::move(evidence);
            continue;
        }

        const bool hadMultiple = candidates.size() > 1;
        std::stable_sort(candidates.begin(), candidates.end(), [&](const auto& left, const auto& right) {
            return rank(left, intent, activeFile, target.base, scopeCandidates) >
                   rank(right, intent, activeFile, target.base, scopeCandidates);
        });
        const auto bestScore =
            rank(candidates.front(), intent, activeFile, target.base, scopeCandidates);
        std::vector<const storage::AstSymbol*> tied;
        for (const auto& candidate : candidates) {
            if (rank(candidate, intent, activeFile, target.base, scopeCandidates) == bestScore)
                tied.push_back(&candidate);
        }

        if (snapshotIncomplete || trace.incomplete()) {
            auto evidence = evidenceFor(candidates.front(), GroundingStatus::Ambiguous);
            evidence.complete = false;
            evidence.qualifiedTarget = requested;
            evidence.candidates = candidateLabels(candidates);
            evidence.matchReason = ambiguousReason(target, condition, parameter, activeFile, candidates);
            intent.grounding = std::move(evidence);
            continue;
        }

        if (tied.size() > 1) {
            auto evidence = evidenceFor(*tied.front(), GroundingStatus::Ambiguous);
            evidence.qualifiedTarget = requested;
            std::vector<std::string> candidatesByName;
            for (const auto* candidate : tied) candidatesByName.push_back(candidateLabel(*candidate));
            std::sort(candidatesByName.begin(), candidatesByName.end());
            candidatesByName.erase(std::unique(candidatesByName.begin(), candidatesByName.end()),
                                   candidatesByName.end());
            evidence.candidates = std::move(candidatesByName);
            if (evidence.candidates.size() > kMaximumCandidates) evidence.candidates.resize(kMaximumCandidates);
            evidence.matchReason = ambiguousReason(target, condition, parameter, activeFile, candidates);
            intent.grounding = std::move(evidence);
            continue;
        }

        const auto& selected = candidates.front();
        const bool typeMismatch =
            (intent.property == "parameter" && selected.kind != storage::AstNodeKind::Parameter) ||
            (intent.property == "field" && selected.kind != storage::AstNodeKind::Field);
        if (typeMismatch) {
            auto evidence = evidenceFor(selected, GroundingStatus::TypeMismatch);
            evidence.matchReason = explicitReason(target, condition, parameter);
            if (evidence.matchReason.empty() && intent.property == "parameter")
                evidence.matchReason = "parameter";
            if (evidence.matchReason.empty())
                evidence.matchReason =
                    selectedReason(target, condition, parameter, activeFile, selected, hadMultiple);
            intent.grounding = std::move(evidence);
            continue;
        }

        auto evidence = evidenceFor(selected, GroundingStatus::Verified);
        evidence.matchReason = selectedReason(target, condition, parameter, activeFile, selected, hadMultiple);
        intent.grounding = std::move(evidence);
    }

    // A batch must be grounded against one complete index snapshot. If the
    // generation changes or either endpoint reports incomplete coverage while
    // enrichment is running, no newly produced evidence is safe to promote or to
    // use as proof of absence.
    const auto afterInfo = index.snapshotInfo();
    bool snapshotChanged = snapshotIncomplete;
    if (!beforeInfo.ok() || !afterInfo.ok()) {
        // Without a readable generation there is no coherent snapshot proof.
        snapshotChanged = true;
    } else {
        const auto& before = beforeInfo.value();
        const auto& after = afterInfo.value();
        if (!before.available || !after.available) {
            // No bound generation means there is no snapshot identity to bind.
            snapshotChanged = true;
        } else if (before.generation != after.generation) {
            snapshotChanged = true;
        } else if (!after.complete) {
            // Coverage can only become incomplete through another refresh, which
            // also changes the generation; this is a defensive check.
            snapshotChanged = true;
        }
    }
    if (snapshotChanged) {
        // This also downgrades evidence that arrived already grounded: an
        // incomplete or changed snapshot cannot make any intent authoritative.
        for (auto& groundedIntent : grounded) {
            if (!groundedIntent.grounding.has_value()) continue;
            auto& evidence = *groundedIntent.grounding;
            evidence.complete = false;
            if (evidence.status == GroundingStatus::Verified ||
                evidence.status == GroundingStatus::NotFoundInScope ||
                evidence.status == GroundingStatus::TypeMismatch) {
                evidence.status = GroundingStatus::Ambiguous;
            }
        }
    }
    return grounded;
}

std::string AstGroundingEngine::computeAstSnapshotHash(const std::vector<CanonicalIntent>& grounded,
                                                       std::string_view workspaceIdentity) {
    std::vector<const AstGroundingEvidence*> verified;
    for (const auto& intent : grounded) {
        if (intent.grounding.has_value() && intent.grounding->status == GroundingStatus::Verified)
            verified.push_back(&*intent.grounding);
    }
    if (verified.empty()) return {};
    std::sort(verified.begin(), verified.end(), [](const AstGroundingEvidence* left,
                                                   const AstGroundingEvidence* right) {
        if (left->filePath != right->filePath) return left->filePath < right->filePath;
        if (left->fileSpan.startByte != right->fileSpan.startByte)
            return left->fileSpan.startByte < right->fileSpan.startByte;
        return left->qualifiedTarget < right->qualifiedTarget;
    });
    // Proposal v2 evidence fields only. complete/hashValidated/matchReason are
    // diagnostic metadata and deliberately excluded from this hash.
    crypto::CanonicalHasher hasher("roclint.ast_context.v1");
    hasher.field(workspaceIdentity).number(verified.size());
    for (const auto* evidence : verified) {
        hasher.field(evidence->qualifiedTarget)
            .field(evidence->filePath)
            .field(evidence->fileHash)
            .number(evidence->fileSpan.startByte)
            .number(evidence->fileSpan.endByte)
            .field(evidence->signature);
    }
    return hasher.finalizeHex();
}

PromptRenderResult AstGroundingEngine::renderPromptContext(
    const std::vector<CanonicalIntent>& intents, std::size_t signatureBudget) {
    PromptRenderResult result;
    const std::string closing = "</canonical_intents>";
    result.xml = "<canonical_intents>\n";
    std::size_t signatureUsed = 0;
    std::size_t candidateUsed = 0;

    // The signature allowance is a sub-limit, never a way around the total C10
    // bound.
    const auto boundedSignatureBudget = std::min(signatureBudget, kMaximumPromptBytes);

    std::vector<const CanonicalIntent*> ordered;
    ordered.reserve(intents.size());
    for (const auto& intent : intents) ordered.push_back(&intent);
    std::stable_sort(ordered.begin(), ordered.end(), [](const CanonicalIntent* left,
                                                        const CanonicalIntent* right) {
        return renderPriority(*left) < renderPriority(*right);
    });

    for (const auto* intent : ordered) {
        const auto blockBudget = kMaximumPromptBytes - result.xml.size() - closing.size();
        if (blockBudget == 0) break;

        std::string block;
        if (intent->grounding.has_value() &&
            intent->grounding->status == GroundingStatus::Verified) {
            const auto remainingSignature =
                boundedSignatureBudget > signatureUsed ? boundedSignatureBudget - signatureUsed : 0;
            block = renderVerifiedBlock(*intent, *intent->grounding, blockBudget, remainingSignature,
                                        signatureUsed);
        } else {
            block = renderCandidateBlock(*intent, intent->grounding ? &*intent->grounding : nullptr,
                                         blockBudget, candidateUsed);
        }

        // A block that cannot fit does not consume budget; a later shorter block
        // may still fit. Only emitted intents become part of the rendered subset.
        if (block.empty()) continue;
        result.xml += block;
        result.rendered.push_back(*intent);
    }

    result.xml += closing;
    return result;
}

std::string AstGroundingEngine::renderPromptXml(const std::vector<CanonicalIntent>& intents,
                                                std::size_t signatureBudget) {
    return renderPromptContext(intents, signatureBudget).xml;
}

} // namespace roclint::core::rewrite
