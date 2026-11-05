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

const std::regex scopeRe(R"(\b(?:in|within|inside)\s+([A-Za-z_][A-Za-z0-9_:]*))",
                         std::regex::icase | std::regex::optimize);

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string targetName(std::string_view target) {
    const auto separator = target.rfind("::");
    return std::string(separator == std::string_view::npos ? target : target.substr(separator + 2));
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
         std::string_view resolvedScope) {
    int score = 0;
    if (!activeFile.empty() && symbol.filePath == activeFile) score += 1000;
    if (!activeFile.empty() && directoryOf(symbol.filePath) == directoryOf(activeFile)) score += 100;
    if (symbol.qualifiedName == intent.target) score += 200;
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

} // namespace

std::vector<CanonicalIntent> AstGroundingEngine::groundIntents(const std::vector<CanonicalIntent>& raw,
                                                               std::string_view activeFile,
                                                               const storage::CodebaseIndex& index) {
    auto grounded = raw;
    for (auto& intent : grounded) {
        if (intent.grounding.has_value()) continue;
        const auto name = targetName(intent.target);
        if (name.empty()) continue;

        std::string scope;
        std::smatch scopeMatch;
        if (std::regex_search(intent.condition, scopeMatch, scopeRe)) scope = scopeMatch[1].str();
        std::optional<storage::AstSymbol> scopeSymbol;
        std::vector<storage::AstSymbol> scopeCandidates;
        bool scopeAmbiguous = false;
        if (!scope.empty()) {
            const auto exact = index.findQualified(scope);
            if (exact.ok() && exact.value().has_value()) scopeSymbol = exact.value();
            else {
                const auto matches = index.findByName(scope, {}, {}, 8, false);
                if (matches.ok() && !matches.value().empty()) {
                    if (matches.value().size() == 1) scopeSymbol = matches.value().front();
                    else {
                        scopeCandidates = matches.value();
                        scopeAmbiguous = true;
                    }
                }
            }
        }

        std::vector<storage::AstSymbol> candidates;
        const bool scopeResolved = !scope.empty() && scopeSymbol.has_value();
        if (scopeResolved) {
            const auto exact = index.findByName(name, scopeSymbol->symbolId, {}, 32, false);
            if (exact.ok()) candidates = exact.value();
        } else if (intent.target.find("::") != std::string::npos) {
            const auto exact = index.findQualified(intent.target);
            if (exact.ok() && exact.value().has_value()) candidates.push_back(*exact.value());
        }
        if (candidates.empty() && scope.empty()) {
            const auto matches = index.findByName(name, {}, {}, 32, false);
            if (matches.ok()) candidates = matches.value();
        }

        if (candidates.empty()) {
            AstGroundingEvidence evidence;
            evidence.status = (!scope.empty() || requiresExistingTarget(intent.action))
                ? GroundingStatus::NotFoundInScope : GroundingStatus::Ungrounded;
            if (!scope.empty()) evidence.qualifiedTarget = scope + "::" + name;
            evidence.candidates = scopeCandidates.empty() ? std::vector<std::string>{} :
                [&]() {
                    std::vector<std::string> names;
                    for (const auto& candidate : scopeCandidates) names.push_back(candidate.qualifiedName);
                    return names;
                }();
            intent.grounding = std::move(evidence);
            continue;
        }

        std::stable_sort(candidates.begin(), candidates.end(), [&](const auto& left, const auto& right) {
            return rank(left, intent, activeFile, scope) > rank(right, intent, activeFile, scope);
        });
        const auto bestScore = rank(candidates.front(), intent, activeFile, scope);
        std::vector<std::string> tied;
        for (const auto& candidate : candidates) {
            if (rank(candidate, intent, activeFile, scope) == bestScore)
                tied.push_back(candidate.qualifiedName);
        }
        if (scopeAmbiguous && tied.size() > 1) {
            AstGroundingEvidence evidence;
            evidence.status = GroundingStatus::Ambiguous;
            evidence.qualifiedTarget = scope + "::" + name;
            evidence.candidates = std::move(tied);
            for (const auto& candidate : scopeCandidates) evidence.candidates.push_back(candidate.qualifiedName);
            std::sort(evidence.candidates.begin(), evidence.candidates.end());
            evidence.candidates.erase(std::unique(evidence.candidates.begin(), evidence.candidates.end()),
                                      evidence.candidates.end());
            if (evidence.candidates.size() > kMaximumCandidates) evidence.candidates.resize(kMaximumCandidates);
            intent.grounding = std::move(evidence);
            continue;
        }
        if (tied.size() > 1) {
            auto evidence = evidenceFor(candidates.front(), GroundingStatus::Ambiguous);
            evidence.candidates = std::move(tied);
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
