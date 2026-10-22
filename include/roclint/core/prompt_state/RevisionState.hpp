#pragma once
#include <string>
#include <cstdint>
#include "roclint/core/prompt_state/PromptRevision.hpp"



namespace roclint::core
{
    class RevisionState
    {
        private:
         PromptRevision currentRevision_;
         std::uint64_t stateVersion_{1};
        public:
        explicit RevisionState(const PromptRevision &);
        bool isStale(std::uint64_t) const noexcept;
        void commitEdit(std::uint64_t, const RevisionId &,  const std::string &, std::uint64_t);

        [[nodiscard]] std::uint64_t stateVersion() const noexcept 
        { 
            return stateVersion_; 
        }
        [[nodiscard]] const PromptRevision& currentRevision() const noexcept 
        { 
            return currentRevision_; 
        }

    };
}