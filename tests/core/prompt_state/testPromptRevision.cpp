//Test for PromptRevision
#include "roclint/core/prompt_state/SessionId.hpp"
#include "roclint/core/prompt_state/RevisionId.hpp"
#include "roclint/core/prompt_state/PromptRevision.hpp"
#include "support/check.hpp"
#include <iostream>
#include <optional>
#include <stdexcept>
using namespace roclint::core;

void testSessionId();
void testRevisionId();
void testRootRevision();
void testChildRevisionSemantics();
void testBouncerInvariants();
int main()
{
    std::cout << "[Test PromptRevision] Starting...\n";
    testSessionId();
    testRevisionId();
    testRootRevision();
    testChildRevisionSemantics();
    testBouncerInvariants();
    return 0;
}

void testSessionId()
{
    bool exceptionThrown = false;
    try
    {
        SessionId sessionId("");

    }
    catch (const std::invalid_argument& e)
        {
            exceptionThrown = true;
        }
        ROCLINT_CHECK(exceptionThrown);

        exceptionThrown = false;

        try
        {
            SessionId sessionId("   ");
        }
        catch (const std::invalid_argument& e)
        {
            exceptionThrown = true;
        }
        ROCLINT_CHECK(exceptionThrown);


        SessionId id("sess_101");
        ROCLINT_CHECK(id.value() == "sess_101");

    }
void testRevisionId()
{
    bool exceptionThrown = false;
    try
    {
        RevisionId revisionId("");
    }
    catch (const std::invalid_argument& e)
    {
        exceptionThrown = true;
    }
    ROCLINT_CHECK(exceptionThrown);

    exceptionThrown = false;

    try
    {
        RevisionId revisionId("   ");
    }
    catch (const std::invalid_argument& e)
    {
        exceptionThrown = true;
    }
    ROCLINT_CHECK(exceptionThrown);

    RevisionId id("rev_202");
    ROCLINT_CHECK(id.value() == "rev_202");
}

void testRootRevision()
{
    SessionId sessionId("sess_101");
    RevisionId revisionId("rev_202");
    std::optional<RevisionId> parentRevisionId = std::nullopt;
    std::string text = "This is a test prompt.";
    std::int64_t createdAtMs = 1234567890;

    PromptRevision rootRevision(sessionId, revisionId, parentRevisionId, text, createdAtMs);

    ROCLINT_CHECK(rootRevision.sessionId() == sessionId);
    ROCLINT_CHECK(rootRevision.revisionId() == revisionId);
    ROCLINT_CHECK(!rootRevision.parentRevisionId().has_value());
    ROCLINT_CHECK(rootRevision.text() == text);
    ROCLINT_CHECK(rootRevision.createdAtMs() == createdAtMs);
}

void testChildRevisionSemantics()
{

    
    SessionId session{"session-001"};
    RevisionId rootRevId{"rev-001"};
    RevisionId childRevId{"rev-002"};
    std::int64_t createdAtMs = 1700000000000;

    //parentRevision 
    PromptRevision rootRev{
        session,
        rootRevId,
        std::nullopt,                 
        "You are a helpful assistant.", 
        createdAtMs               
    };

    // Child Revision (Has a parent)
    PromptRevision childRev
    {
        session,
        childRevId,
        rootRev.revisionId(),       
        "You are a helpful assistant.\nAlways format code cleanly.", 
        createdAtMs + 1000

    };

    //duplicate Revision
    PromptRevision duplicateRev{
        session,
        rootRevId,
        std::nullopt,           
        "You are a helpful assistant.", 
        createdAtMs
    };
    ROCLINT_CHECK(childRev.parentRevisionId().has_value());
    ROCLINT_CHECK(rootRev.revisionId() == duplicateRev.revisionId());
    ROCLINT_CHECK(childRev.parentRevisionId().value() == rootRevId);
    ROCLINT_CHECK(rootRev == duplicateRev);
    ROCLINT_CHECK(rootRev != childRev);
}

void testBouncerInvariants()
{
    SessionId session{"session-001"};
    RevisionId rootRevId{"rev-001"};
    std::int64_t invalidTimeStamp = -1700000000000; // Invalid negative timestamp
    std::int64_t timeStamp = 1700000000000; // Valid timestamp
    std::string invalidText= "This is a prompt will fail.\r\n"; //Invalid text
    std::string text= "This is a prompt will pass.";
    bool exceptionThrown = false;

    try
    {
        PromptRevision invalidParentRev{
            session,
            rootRevId,
            rootRevId,  // Self-referential parent
            text,
            timeStamp
        };
    }
    catch (const std::invalid_argument& e)
    {
       exceptionThrown = true;
    }
    ROCLINT_CHECK(exceptionThrown);

    exceptionThrown = false;

    try
    {
        PromptRevision invalidTextRev{
            session,
            rootRevId,
            std::nullopt,  // No parent
            invalidText,   // Invalid text with CRLF
            timeStamp
        };
    }
    catch (const std::invalid_argument& e)
    {
       exceptionThrown = true;
    }
    ROCLINT_CHECK(exceptionThrown);
    
    exceptionThrown = false;

    try
    {
        PromptRevision invalidTimestampRev{
            session,
            rootRevId,
            std::nullopt,  // No parent
            text,
            invalidTimeStamp // Invalid negative timestamp
        };
    }
    catch (const std::invalid_argument& e)
    {
       exceptionThrown = true;
    }
    ROCLINT_CHECK(exceptionThrown);
}



   
