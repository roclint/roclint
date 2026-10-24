#include "roclint/core/prompt_state/RevisionState.hpp"
#include "support/check.hpp"

#include <iostream>

using namespace roclint::core;

namespace {

PromptRevision rootRevision(const std::string& session = "sess_01") {
    const SessionId sid(session);
    return PromptRevision(sid, RevisionState::makeRevisionId(sid, 1), std::nullopt, "Initial prompt", 1000);
}

void testRevisionIdFormat() {
    ROCLINT_CHECK(RevisionState::makeRevisionId(SessionId("sess_01"), 1).value() == "rev_sess_01_1");
    ROCLINT_CHECK(RevisionState::makeRevisionId(SessionId("sess_01"), 42).value() == "rev_sess_01_42");
    // Root revisions in different sessions never collide.
    ROCLINT_CHECK(!(RevisionState::makeRevisionId(SessionId("sess_a"), 1) ==
                    RevisionState::makeRevisionId(SessionId("sess_b"), 1)));
}

void testInitialState() {
    RevisionState state(rootRevision());
    ROCLINT_CHECK(!state.isStale(1));
    ROCLINT_CHECK(state.isStale(2));
    ROCLINT_CHECK(state.isStale(0));
    ROCLINT_CHECK(state.status() == RevisionStatus::Clean);
    ROCLINT_CHECK(state.draftEditGeneration() == 0);
    ROCLINT_CHECK(state.proposalEditGeneration() == 0);
}

void testValidCommit() {
    RevisionState state(rootRevision());
    auto id = state.commitEdit(1, "Updated prompt", 2000);
    ROCLINT_CHECK(id.ok());
    ROCLINT_CHECK(id.value().value() == "rev_sess_01_2");
    ROCLINT_CHECK(state.stateVersion() == 2);
    ROCLINT_CHECK(state.currentRevision().text() == "Updated prompt");
    ROCLINT_CHECK(state.currentRevision().parentRevisionId()->value() == "rev_sess_01_1");
    ROCLINT_CHECK(state.highestIssuedSequence() == 2);
}

void testStaleCommitReturnsErrorWithoutMutation() {
    RevisionState state(rootRevision());
    ROCLINT_CHECK(state.commitEdit(1, "Updated prompt", 2000).ok());
    auto stale = state.commitEdit(1, "Stale edit", 3000);
    ROCLINT_CHECK(!stale.ok());
    ROCLINT_CHECK(stale.error().code == ErrorCode::StaleVersion);
    ROCLINT_CHECK(state.stateVersion() == 2);
    ROCLINT_CHECK(state.currentRevision().revisionId().value() == "rev_sess_01_2");
}

void testCommitNormalizesLineEndingsWithoutThrowing() {
    RevisionState state(rootRevision());
    auto crlf = state.commitEdit(1, "line one\r\nline two\rline three", 2000);
    ROCLINT_CHECK(crlf.ok());
    ROCLINT_CHECK(state.currentRevision().text() == "line one\nline two\nline three");
}

void testNegativeTimestampIsInvalidInput() {
    RevisionState state(rootRevision());
    auto bad = state.commitEdit(1, "text", -5);
    ROCLINT_CHECK(!bad.ok());
    ROCLINT_CHECK(bad.error().code == ErrorCode::InvalidInput);
    ROCLINT_CHECK(state.stateVersion() == 1);
}

void testDraftDirtyLifecycle() {
    RevisionState state(rootRevision());
    ROCLINT_CHECK(state.noteDraftEdit(1).ok());
    ROCLINT_CHECK(state.isDraftDirty());
    ROCLINT_CHECK(state.draftEditGeneration() == 1);
    ROCLINT_CHECK(state.stateVersion() == 2); // every accepted mutation advances the version

    auto stale = state.noteDraftEdit(7);
    ROCLINT_CHECK(!stale.ok());
    ROCLINT_CHECK(stale.error().code == ErrorCode::StaleVersion);
    ROCLINT_CHECK(state.draftEditGeneration() == 1);

    ROCLINT_CHECK(state.commitEdit(2, "frozen", 2000).ok());
    ROCLINT_CHECK(state.status() == RevisionStatus::Clean);
    ROCLINT_CHECK(state.draftEditGeneration() == 1); // generations are monotonic
}

void testProposalGeneration() {
    RevisionState state(rootRevision());
    ROCLINT_CHECK(state.noteProposalEdit(state.stateVersion()).ok());
    ROCLINT_CHECK(state.noteProposalEdit(state.stateVersion()).ok());
    ROCLINT_CHECK(state.proposalEditGeneration() == 2);
}

void testRestoreNeverReissuesSequence() {
    // Restoring revision 2 of a session that already issued 5 sequences continues at 6.
    const SessionId sid("sess_01");
    PromptRevision restored(sid, RevisionState::makeRevisionId(sid, 2), RevisionState::makeRevisionId(sid, 1),
                            "older text", 1500);
    RevisionState state(restored, 5, 9);
    auto id = state.commitEdit(9, "new text", 4000);
    ROCLINT_CHECK(id.ok());
    ROCLINT_CHECK(id.value().value() == "rev_sess_01_6");
}

void testEveryMutationRejectsAnOldVersionWithoutChangingState() {
    RevisionState state(rootRevision());
    ROCLINT_CHECK(state.advanceState(1).ok());
    const auto version = state.stateVersion();
    ROCLINT_CHECK(!state.noteDraftEdit(1).ok());
    ROCLINT_CHECK(!state.noteProposalEdit(1).ok());
    ROCLINT_CHECK(!state.advanceState(1).ok());
    ROCLINT_CHECK(state.stateVersion() == version && state.status() == RevisionStatus::Clean);
    ROCLINT_CHECK(state.draftEditGeneration() == 0 && state.proposalEditGeneration() == 0);
    RevisionState exhausted(rootRevision(), 1, RevisionState::kMaxStateVersion);
    ROCLINT_CHECK(!exhausted.advanceState(exhausted.stateVersion()).ok());
    ROCLINT_CHECK(!exhausted.noteDraftEdit(exhausted.stateVersion()).ok());
    ROCLINT_CHECK(!exhausted.noteProposalEdit(exhausted.stateVersion()).ok());
    ROCLINT_CHECK(exhausted.stateVersion() == RevisionState::kMaxStateVersion);
    ROCLINT_CHECK(exhausted.status() == RevisionStatus::Clean && exhausted.draftEditGeneration() == 0);
}

void testLineageChain() {
    RevisionState state(rootRevision());
    ROCLINT_CHECK(state.commitEdit(1, "Second prompt", 2000).ok());
    ROCLINT_CHECK(state.commitEdit(2, "Third prompt", 3000).ok());
    ROCLINT_CHECK(state.stateVersion() == 3);
    ROCLINT_CHECK(state.currentRevision().revisionId().value() == "rev_sess_01_3");
    ROCLINT_CHECK(state.currentRevision().parentRevisionId()->value() == "rev_sess_01_2");
}

} // namespace

int main() {
    std::cout << "[Test RevisionState] Starting...\n";
    testRevisionIdFormat();
    testInitialState();
    testValidCommit();
    testStaleCommitReturnsErrorWithoutMutation();
    testCommitNormalizesLineEndingsWithoutThrowing();
    testNegativeTimestampIsInvalidInput();
    testDraftDirtyLifecycle();
    testProposalGeneration();
    testRestoreNeverReissuesSequence();
    testEveryMutationRejectsAnOldVersionWithoutChangingState();
    testLineageChain();
    std::cout << "[Test RevisionState] All tests PASSED.\n";
    return 0;
}
