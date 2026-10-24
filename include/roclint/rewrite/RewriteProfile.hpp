#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::rewrite {

enum class ProfileFamily {
    Generic,
    AnthropicStructured,
    OpenAiStructured,
    LocalCompact
};

struct RewriteProfileConfig {
    std::string id{};             // e.g. "GENERIC@1", "ANTHROPIC_STRUCTURED@1"
    ProfileFamily family{ProfileFamily::Generic};
    unsigned int version{1};
    std::string displayName{};
    std::string structuralInstructions{};
    bool formatXmlTags{false};
    bool formatMarkdownSections{false};
    bool compactEncoding{false};

    bool operator==(const RewriteProfileConfig&) const = default;
};

class ProfileRegistry {
public:
    [[nodiscard]] static std::optional<RewriteProfileConfig> getProfile(std::string_view profileId);
    [[nodiscard]] static const RewriteProfileConfig& defaultProfile() noexcept;
    [[nodiscard]] static std::vector<RewriteProfileConfig> availableProfiles();
    [[nodiscard]] static bool isKnownProfile(std::string_view profileId);
};

[[nodiscard]] const char* toString(ProfileFamily family) noexcept;

} // namespace roclint::core::rewrite
