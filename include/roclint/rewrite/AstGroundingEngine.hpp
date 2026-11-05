#pragma once

#include "roclint/rewrite/CanonicalIntent.hpp"
#include "roclint/storage/CodebaseIndex.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::rewrite {

class AstGroundingEngine {
public:
    // Matches raw canonical intents to indexed AST symbols. Unresolved targets fail
    // closed instead of being promoted to grounded model evidence.
    [[nodiscard]] static std::vector<CanonicalIntent> groundIntents(
        const std::vector<CanonicalIntent>& raw,
        std::string_view activeFile,
        const storage::CodebaseIndex& index);

    // Renders a bounded XML block for the local model. Signatures share the C10
    // budget; function bodies are never included.
    [[nodiscard]] static std::string renderPromptXml(const std::vector<CanonicalIntent>& intents,
                                                     std::size_t signatureBudget = 300);

    // SHA-256 over workspace identity plus every verified symbol's file hash,
    // byte span and signature. Empty when no intent is verified, which keeps
    // proposals on the legacy v1 hash domain.
    [[nodiscard]] static std::string computeAstSnapshotHash(
        const std::vector<CanonicalIntent>& grounded, std::string_view workspaceIdentity);

    static constexpr std::size_t kMaximumCandidates = 8;
};

} // namespace roclint::core::rewrite
