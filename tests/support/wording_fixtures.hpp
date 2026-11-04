#pragma once
#include "support/fixtures.hpp"

namespace fx {
struct WordingCase {
    std::string original;
    std::string expected;
    std::string profile{"GENERIC@1"};
    std::string answer{};
};
inline std::vector<WordingCase> leanWordingCases() {
    return {
        {"Write a friendly two-sentence thank-you note.", "Write a friendly two-sentence thank-you note."},
        {"Make me a cool website.", "Build a website with a bold layout and playful visual accents.", "GENERIC@1", "Bold layout with playful visual accents"},
        {"Create an engaging, visually appealing website. Keep it usable and accessible.", "Create an engaging, visually appealing website. Keep it usable and accessible."},
        {"Improve this code without changing its API.", "Improve the implementation without changing its API."},
        {"Reduce the parser's runtime to less than 100 ms without changing its output.", "Reduce the parser's runtime to less than 100 ms without changing its output."},
        {"Summarize the supplied notes in three bullet points.", "# Goal\nSummarize the supplied notes in three bullet points.", "OPENAI_STRUCTURED@1"},
        {"Summarize the supplied notes in three bullet points.", "<task>Summarize the supplied notes in three bullet points.</task>", "ANTHROPIC_STRUCTURED@1"}
    };
}
inline roclint::bridge::GraniteRewriteRequest wordingRequest(const WordingCase& item, int index) {
    const auto session="sess_wording_"+std::to_string(index);
    const auto revision=roclint::core::RevisionState::makeRevisionId(roclint::core::SessionId(session),1).value();
    AnalysisBuilder builder(session,revision,item.original);
    if (!item.answer.empty()) builder.group("Which visual direction?", {item.answer,"Calm and minimal"}, "cool");
    Pipeline pipeline(item.original,builder.build());
    if (!item.answer.empty()) {
        ROCLINT_CHECK(roclint::core::intent::InterventionStateMachine::applyChoice(pipeline.analysis,pipeline.decision,pipeline.state,"alt_0_0").ok());
        pipeline.reevaluate();
    }
    roclint::bridge::GraniteRewriteRequest request;
    request.sessionId=session; request.revisionId=revision; request.originalText=item.original;
    request.analysis=finalSnapshot(pipeline.analysis,item.original,pipeline.eval);
    auto intent=roclint::core::intent::IntentProvenanceBuilder::buildConfirmedIntent(request.analysis,pipeline.policy,pipeline.state,1000);
    ROCLINT_CHECK(intent.ok()); request.confirmedIntent=intent.value(); request.rewriteProfileId=item.profile; request.proposalEditGeneration=1;
    return request;
}
} // namespace fx
