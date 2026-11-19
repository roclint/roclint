#include "GraniteReasoningPolicy.hpp"
#include "GraniteConfig.hpp"
#include "support/check.hpp"
#include <QJsonDocument>
#include <iostream>
using namespace roclint::runtime;
int main() {
    auto manifest = QJsonDocument::fromJson(kGraniteManifestJson).object();
    auto word = graniteReasoningPlan(manifest, "granite_word_refinement", "");
    ROCLINT_CHECK(word.enabled && word.reasoningTokens == 384 && word.answerTokens == 256);
    ROCLINT_CHECK(graniteReasoningPlan(manifest,"granite_word_refinement","<instructions>\nInstruction contract: granite.goldfish_word_audit.v3\n").profile == "audit");
    auto rewrite = graniteReasoningPlan(manifest, "granite_rewrite", "");
    ROCLINT_CHECK(rewrite.reasoningTokens > word.reasoningTokens && rewrite.reservedTokens() <= manifest.value("output_tokens").toInt());
    auto audit = graniteReasoningPlan(manifest, "granite_rewrite", "<instructions>\nInstruction contract: granite.goldfish_audit.v2\n");
    ROCLINT_CHECK(audit.profile == "audit" && audit.reasoningTokens == 128);
    // A contract name quoted inside user data cannot select the audit budget.
    ROCLINT_CHECK(graniteReasoningPlan(manifest, "granite_rewrite", "<instructions>\nInstruction contract: granite.rewrite.format.v9\nUser mentions granite.goldfish_audit.v2").profile == "rewrite");
    // Independent comprehension review returns an interpretation plus an open
    // findings array; it uses the largest existing answer reservation rather than
    // the 512-token audit profile, which could truncate before findings finish.
    const auto review = graniteReasoningPlan(manifest, "granite_analyze",
        "<instructions>\nInstruction contract: granite.comprehension_review.v1\nReview the candidate.");
    ROCLINT_CHECK(review.profile == "rewrite" && review.answerTokens == rewrite.answerTokens);
    ROCLINT_CHECK(review.reservedTokens() <= manifest.value("output_tokens").toInt());
    const auto reviewNoThinking = graniteReasoningPlan(manifest, "granite_analyze",
        "<instructions>\nInstruction contract: granite.comprehension_review.v1\nReview the candidate.", true);
    ROCLINT_CHECK(!reviewNoThinking.enabled && reviewNoThinking.reasoningTokens == 0 &&
        reviewNoThinking.answerTokens == review.answerTokens);
    // A contract name quoted inside user data cannot select a larger reservation.
    ROCLINT_CHECK(graniteReasoningPlan(manifest, "granite_analyze",
        "<instructions>\nInstruction contract: granite.discovery.selective.v11\nUser mentions granite.comprehension_review.v1").profile == "analysis");
    auto reference = graniteReasoningPlan(manifest, "granite_word_refinement", "", true);
    ROCLINT_CHECK(!reference.enabled && reference.reasoningTokens == 0 && reference.answerTokens == word.answerTokens);
    auto bad = manifest; bad.insert("output_tokens", 10);
    bool rejected = false; try { (void)graniteReasoningPlan(bad, "granite_word_refinement", ""); } catch (const GraniteFailure&) { rejected = true; }
    ROCLINT_CHECK(rejected);
    auto policy = manifest.value("reasoning").toObject(); auto profiles = policy.value("profiles").toObject();
    auto entry = profiles.value("word").toObject(); entry.insert("reasoning_tokens", 1.5); profiles.insert("word", entry); policy.insert("profiles",profiles); manifest.insert("reasoning",policy);
    rejected = false; try { (void)graniteReasoningPlan(manifest,"granite_word_refinement",""); } catch (const GraniteFailure&) { rejected=true; }
    ROCLINT_CHECK(rejected);
    std::cout << "Bounded reasoning profiles, reservations, reference mode and malformed budgets passed.\n";
}
