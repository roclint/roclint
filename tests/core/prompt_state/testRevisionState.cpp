#include "roclint/core/prompt_state/RevisionState.hpp"
#include "support/check.hpp"
#include <iostream>
#include <stdexcept>

using namespace roclint::core;

void testInitialState();
void testValidEditCommit();
void testStaleEditCommitThrows();
void testLineageChain();

int main()
{
    std::cout << "[Test RevisionState] Starting...\n";
    testInitialState();
    testValidEditCommit();
    testStaleEditCommitThrows();
    testLineageChain();
    std::cout << "[Test RevisionState] All tests PASSED.\n";
    return 0;
}

void testInitialState()
{
    PromptRevision root(SessionId("sess_01"), RevisionId("rev_01"), 
                       std::nullopt, "Initial prompt", 1000);

    RevisionState state(root);

    ROCLINT_CHECK(state.isStale(1) == false);
    ROCLINT_CHECK(state.isStale(2) == true);
    ROCLINT_CHECK(state.isStale(0) == true);


}

void testValidEditCommit()
{
    PromptRevision root(SessionId("sess_01"), RevisionId("rev_01"), 
                       std::nullopt, "Initial prompt", 1000);
    RevisionState state(root);
    state.commitEdit(1, RevisionId("rev_02"), "Updated prompt", 2000);

    ROCLINT_CHECK(state.stateVersion() == 2);
    ROCLINT_CHECK(state.isStale(1) == true); 
    ROCLINT_CHECK(state.isStale(2) == false); 
    ROCLINT_CHECK(state.currentRevision().text() == "Updated prompt");
    ROCLINT_CHECK(state.currentRevision().revisionId().value() == "rev_02");
    ROCLINT_CHECK(state.currentRevision().parentRevisionId()->value() == "rev_01");
}
void testStaleEditCommitThrows()
{
    PromptRevision root(SessionId("sess_01"), RevisionId("rev_01"), 
                       std::nullopt, "Initial prompt", 1000);
    RevisionState state(root);
    
    // First advance to version 2
    state.commitEdit(1, RevisionId("rev_02"), "Updated prompt", 2000);
    ROCLINT_CHECK(state.stateVersion() == 2);

    // Now try to commit with an old expectedVersion (1)
    bool exceptionThrown = false;
    try
    {
        state.commitEdit(1, RevisionId("rev_03"), "Stale edit", 3000);
    }
    catch (const std::runtime_error&)
    {
        exceptionThrown = true;
    }
    ROCLINT_CHECK(exceptionThrown);

    // Verify state was NOT modified
    ROCLINT_CHECK(state.stateVersion() == 2);
    ROCLINT_CHECK(state.currentRevision().revisionId().value() == "rev_02");
}

void testLineageChain()
{
    PromptRevision root(SessionId("sess_01"), RevisionId("rev_01"), 
                       std::nullopt, "Initial prompt", 1000);
    RevisionState state(root);
    state.commitEdit(1, RevisionId("rev_02"), "Second prompt", 2000);
    state.commitEdit(2, RevisionId("rev_03"), "Third prompt", 3000);

    ROCLINT_CHECK(state.stateVersion() == 3);
    ROCLINT_CHECK(state.currentRevision().text() == "Third prompt");
    ROCLINT_CHECK(state.currentRevision().revisionId().value() == "rev_03");
    ROCLINT_CHECK(state.currentRevision().parentRevisionId()->value() == "rev_02");   
}