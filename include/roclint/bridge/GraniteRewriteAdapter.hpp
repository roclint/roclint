#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/core/Result.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace roclint::bridge {

inline constexpr std::size_t kMaxRewriteResponseBytes = 64 * 1024;

struct GraniteRewriteRequest {
    std::string sessionId{};
    std::string revisionId{};
    std::string originalText{};                    // Frozen revision text (canonical LF)
    core::analysis::AnalysisSnapshot analysis{};   // Final snapshot the intent is bound to
    core::intent::ConfirmedIntent confirmedIntent{};
    std::string rewriteProfileId{"GENERIC@1"};
    std::string groundedCanonicalIntentXml{};      // Optional AST-grounded block from the host index
    std::string astSnapshotHash{};                 // Optional hash of verified AST evidence (proposal v2)
    std::uint64_t proposalEditGeneration{0};
};

// XML text/attribute escaping for user-authored content embedded in model instructions.
[[nodiscard]] std::string xmlEscape(std::string_view text);

// Builds the instruction text for the rewrite model. It carries only the user's prompt, the
// user's explicit decisions, and the profile's formatting instruction.
[[nodiscard]] std::string buildRewritePrompt(const GraniteRewriteRequest& request,
                                             const core::rewrite::RewriteProfileConfig& profile);

// JSON Schema for the rewrite reply; the reply is still strictly validated by parseRewriteResponse.
[[nodiscard]] const std::string& rewriteResponseSchema();

class GraniteRewriteAdapter {
public:
    // Produces and seals a RewriteProposal from a live model call. Fails closed when the runtime
    // is unavailable or the model output is malformed. Never commits or copies anything.
    [[nodiscard]] static core::Result<core::rewrite::RewriteProposal> synthesizeRewrite(
        const GraniteRewriteRequest& request,
        GraniteAdapter& runtime,
        const GraniteTransport& transport,
        std::int64_t nowMs);

    // Parses the model's JSON rewrite reply and returns a sealed proposal.
    [[nodiscard]] static core::Result<core::rewrite::RewriteProposal> parseRewriteResponse(
        std::string_view modelOutput,
        const GraniteRewriteRequest& request,
        const core::rewrite::RewriteProfileConfig& profile,
        const GraniteModelInfo& model,
        std::int64_t nowMs);
};

} // namespace roclint::bridge
