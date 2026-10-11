#include "roclint/core/prompt_state/SessionId.hpp"
#include <stdexcept>

namespace roclint::core 
{
    SessionId::SessionId(std::string id)
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
            throw std::invalid_argument("SessionId cannot be empty or blank");
        }

        sessionId = id;
    }


}