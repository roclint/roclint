#include "roclint/rewrite/RewriteProfile.hpp"

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
    .displayName = "Claude (XML sections)",
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
    if (profileId == "GENERIC@1" || profileId == "GENERIC") {
        return kGenericProfile;
    }
    if (profileId == "ANTHROPIC_STRUCTURED@1" || profileId == "ANTHROPIC_STRUCTURED") {
        return kAnthropicProfile;
    }
    if (profileId == "OPENAI_STRUCTURED@1" || profileId == "OPENAI_STRUCTURED") {
        return kOpenAiProfile;
    }
    if (profileId == "LOCAL_COMPACT@1" || profileId == "LOCAL_COMPACT") {
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
