#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <iostream>

using namespace roclint::core;
using namespace roclint::core::rewrite;
using namespace roclint::bridge;

namespace {

GraniteRewriteRequest requestFor(const fx::Chain& c, const std::string& profile = "GENERIC@1") {
    GraniteRewriteRequest req;
    req.sessionId = c.session;
    req.revisionId = c.revision;
    req.originalText = fx::kChainPrompt;
    req.analysis = c.snapshot;
    req.confirmedIntent = c.intent;
    req.rewriteProfileId = profile;
    return req;
}

void makeReady(GraniteAdapter& runtime) {
    ROCLINT_CHECK(runtime.beginLoading());
    ROCLINT_CHECK(runtime.completeLoading(fx::testModel()));
}

void testSynthesizeProducesValidatedProposalOnly() {
    fx::Chain c;
    GraniteAdapter runtime;
    makeReady(runtime);
    std::string instruction;
    auto p = GraniteRewriteAdapter::synthesizeRewrite(
        requestFor(c, "ANTHROPIC_STRUCTURED@1"), runtime,
        [&](std::string_view in) -> Result<std::string> {
            instruction = std::string(in);
            return fx::rewriteReply("<task>Refactor the parser for lower latency per call.</task>");
        },
        5000);
    ROCLINT_CHECK(p.ok());
    ROCLINT_CHECK(instruction.find("&lt;task&gt;&lt;/task&gt;") != std::string::npos);
    ROCLINT_CHECK(runtime.state() == GraniteRuntimeState::Ready);
    const auto r = RewriteValidator::validateProposal(p.value(), c.session, c.revision, c.intent, {});
    ROCLINT_CHECK(r.isValid);
    ROCLINT_CHECK(p.value().rewriteProfileId == "ANTHROPIC_STRUCTURED@1");
    ROCLINT_CHECK(p.value().getConstraint("profile_structure")->status == ConstraintStatus::MechanicallyVerified);
}

void testSynthesizeFailsClosed() {
    fx::Chain c;
    auto echo = [](std::string_view) -> Result<std::string> { return fx::rewriteReply("x"); };

    GraniteAdapter unloaded;
    auto notReady = GraniteRewriteAdapter::synthesizeRewrite(requestFor(c), unloaded, echo, 1);
    ROCLINT_CHECK(!notReady.ok() && notReady.error().code == ErrorCode::ProviderUnavailable);

    GraniteAdapter busy;
    makeReady(busy);
    {
        auto lease = busy.tryAcquire();
        ROCLINT_CHECK(lease.has_value());
        ROCLINT_CHECK(!GraniteRewriteAdapter::synthesizeRewrite(requestFor(c), busy, echo, 1).ok());
    }

    GraniteAdapter ready;
    makeReady(ready);
    ROCLINT_CHECK(!GraniteRewriteAdapter::synthesizeRewrite(requestFor(c), ready, GraniteTransport{}, 1).ok());
    ROCLINT_CHECK(!GraniteRewriteAdapter::synthesizeRewrite(requestFor(c, "UNKNOWN@1"), ready, echo, 1).ok());

    auto noIntent = requestFor(c);
    noIntent.confirmedIntent = {};
    ROCLINT_CHECK(!GraniteRewriteAdapter::synthesizeRewrite(noIntent, ready, echo, 1).ok());

    auto wrongRevision = requestFor(c);
    wrongRevision.revisionId = "rev_sess_a_2";
    ROCLINT_CHECK(!GraniteRewriteAdapter::synthesizeRewrite(wrongRevision, ready, echo, 1).ok());

    auto empty = GraniteRewriteAdapter::synthesizeRewrite(
        requestFor(c), ready, [](std::string_view) -> Result<std::string> { return fx::rewriteReply("  \n"); }, 1);
    ROCLINT_CHECK(!empty.ok());
    ROCLINT_CHECK(ready.state() == GraniteRuntimeState::Ready);

    auto prose = GraniteRewriteAdapter::synthesizeRewrite(
        requestFor(c), ready, [](std::string_view) -> Result<std::string> { return std::string("Here you go!"); }, 1);
    ROCLINT_CHECK(!prose.ok());

    auto down = GraniteRewriteAdapter::synthesizeRewrite(
        requestFor(c), ready,
        [](std::string_view) -> Result<std::string> { return makeError(ErrorCode::ProviderUnavailable, "refused"); },
        1);
    ROCLINT_CHECK(!down.ok());
    ROCLINT_CHECK(ready.state() == GraniteRuntimeState::Failed);
}

void testModelCrlfIsNormalized() {
    fx::Chain c;
    auto p = GraniteRewriteAdapter::parseRewriteResponse(fx::rewriteReply("line one\r\nlower latency per call"),
                                                         requestFor(c), ProfileRegistry::defaultProfile(),
                                                         fx::testModel(), 1);
    ROCLINT_CHECK(p.ok());
    ROCLINT_CHECK(p.value().rewrittenText == "line one\nlower latency per call");
    ROCLINT_CHECK(RewriteValidator::validateProposal(p.value(), c.session, c.revision, c.intent, {}).isValid);
}

void testOversizedReplyRejected() {
    fx::Chain c;
    const std::string huge = fx::rewriteReply(std::string(kMaxRewriteResponseBytes, 'x'));
    ROCLINT_CHECK(!GraniteRewriteAdapter::parseRewriteResponse(huge, requestFor(c), ProfileRegistry::defaultProfile(),
                                                               fx::testModel(), 1)
                       .ok());
}

} // namespace

int main() {
    std::cout << "[Test Rewrite Checkpoint] Starting...\n";
    testSynthesizeProducesValidatedProposalOnly();
    testSynthesizeFailsClosed();
    testModelCrlfIsNormalized();
    testOversizedReplyRejected();
    std::cout << "[Test Rewrite Checkpoint] All tests PASSED.\n";
    return 0;
}
