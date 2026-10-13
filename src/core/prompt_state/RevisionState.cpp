#include <string>
#include <cstdint>
#include <stdexcept>
#include <limits>
#include "roclint/core/prompt_state/RevisionState.hpp"

namespace roclint::core
{
    RevisionState::RevisionState(const PromptRevision &revision)
        : currentRevision_(revision), stateVersion_(1) 
    {
    }
    bool RevisionState::isStale(std::uint64_t expectedVersion) const noexcept
    {
        return expectedVersion != stateVersion_;
    }

    void RevisionState::commitEdit(std::uint64_t expectedVersion, const RevisionId &newRevisionId, 
                                   const std::string &newPromptText, std::uint64_t createdAtMs)
    {
        if (isStale(expectedVersion))
        {
            throw std::runtime_error("Cannot commit changes: the state is stale.");
        }

        PromptRevision newRevision(
            currentRevision_.sessionId(),
            newRevisionId,
            currentRevision_.revisionId(), // Set the current revision as the parent
            newPromptText,
            createdAtMs
        );

        if (stateVersion_ != std::numeric_limits<std::uint64_t>::max())
        {
            currentRevision_ = newRevision;
             ++stateVersion_;
         
        }
        else
        {
            throw std::runtime_error("Error: Maximum state version");
        }



    }

}