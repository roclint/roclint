#include "roclint/core/prompt_state/RevisionId.hpp"
#include <stdexcept>

namespace roclint::core 
{
    RevisionId::RevisionId(std::string id)
    {
        bool isBlank = true;
        for (char c : id) 
        {
            if (c != ' ' && c != '\t' && c != '\n') 
            {
                isBlank = false;
                break;
            }
        }

        if (isBlank) 
        {
            throw std::invalid_argument("RevisionId cannot be empty or blank");
        }

        revisionId = id;
    }
}