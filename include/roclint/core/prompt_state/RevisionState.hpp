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
        // Constructor
        explicit RevisionState(const PromptRevision &);
        // Method to check if the state is stale based on the expected version
        bool isStale(std::uint64_t) const noexcept;
        void commitEdit(std::uint64_t, const RevisionId &,  const std::string &, std::uint64_t);


        //getters
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