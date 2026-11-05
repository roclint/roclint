#include "roclint/rewrite/AstGroundingEngine.hpp"

#include "roclint/core/crypto/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

namespace roclint::core::rewrite {
namespace {

const std::regex scopeRe(
    R"(\b(?:in|within|inside)\s+((?:`[^`]+`|\*\*[^*]+\*\*|__[^_]+__|"[^"]+"|'[^']+'|[A-Za-z_][A-Za-z0-9_:]*)))",
    std::regex::icase | std::regex::optimize);

std::string trimText(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

// Strips code-formatting wrappers that can survive canonical extraction. The
// intent keeps its original text; only symbol matching uses this normalized form.
std::string normalizeIdentifier(std::string_view raw) {
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
    while (!text.empty() && (text.back() == '.' || text.back() == ',' || text.back() == ';' ||
                             text.back() == '!' || text.back() == '?')) {
        text.pop_back();
    }
    return trimText(text);
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

std::string escapeXml(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (const char character : text) {
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

// Truncates already-escaped text without splitting an XML entity, so the
// signature remains well-formed inside the C10 byte budget.
std::string truncateEscaped(const std::string& escaped, std::size_t budget) {
    if (escaped.size() <= budget) return escaped;
    auto cut = budget;
    const auto ampersand = escaped.rfind('&', cut);
    if (ampersand != std::string::npos) {
        const auto semicolon = escaped.find(';', ampersand);
        if (semicolon == std::string::npos || semicolon >= cut) cut = ampersand;
    }
    return escaped.substr(0, cut);
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

int rank(const storage::AstSymbol& symbol, const CanonicalIntent& intent, std::string_view activeFile,
         std::string_view normalizedTarget, std::string_view resolvedScope) {
    int score = 0;
    if (!activeFile.empty() && symbol.filePath == activeFile) score += 1000;
    if (!activeFile.empty() && directoryOf(symbol.filePath) == directoryOf(activeFile)) score += 100;
    if (symbol.qualifiedName == normalizedTarget) score += 200;
    if (!resolvedScope.empty() && symbol.qualifiedName.starts_with(std::string(resolvedScope) + "::")) score += 80;
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

bool isScopeKind(storage::AstNodeKind kind) {
    return kind == storage::AstNodeKind::Namespace || kind == storage::AstNodeKind::Class ||
           kind == storage::AstNodeKind::Struct || kind == storage::AstNodeKind::Enum ||
           kind == storage::AstNodeKind::Function || kind == storage::AstNodeKind::Method;
}

} // namespace

std::vector<CanonicalIntent> AstGroundingEngine::groundIntents(const std::vector<CanonicalIntent>& raw,
                                                               std::string_view activeFile,
                                                               const storage::CodebaseIndex& index) {
    auto grounded = raw;
    for (auto& intent : grounded) {
        if (intent.grounding.has_value()) continue;

        const auto normalizedTarget = normalizeIdentifier(intent.target);
        const auto name = targetName(normalizedTarget);
        if (name.empty()) continue;

        std::string scope;
        std::smatch scopeMatch;
        if (std::regex_search(intent.condition, scopeMatch, scopeRe))
            scope = normalizeIdentifier(scopeMatch[1].str());
        const bool explicitScope = !scope.empty();

        std::vector<storage::AstSymbol> scopeCandidates;
        if (explicitScope) {
            const auto exact = index.findQualified(scope);
            if (exact.ok()) scopeCandidates = exact.value();
            if (scopeCandidates.empty()) {
                const auto matches = index.findByName(scope, {}, {}, 32, false);
                if (matches.ok()) scopeCandidates = matches.value();
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
        if (explicitScope && !scopeMissing) {
            // Query the requested child directly under each resolved scope. A
            // fixed-window child enumeration could miss late members and must
            // never be treated as proof that the target is absent.
            for (const auto& scopeSymbol : scopeCandidates) {
                const auto matches = index.findByName(name, scopeSymbol.symbolId, {}, 64, false);
                if (!matches.ok()) continue;
                for (const auto& child : matches.value()) candidates.push_back(child);
            }
        } else if (!explicitScope) {
            if (normalizedTarget.find("::") != std::string::npos) {
                const auto exact = index.findQualified(normalizedTarget);
                if (exact.ok()) candidates = exact.value();
            } else {
                const auto matches = index.findByName(name, {}, {}, 64, false);
                if (matches.ok()) candidates = matches.value();
            }
        }
        deduplicate(candidates);

        if (candidates.empty()) {
            AstGroundingEvidence evidence;
            if (explicitScope) {
                if (scopeMissing || scopeCandidates.size() == 1) {
                    evidence.status = GroundingStatus::NotFoundInScope;
                    evidence.qualifiedTarget = scope + "::" + name;
                } else {
                    // The scope itself could not be narrowed to one symbol, and no
                    // child resolved it. Never widen this to a global lookup.
                    evidence.status = GroundingStatus::Ambiguous;
                    evidence.qualifiedTarget = scope + "::" + name;
                    evidence.candidates = candidateLabels(scopeCandidates);
                }
            } else if (normalizedTarget.find("::") != std::string::npos ||
                       requiresExistingTarget(intent.action)) {
                evidence.status = GroundingStatus::NotFoundInScope;
                evidence.qualifiedTarget = normalizedTarget;
            } else {
                evidence.status = GroundingStatus::Ungrounded;
            }
            intent.grounding = std::move(evidence);
            continue;
        }

        std::stable_sort(candidates.begin(), candidates.end(), [&](const auto& left, const auto& right) {
            return rank(left, intent, activeFile, normalizedTarget, scope) >
                   rank(right, intent, activeFile, normalizedTarget, scope);
        });
        const auto bestScore = rank(candidates.front(), intent, activeFile, normalizedTarget, scope);
        std::vector<const storage::AstSymbol*> tied;
        for (const auto& candidate : candidates) {
            if (rank(candidate, intent, activeFile, normalizedTarget, scope) == bestScore)
                tied.push_back(&candidate);
        }
        if (tied.size() > 1) {
            auto evidence = evidenceFor(*tied.front(), GroundingStatus::Ambiguous);
            evidence.qualifiedTarget = explicitScope ? scope + "::" + name : normalizedTarget;
            std::vector<std::string> candidatesByName;
            for (const auto* candidate : tied) candidatesByName.push_back(candidateLabel(*candidate));
            std::sort(candidatesByName.begin(), candidatesByName.end());
            candidatesByName.erase(std::unique(candidatesByName.begin(), candidatesByName.end()),
                                   candidatesByName.end());
            evidence.candidates = std::move(candidatesByName);
            if (evidence.candidates.size() > kMaximumCandidates) evidence.candidates.resize(kMaximumCandidates);
            intent.grounding = std::move(evidence);
            continue;
        }

        const auto& selected = candidates.front();
        const bool typeMismatch =
            (intent.property == "parameter" && selected.kind != storage::AstNodeKind::Parameter) ||
            (intent.property == "field" && selected.kind != storage::AstNodeKind::Field);
        if (typeMismatch) {
            intent.grounding = evidenceFor(selected, GroundingStatus::TypeMismatch);
            continue;
        }
        intent.grounding = evidenceFor(selected, GroundingStatus::Verified);
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

std::string AstGroundingEngine::renderPromptXml(const std::vector<CanonicalIntent>& intents,
                                                std::size_t signatureBudget) {
    std::ostringstream output;
    output << "<canonical_intents>\n";
    std::size_t used = 0;
    for (const auto& intent : intents) {
        output << "  <intent action=\"" << toString(intent.action) << "\" target=\"" << escapeXml(intent.target)
               << "\"";
        if (intent.grounding.has_value()) {
            output << " status=\"" << groundingName(intent.grounding->status) << "\"";
            if (intent.grounding->status == GroundingStatus::Verified) {
                output << ">\n";
                output << "    <grounded_symbol>" << escapeXml(intent.grounding->qualifiedTarget)
                       << "</grounded_symbol>\n";
                output << "    <location file=\"" << escapeXml(intent.grounding->filePath)
                       << "\" line=\"" << intent.grounding->line << "\" />\n";
                const auto signature = escapeXml(intent.grounding->signature);
                if (!signature.empty() && used < signatureBudget) {
                    const auto rendered = truncateEscaped(signature, signatureBudget - used);
                    if (!rendered.empty()) {
                        output << "    <signature>" << rendered << "</signature>\n";
                        used += rendered.size();
                    }
                }
                output << "  </intent>\n";
                continue;
            }
            output << ">\n";
            for (const auto& candidate : intent.grounding->candidates)
                output << "    <candidate>" << escapeXml(candidate) << "</candidate>\n";
            output << "  </intent>\n";
            continue;
        }
        output << " status=\"UNGROUNDED\" />\n";
    }
    output << "</canonical_intents>";
    return output.str();
}

} // namespace roclint::core::rewrite
