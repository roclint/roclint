#include "roclint/core/prompt_state/PromptRevision.hpp"
#include <stdexcept> 

namespace roclint::core
{
    PromptRevision::PromptRevision(SessionId sessionId, RevisionId revisionId,
                               std::optional<RevisionId> parentRevisionId,
                               std::string text, std::int64_t createdAtMs)
                               : sessionId_(std::move(sessionId)),
                               revisionId_(std::move(revisionId)),
                               parentRevisionId_(std::move(parentRevisionId)),
                               prompt_(std::move(text)),
                               createdAtMs_(createdAtMs)
    {
        // 1. Prevent self-referential loops
        if (parentRevisionId_.has_value() && (*parentRevisionId_ == revisionId_))
        {
            throw std::invalid_argument("A PromptRevision cannot be its own parent.");
        }

        // 2. Reject invalid line endings (Bouncer pattern)
        if (prompt_.find('\r') != std::string::npos)
        {
            throw std::invalid_argument("Prompt text cannot contain carriage returns (CRLF/CR). Canonical LF required.");
        }

        // 3. Validate timestamp
        if (createdAtMs_ < 0)
        {
            throw std::invalid_argument("createdAtMs cannot be negative.");
        }
    }
}