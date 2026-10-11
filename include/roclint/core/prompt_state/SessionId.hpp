#pragma once

#include <string>

namespace roclint::core 
{
    class SessionId 
    {
        private:
        std::string sessionId;
        public:
        // Constructor
        explicit SessionId(std::string id);
        
        //Getter for sessionId
        [[nodiscard]] const std::string& value() const noexcept 
        { 
            return sessionId; 
        }
        
        bool operator==(const SessionId& other) const noexcept 
        {
            return sessionId == other.sessionId;
        }
};
}