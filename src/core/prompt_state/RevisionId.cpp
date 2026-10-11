#include "roclint/core/prompt_state/RevisionId.hpp"
#include <stdexcept>

namespace roclint::core 
{
    // Constructor that initializes the RevisionId with a given string.
    RevisionId::RevisionId(std::string id)
    {
        bool isBlank = true;
        // Check if the provided id is blank (only whitespace characters).
        for (char c : id) 
        {
            if (c != ' ' && c != '\t' && c != '\n') 
            {
                isBlank = false;
                break;
            }
        }

        // If the id is blank, throw an exception to indicate that a valid RevisionId cannot be empty or blank.
        if (isBlank) 
        {
            throw std::invalid_argument("RevisionId cannot be empty or blank");
        }

        revisionId = id;
    }
}