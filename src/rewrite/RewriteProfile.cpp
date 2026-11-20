#include "roclint/rewrite/RewriteProfile.hpp"
#include <cctype>
#include <string>

namespace roclint::core::rewrite {

namespace {

const RewriteProfileConfig kGenericProfile{
    .id = "GENERIC@1",
    .family = ProfileFamily::Generic,
    .version = 1,
    .displayName = "Plain prose",
    .structuralInstructions = "Write the request as clear, plain prose.",
    .formatXmlTags = false,
    .formatMarkdownSections = false,
    .compactEncoding = false
};

const RewriteProfileConfig kAnthropicProfile{
    .id = "ANTHROPIC_STRUCTURED@1",
    .family = ProfileFamily::AnthropicStructured,
    .version = 1,
    .displayName = "Anthropic (XML sections)",
    .structuralInstructions = "Put the request inside <task></task> tags and any background inside <context></context> tags.",
    .formatXmlTags = true,
    .formatMarkdownSections = false,
    .compactEncoding = false
};

const RewriteProfileConfig kOpenAiProfile{
    .id = "OPENAI_STRUCTURED@1",
    .family = ProfileFamily::OpenAiStructured,
    .version = 1,
    .displayName = "OpenAI (Markdown sections)",
    .structuralInstructions = "Separate the request into sections with Markdown headings such as '# Goal' and '# Context'.",
    .formatXmlTags = false,
    .formatMarkdownSections = true,
    .compactEncoding = false
};

const RewriteProfileConfig kLocalCompactProfile{
    .id = "LOCAL_COMPACT@1",
    .family = ProfileFamily::LocalCompact,
    .version = 1,
    .displayName = "Compact",
    .structuralInstructions = "Keep the request short and direct.",
    .formatXmlTags = false,
    .formatMarkdownSections = false,
    .compactEncoding = true
};

} // namespace

std::optional<RewriteProfileConfig> ProfileRegistry::getProfile(std::string_view profileId) {
    std::string upper;
    upper.reserve(profileId.size());
    for (char c : profileId) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (upper == "GENERIC@1" || upper == "GENERIC") {
        return kGenericProfile;
    }
    if (upper == "ANTHROPIC_STRUCTURED@1" || upper == "ANTHROPIC_STRUCTURED" || upper == "ANTHROPIC") {
        return kAnthropicProfile;
    }
    if (upper == "OPENAI_STRUCTURED@1" || upper == "OPENAI_STRUCTURED" || upper == "OPENAI") {
        return kOpenAiProfile;
    }
    if (upper == "LOCAL_COMPACT@1" || upper == "LOCAL_COMPACT" || upper == "COMPACT") {
        return kLocalCompactProfile;
    }
    return std::nullopt;
}

const RewriteProfileConfig& ProfileRegistry::defaultProfile() noexcept {
    return kGenericProfile;
}

std::vector<RewriteProfileConfig> ProfileRegistry::availableProfiles() {
    return {kGenericProfile, kAnthropicProfile, kOpenAiProfile, kLocalCompactProfile};
}

bool ProfileRegistry::isKnownProfile(std::string_view profileId) {
    return getProfile(profileId).has_value();
}

const char* toString(ProfileFamily family) noexcept {
    switch (family) {
        case ProfileFamily::Generic: return "GENERIC";
        case ProfileFamily::AnthropicStructured: return "ANTHROPIC_STRUCTURED";
        case ProfileFamily::OpenAiStructured: return "OPENAI_STRUCTURED";
        case ProfileFamily::LocalCompact: return "LOCAL_COMPACT";
    }
    return "UNKNOWN";
}

} // namespace roclint::core::rewrite
