#pragma once
#include <string>
#include <cstdint>
#include <optional>
#include "roclint/core/prompt_state/SessionId.hpp"
#include "roclint/core/prompt_state/RevisionId.hpp"

namespace roclint::core 
{
  class PromptRevision 
  {
     private:
     SessionId sessionId_;
     RevisionId revisionId_;
     std::optional<RevisionId> parentRevisionId_;
     std::string prompt_;
     std::int64_t createdAtMs_;

   

   public:
    // Constructor
     PromptRevision(SessionId sessionId, RevisionId revisionId, 
                    std::optional<RevisionId> parentRevisionId,
                    std::string text, std::int64_t createdAtMs = 0);
    // Getters
    [[nodiscard]] const SessionId& sessionId() const noexcept 
    { 
        return sessionId_; 
    }
    [[nodiscard]] const RevisionId& revisionId() const noexcept 
    { 
        return revisionId_; 
    }
    [[nodiscard]] const std::optional<RevisionId>& parentRevisionId() const noexcept 
    { 
        return parentRevisionId_; 
    }
    [[nodiscard]] const std::string& text() const noexcept 
    { 
        return prompt_; 
    }
    [[nodiscard]] std::int64_t createdAtMs() const noexcept 
    { 
        return createdAtMs_; 
    }
    bool operator == (const PromptRevision& other) const noexcept = default;
  };

}




