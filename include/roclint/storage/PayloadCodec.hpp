#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/analysis/AnalysisSnapshot.hpp"
#include "roclint/core/Result.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"

#include <string>
#include <string_view>

namespace roclint::storage {

[[nodiscard]] std::string encodePromptTriage(const core::analysis::PromptTriage& triage);
[[nodiscard]] core::Result<core::analysis::PromptTriage> decodePromptTriage(std::string_view payload);

[[nodiscard]] std::string encodeAnalysis(const core::analysis::NormalizedAnalysis& analysis);
[[nodiscard]] core::Result<core::analysis::NormalizedAnalysis> decodeAnalysis(std::string_view payload);

[[nodiscard]] std::string encodeAnalysisSnapshot(const core::analysis::AnalysisSnapshot& snapshot);
[[nodiscard]] core::Result<core::analysis::AnalysisSnapshot> decodeAnalysisSnapshot(std::string_view payload);

[[nodiscard]] std::string encodeIntent(const core::intent::ConfirmedIntent& intent);
[[nodiscard]] core::Result<core::intent::ConfirmedIntent> decodeIntent(std::string_view payload);

[[nodiscard]] std::string encodeProposal(const core::rewrite::RewriteProposal& proposal);
[[nodiscard]] core::Result<core::rewrite::RewriteProposal> decodeProposal(std::string_view payload);

} // namespace roclint::storage
