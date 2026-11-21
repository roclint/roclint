#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::analysis {

enum class DataHazardType {
    None,
    ConcurrencyRaceCondition,
    WildcardKeystrokeSearch,
    UnboundedCascadeDelete,
    InsecureDirectObjectReference
};

[[nodiscard]] constexpr std::string_view toString(DataHazardType hazard) noexcept {
    switch (hazard) {
        case DataHazardType::None: return "NONE";
        case DataHazardType::ConcurrencyRaceCondition: return "CONCURRENCY_RACE_CONDITION";
        case DataHazardType::WildcardKeystrokeSearch: return "WILDCARD_KEYSTROKE_SEARCH";
        case DataHazardType::UnboundedCascadeDelete: return "UNBOUNDED_CASCADE_DELETE";
        case DataHazardType::InsecureDirectObjectReference: return "INSECURE_DIRECT_OBJECT_REFERENCE";
    }
    return "UNKNOWN";
}

struct DataHazardResult {
    DataHazardType type{DataHazardType::None};
    std::string suggestedFocus{};
    std::string suggestedQuestion{};
    std::string primaryOptionText{};
    std::string primaryOptionEffect{};
    std::string secondaryOptionText{};
    std::string secondaryOptionEffect{};
};

class DataHazardPolicy {
public:
    [[nodiscard]] static std::vector<DataHazardResult> scanPrompt(std::string_view prompt);
    [[nodiscard]] static bool hasHazard(std::string_view prompt, DataHazardType type);
};

} // namespace roclint::core::analysis
