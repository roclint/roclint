#include "roclint/rewrite/StructuredEntityExtractor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace roclint::core::rewrite {
namespace {

enum class PatternKind {
    Mandatory,
    Quantified,
    AlwaysAction,
    Imperative,
    FilenameExtension,
    Permission,
    ScopeAction
};

struct Sentence {
    std::string text{};
    std::size_t startByte{0};
};

struct Requirement {
    std::string subject{};
    std::string value{};
    std::string scope{};
    std::string clause{};
    std::size_t startByte{0};
    PatternKind kind{PatternKind::Mandatory};
};

const std::regex mandatoryRe(
    R"(\b([A-Za-z][A-Za-z0-9 _-]{0,48}?)\s+(?:must|shall|should|will)\s+(.+))",
    std::regex::icase | std::regex::optimize);
const std::regex quantifiedRe(
    R"(\b(?:use|require|set|apply|keep|limit)\s+exactly\s+([A-Za-z0-9.-]+)\s+([A-Za-z][A-Za-z0-9 _-]{0,32}))",
    std::regex::icase | std::regex::optimize);
const std::regex alwaysActionRe(
    R"(\balways\s+(include|exclude|omit|add|remove|show|hide|enable|disable)\s+([A-Za-z][A-Za-z0-9 _-]{1,48}?)(?:\s+(?:in|from|for|on|at|when|within|to)\s+(.+?))?\s*$)",
    std::regex::icase | std::regex::optimize);
const std::regex imperativeIncludeExcludeRe(
    R"(^\s*(include|exclude|add|remove|show|hide|enable|disable)\s+([A-Za-z][A-Za-z0-9 _-]{1,48}?)(?:\s+(?:in|from|for|on|at|when|within|to)\s+(.+?))?\s*$)",
    std::regex::icase | std::regex::optimize);
const std::regex filenameExtRe(
    R"(\b(?:use|apply|save|write|export|set)\s+(?:the\s+)?(?:file\s+|filename\s+)?(?:extension\s+)?(\.?[A-Za-z0-9]{1,12})(?:\s+(?:for|to)\s+(?:every|all|each|the)\s+([A-Za-z][A-Za-z0-9 _-]{1,48}))?\s*$)",
    std::regex::icase | std::regex::optimize);
const std::regex permissionRe(
    R"(\b(only|every|all|any)\s+([A-Za-z][A-Za-z0-9 _-]{1,40}?)\s+(?:may|can|is allowed to|are allowed to)\s+([A-Za-z][A-Za-z0-9 _-]{1,40}))",
    std::regex::icase | std::regex::optimize);
const std::regex scopeActionRe(
    R"(^\s*(expand|collapse|open|close)\s+(?:the\s+)?(?:([A-Za-z][A-Za-z0-9 _-]{1,40}?)\s+)?(?:on|in|for|when|at)\s+(.+?)\s*$)",
    std::regex::icase | std::regex::optimize);
const std::regex numberTokenRe(R"((?:\d+(?:\.\d+)?))", std::regex::optimize);
const std::regex extensionTokenRe(R"(\.[A-Za-z0-9]{1,12}\b)", std::regex::optimize);
const std::regex filenameTokenRe(
    R"(\b[A-Za-z0-9_-]{1,64}\.(?:txt|md|json|yaml|yml|xml|csv|html|css|js|ts|jpeg|jpg|png|gif|pdf|log|ini|toml|sql)\b)",
    std::regex::icase | std::regex::optimize);
const std::regex negationRe(R"(\b(?:not|never|no|without|disallow|forbid|prohibit|cannot|can't)\b)",
                            std::regex::icase | std::regex::optimize);

const std::set<std::string> kNumberWords{
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
    "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty"};

const std::set<std::string> kActionPositive{
    "include", "add", "show", "display", "enable", "allow", "keep", "retain", "visible", "remain", "appear",
    "accept", "expand", "open", "present"};

const std::set<std::string> kActionNegative{
    "exclude", "omit", "remove", "hide", "disable", "disallow", "delete", "disappear", "hidden",
    "absent", "reject", "collapse", "close", "closed", "never"};

const std::set<std::string> kStopWords{
    "a", "an", "the", "of", "for", "to", "in", "on", "at", "and", "or", "with", "by", "from", "as", "is",
    "are", "be", "this", "that", "these", "those", "every", "all", "each", "any"};

const std::set<std::string> kDays{
    "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};

const std::set<std::string> kShapes{
    "circle", "square", "triangle", "rectangle", "oval", "ellipse", "hexagon", "pentagon", "octagon",
    "diamond", "star", "sphere", "cube"};

const std::set<std::string> kSeverity{"critical", "informational", "warning", "error", "debug", "success", "fatal"};
const std::set<std::string> kDevices{"desktop", "mobile", "tablet", "phone", "watch"};
const std::set<std::string> kPlatforms{"windows", "macos", "linux", "ios", "android"};
const std::set<std::string> kEnvironments{"production", "staging", "development", "test", "testing", "local"};

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

bool isWordCharacter(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_' || character == '-';
}

std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    for (const char character : text) {
        if (isWordCharacter(character)) current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        else if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

std::string singularize(std::string word) {
    if (word.size() > 4 && word.ends_with("ies")) {
        word.replace(word.size() - 3, 3, "y");
    } else if (word.size() > 4 && (word.ends_with("ches") || word.ends_with("shes") || word.ends_with("xes") ||
                                   word.ends_with("zes") || word.ends_with("ses"))) {
        word.pop_back();
        word.pop_back();
    } else if (word.size() > 3 && word.ends_with("s") && !word.ends_with("ss")) {
        word.pop_back();
    }
    return word;
}

std::string normalizedSubject(std::string_view subject) {
    std::string out;
    for (auto word : words(subject)) {
        if (kStopWords.contains(word)) continue;
        word = singularize(std::move(word));
        if (word.empty() || kStopWords.contains(word)) continue;
        if (!out.empty()) out.push_back(' ');
        out += word;
    }
    return out;
}

std::string normalizedValue(std::string_view value) {
    auto out = lower(trim(value));
    while (out.size() >= 2) {
        if (out.starts_with("the ")) out.erase(0, 4);
        else if (out.starts_with("a ")) out.erase(0, 2);
        else if (out.starts_with("an ")) out.erase(0, 3);
        else break;
    }
    std::string collapsed;
    bool pendingSpace = false;
    for (const char character : out) {
        if (std::isspace(static_cast<unsigned char>(character))) {
            pendingSpace = !collapsed.empty();
            continue;
        }
        if (pendingSpace) {
            collapsed.push_back(' ');
            pendingSpace = false;
        }
        collapsed.push_back(character);
    }
    return trim(collapsed);
}

double similarity(std::string_view left, std::string_view right) {
    const auto a = words(left);
    const auto b = words(right);
    if (a.empty() || b.empty()) return 0.0;
    std::set<std::string> uniqueA;
    std::set<std::string> uniqueB;
    for (const auto& word : a) if (!kStopWords.contains(word)) uniqueA.insert(singularize(word));
    for (const auto& word : b) if (!kStopWords.contains(word)) uniqueB.insert(singularize(word));
    if (uniqueA.empty() || uniqueB.empty()) return 0.0;
    std::size_t intersection = 0;
    for (const auto& word : uniqueA) if (uniqueB.contains(word)) ++intersection;
    const auto unionSize = uniqueA.size() + uniqueB.size() - intersection;
    return unionSize == 0 ? 0.0 : static_cast<double>(intersection) / static_cast<double>(unionSize);
}

bool sameSubject(const Requirement& left, const Requirement& right) {
    const auto a = normalizedSubject(left.subject);
    const auto b = normalizedSubject(right.subject);
    if (!a.empty() && a == b) return true;
    const bool actionKind = left.kind == PatternKind::AlwaysAction || left.kind == PatternKind::Imperative ||
                            right.kind == PatternKind::AlwaysAction || right.kind == PatternKind::Imperative;
    return similarity(left.subject, right.subject) >= (actionKind ? 0.4 : 0.5);
}

int actionPolarity(const std::string& value) {
    int polarity = 0;
    for (const auto& word : words(value)) {
        if (kActionPositive.contains(word)) {
            if (polarity < 0) return 0;
            polarity = 1;
        } else if (kActionNegative.contains(word)) {
            if (polarity > 0) return 0;
            polarity = -1;
        }
    }
    return polarity;
}

std::set<std::string> numberTokens(const std::string& value) {
    std::set<std::string> result;
    for (std::sregex_iterator it(value.begin(), value.end(), numberTokenRe), end; it != end; ++it) result.insert(it->str());
    for (const auto& word : words(value)) if (kNumberWords.contains(word)) result.insert(word);
    return result;
}

std::set<std::string> extensionTokens(const std::string& value) {
    std::set<std::string> result;
    for (std::sregex_iterator it(value.begin(), value.end(), extensionTokenRe), end; it != end; ++it) {
        auto token = lower(it->str());
        if (token.size() > 1) result.insert(std::move(token));
    }
    return result;
}

std::set<std::string> filenameTokens(const std::string& value) {
    std::set<std::string> result;
    for (std::sregex_iterator it(value.begin(), value.end(), filenameTokenRe), end; it != end; ++it) result.insert(lower(it->str()));
    return result;
}

bool containsAny(const std::string& value, const std::set<std::string>& candidates) {
    for (const auto& word : words(value)) if (candidates.contains(word)) return true;
    return false;
}

bool knownCategoryDiffers(const std::string& left, const std::string& right) {
    for (const auto* category : {&kSeverity, &kDevices, &kPlatforms, &kEnvironments}) {
        const bool inLeft = containsAny(left, *category);
        const bool inRight = containsAny(right, *category);
        if (inLeft && inRight) {
            for (const auto& word : words(left))
                if (category->contains(word) && !containsAny(right, {word})) return true;
            for (const auto& word : words(right))
                if (category->contains(word) && !containsAny(left, {word})) return true;
        }
    }
    return false;
}

bool scopesAreDisjoint(const Requirement& left, const Requirement& right) {
    if (left.scope.empty() || right.scope.empty()) return false;
    const auto a = normalizedValue(left.scope);
    const auto b = normalizedValue(right.scope);
    if (a == b) return false;
    return knownCategoryDiffers(a, b);
}

const std::set<std::string> kWidthTerms{"wide", "width", "horizontal", "horizontally"};
const std::set<std::string> kHeightTerms{"high", "tall", "height", "vertical", "vertically"};
const std::set<std::string> kDepthTerms{"deep", "depth"};

enum class Dimension { None, Width, Height, Depth };

Dimension detectDimension(std::string_view text) {
    for (const auto& w : words(text)) {
        if (kWidthTerms.contains(w)) return Dimension::Width;
        if (kHeightTerms.contains(w)) return Dimension::Height;
        if (kDepthTerms.contains(w)) return Dimension::Depth;
    }
    return Dimension::None;
}

const std::unordered_map<std::string, double>& numberWordMap() {
    static const std::unordered_map<std::string, double> map{
        {"zero", 0}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5},
        {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10},
        {"eleven", 11}, {"twelve", 12}, {"thirteen", 13}, {"fourteen", 14}, {"fifteen", 15},
        {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19}, {"twenty", 20},
        {"thirty", 30}, {"forty", 40}, {"fifty", 50}, {"sixty", 60}
    };
    return map;
}

std::optional<double> parseNumberToken(std::string_view token) {
    const auto& map = numberWordMap();
    auto l = lower(token);
    auto it = map.find(l);
    if (it != map.end()) return it->second;
    try {
        std::size_t pos = 0;
        std::string s(token);
        double val = std::stod(s, &pos);
        if (pos > 0) return val;
    } catch (...) {}
    return std::nullopt;
}

std::optional<double> parseTimeQuantitySeconds(std::string_view text) {
    static const std::regex timeRe(
        "\\b([A-Za-z0-9.]+)\\s*(milliseconds?|ms|seconds?|secs?|s|minutes?|mins?|m|hours?|hrs?|h|days?|d)\\b",
        std::regex::icase | std::regex::optimize);
    std::string s(text);
    std::smatch m;
    if (std::regex_search(s, m, timeRe)) {
        auto numOpt = parseNumberToken(m[1].str());
        if (numOpt.has_value()) {
            double val = *numOpt;
            std::string unit = lower(m[2].str());
            if (unit == "ms" || unit.starts_with("milli")) return val * 0.001;
            if (unit == "s" || unit.starts_with("sec")) return val;
            if (unit == "m" || unit.starts_with("min")) return val * 60.0;
            if (unit == "h" || unit.starts_with("hour") || unit.starts_with("hr")) return val * 3600.0;
            if (unit == "d" || unit.starts_with("day")) return val * 86400.0;
        }
    }
    return std::nullopt;
}

bool valuesConflict(const Requirement& left, const Requirement& right) {
    const auto a = normalizedValue(left.value);
    const auto b = normalizedValue(right.value);
    if (a.empty() || b.empty() || a == b) return false;

    const int polarityA = actionPolarity(a);
    const int polarityB = actionPolarity(b);
    if (polarityA != 0 && polarityB != 0 && polarityA != polarityB) return true;

    // Time quantities with units: convert to seconds. Equivalent durations (e.g. 60s vs 1m)
    // are not conflicts; differing durations for the same subject are conflicts.
    const auto timeA = parseTimeQuantitySeconds(a);
    const auto timeB = parseTimeQuantitySeconds(b);
    if (timeA.has_value() && timeB.has_value()) {
        if (std::abs(*timeA - *timeB) < 1e-6) return false;
        return true;
    }

    // Orthogonal dimensions (e.g. 10 pixels wide vs 20 pixels high) do not conflict.
    const auto dimA = detectDimension(a);
    const auto dimB = detectDimension(b);
    if (dimA != Dimension::None && dimB != Dimension::None && dimA != dimB) {
        return false;
    }

    const auto numbersA = numberTokens(a);
    const auto numbersB = numberTokens(b);
    if (!numbersA.empty() && !numbersB.empty() && numbersA != numbersB) return true;

    const auto extensionsA = extensionTokens(a);
    const auto extensionsB = extensionTokens(b);
    if (!extensionsA.empty() && !extensionsB.empty() && extensionsA != extensionsB) return true;

    const auto filenamesA = filenameTokens(a);
    const auto filenamesB = filenameTokens(b);
    if (!filenamesA.empty() && !filenamesB.empty() && filenamesA != filenamesB) return true;

    const bool daysA = containsAny(a, kDays);
    const bool daysB = containsAny(b, kDays);
    if (daysA && daysB && a != b) {
        for (const auto& day : words(a)) if (kDays.contains(day) && !containsAny(b, {day})) return true;
    }

    const bool shapesA = containsAny(a, kShapes);
    const bool shapesB = containsAny(b, kShapes);
    if (shapesA && shapesB && a != b) {
        for (const auto& shape : words(a)) if (kShapes.contains(shape) && !containsAny(b, {shape})) return true;
    }

    const auto permissionA = normalizedValue(a);
    const auto permissionB = normalizedValue(b);
    const bool onlyA = permissionA.starts_with("only ");
    const bool onlyB = permissionB.starts_with("only ");
    const bool universalA = permissionA.starts_with("every ") || permissionA.starts_with("all ") ||
                            permissionA.starts_with("any ");
    const bool universalB = permissionB.starts_with("every ") || permissionB.starts_with("all ") ||
                            permissionB.starts_with("any ");
    if (onlyA || onlyB) {
        if (onlyA && universalB) return true;
        if (onlyB && universalA) return true;
        if (onlyA && onlyB) {
            auto targetA = normalizedSubject(permissionA.substr(5));
            auto targetB = normalizedSubject(permissionB.substr(5));
            if (!targetA.empty() && !targetB.empty() && targetA != targetB) return true;
        }
    }

    const bool negationA = std::regex_search(a, negationRe);
    const bool negationB = std::regex_search(b, negationRe);
    if (negationA != negationB) return true;

    return false;
}

void addRequirement(std::vector<Requirement>& requirements, Requirement requirement) {
    requirement.subject = trim(requirement.subject);
    requirement.value = trim(requirement.value);
    requirement.scope = trim(requirement.scope);
    if (requirement.subject.empty() || requirement.value.empty() || requirement.clause.empty()) return;
    const auto subject = normalizedSubject(requirement.subject);
    const auto value = normalizedValue(requirement.value);
    for (const auto& existing : requirements) {
        if (existing.clause == requirement.clause && normalizedSubject(existing.subject) == subject &&
            normalizedValue(existing.value) == value) {
            return;
        }
    }
    requirements.push_back(std::move(requirement));
}

void extractFromSentence(const Sentence& sentence, std::vector<Requirement>& requirements) {
    const auto clause = trimClause(sentence.text);
    if (clause.empty()) return;
    std::smatch match;

    if (std::regex_search(clause, match, mandatoryRe)) {
        addRequirement(requirements, {match[1].str(), match[2].str(), {}, clause, sentence.startByte, PatternKind::Mandatory});
    }
    if (std::regex_search(clause, match, quantifiedRe)) {
        addRequirement(requirements, {match[2].str(), match[1].str(), {}, clause, sentence.startByte, PatternKind::Quantified});
    }
    if (std::regex_search(clause, match, alwaysActionRe)) {
        addRequirement(requirements, {match[2].str(), match[1].str(), match[3].str(), clause, sentence.startByte,
                                      PatternKind::AlwaysAction});
    }
    if (std::regex_search(clause, match, imperativeIncludeExcludeRe)) {
        addRequirement(requirements, {match[2].str(), match[1].str(), match[3].str(), clause, sentence.startByte,
                                      PatternKind::Imperative});
    }
    if (std::regex_search(clause, match, filenameExtRe)) {
        const std::string subject = match[2].matched && !match[2].str().empty() ? match[2].str() : "filename extension";
        addRequirement(requirements, {subject, match[1].str(), {}, clause, sentence.startByte, PatternKind::FilenameExtension});
    }
    if (std::regex_search(clause, match, permissionRe)) {
        addRequirement(requirements, {match[3].str(), match[1].str() + " " + match[2].str(), {}, clause, sentence.startByte,
                                      PatternKind::Permission});
    }
    if (std::regex_search(clause, match, scopeActionRe)) {
        const std::string subject = match[2].matched && !match[2].str().empty() ? match[2].str() : match[1].str();
        addRequirement(requirements, {subject, match[1].str(), match[3].str(), clause, sentence.startByte, PatternKind::ScopeAction});
    }
}

struct Span {
    std::size_t start{0};
    std::size_t end{0};
};

std::vector<Span> extractQuotedSpans(std::string_view text) {
    std::vector<Span> spans;
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        if (i + 2 < n && (text.substr(i, 3) == "```" || text.substr(i, 3) == "\"\"\"")) {
            const std::string delim(text.substr(i, 3));
            std::size_t start = i;
            i += 3;
            auto pos = text.find(delim, i);
            if (pos == std::string_view::npos) i = n;
            else i = pos + 3;
            spans.push_back({start, i});
            continue;
        }
        if (text[i] == '"' || text[i] == '`') {
            const char q = text[i];
            std::size_t start = i;
            ++i;
            while (i < n && text[i] != q) {
                if (text[i] == '\\' && i + 1 < n) i += 2;
                else ++i;
            }
            if (i < n && text[i] == q) ++i;
            spans.push_back({start, i});
            continue;
        }
        if (text[i] == '\'') {
            const bool prevAlnum = (i > 0 && isWordCharacter(text[i - 1]));
            const bool nextAlnum = (i + 1 < n && isWordCharacter(text[i + 1]));
            if (prevAlnum && nextAlnum) {
                ++i;
                continue;
            }
            std::size_t start = i;
            ++i;
            while (i < n && text[i] != '\'') {
                if (text[i] == '\\' && i + 1 < n) i += 2;
                else ++i;
            }
            if (i < n && text[i] == '\'') ++i;
            spans.push_back({start, i});
            continue;
        }
        ++i;
    }
    return spans;
}

std::string maskEmbeddedQuotes(std::string_view text) {
    const auto spans = extractQuotedSpans(text);
    if (spans.empty()) return std::string(text);
    if (spans.size() == 1 && spans.front().start == 0 && spans.front().end == text.size()) {
        return std::string(text);
    }
    std::string masked(text);
    for (const auto& span : spans) {
        const std::size_t interiorStart = span.start + 1;
        const std::size_t interiorEnd = (span.end > span.start + 1) ? (span.end - 1) : span.end;
        for (std::size_t k = interiorStart; k < interiorEnd && k < masked.size(); ++k) {
            masked[k] = ' ';
        }
    }
    return masked;
}

std::vector<Sentence> splitSentences(std::string_view text) {
    std::vector<Sentence> sentences;
    std::size_t start = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        bool boundary = false;
        if (character == '\n' || character == '\r') {
            boundary = true;
        } else if (character == '!' || character == '?') {
            boundary = index + 1 >= text.size() || std::isspace(static_cast<unsigned char>(text[index + 1]));
        } else if (character == ';') {
            boundary = index + 1 >= text.size() || std::isspace(static_cast<unsigned char>(text[index + 1]));
        } else if (character == '.') {
            // Preserve decimals, filenames and abbreviations: only a period followed
            // by whitespace (or end of input) terminates a sentence.
            boundary = index + 1 >= text.size() || std::isspace(static_cast<unsigned char>(text[index + 1]));
        }
        if (!boundary) continue;
        if (index + 1 > start) sentences.push_back({std::string(text.substr(start, index + 1 - start)), start});
        start = index + 1;
    }
    if (start < text.size()) sentences.push_back({std::string(text.substr(start)), start});
    return sentences;
}

} // namespace

PreFlightExtractionReport StructuredEntityExtractor::analyze(std::string_view promptText) {
    PreFlightExtractionReport report;
    if (promptText.empty() || promptText.size() > kMaxPromptBytes) return report;

    const auto sanitizedPrompt = maskEmbeddedQuotes(promptText);
    const auto sentences = splitSentences(sanitizedPrompt);
    std::vector<Requirement> requirements;
    requirements.reserve(sentences.size() * 2);
    for (const auto& sentence : sentences) extractFromSentence(sentence, requirements);

    std::set<std::string> seen;
    for (std::size_t left = 0; left < requirements.size(); ++left) {
        for (std::size_t right = left + 1; right < requirements.size(); ++right) {
            const auto& a = requirements[left];
            const auto& b = requirements[right];
            if (a.clause == b.clause) continue;
            if (!sameSubject(a, b)) continue;
            if (scopesAreDisjoint(a, b)) continue;
            if (!valuesConflict(a, b)) continue;

            std::string first = a.clause;
            std::string second = b.clause;
            if (a.startByte > b.startByte) std::swap(first, second);
            const auto key = normalizedSubject(a.subject) + '\x1f' + first + '\x1f' + second;
            if (!seen.insert(key).second) continue;
            report.detectedExplicitConflicts.push_back({first, second, normalizedSubject(a.subject)});
        }
    }

    std::sort(report.detectedExplicitConflicts.begin(), report.detectedExplicitConflicts.end(),
              [](const DetectedConflict& left, const DetectedConflict& right) {
                  if (left.requirementA != right.requirementA) return left.requirementA < right.requirementA;
                  return left.requirementB < right.requirementB;
              });
    return report;
}

} // namespace roclint::core::rewrite
