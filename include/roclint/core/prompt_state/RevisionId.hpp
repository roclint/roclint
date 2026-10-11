#pragma once
#include <string>

namespace roclint::core 
{
    class RevisionId 
    {
    private:
        std::string revisionId;

    public:

        //constructor
        explicit RevisionId(std::string id);

        //Getter for revisionId
        [[nodiscard]] const std::string& value() const noexcept 
        { 
            return revisionId; 
        }

        bool operator==(const RevisionId& other) const noexcept 
        {
            return revisionId == other.revisionId;
        }
    };
}