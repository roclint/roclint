#include "roclint/rewrite/RewriteProfile.hpp"

namespace roclint::core::rewrite {

namespace {

const RewriteProfileConfig kGenericProfile{
    .id = "GENERIC@1",
    .family = ProfileFamily::Generic,
    .version = 1,
    .displayName = "Generic Standard Prompt",
    .structuralInstructions = "Format prompt clearly with comments, task definition, and constraints.",
    .formatXmlTags = false,
    .formatMarkdownSections = true,
    .compactEncoding = false
};

const RewriteProfileConfig kAnthropicProfile{
    .id = "ANTHROPIC_STRUCTURED@1",
    .family = ProfileFamily::AnthropicStructured,
    .version = 1,
    .displayName = "Anthropic Claude Structured (XML)",
    .structuralInstructions = "Wrap instructions, context, and constraints in clean XML tags (<task>, <context>, <constraints>).",
    .formatXmlTags = true,
    .formatMarkdownSections = false,
    .compactEncoding = false
};

const RewriteProfileConfig kOpenAiProfile{
    .id = "OPENAI_STRUCTURED@1",
    .family = ProfileFamily::OpenAiStructured,
    .version = 1,
    .displayName = "OpenAI Structured Sections",
    .structuralInstructions = "Format with Markdown headings (# System / # Goal / # Instructions / # Output Format).",
    .formatXmlTags = false,
    .formatMarkdownSections = true,
    .compactEncoding = false
};

const RewriteProfileConfig kLocalCompactProfile{
    .id = "LOCAL_COMPACT@1",
    .family = ProfileFamily::LocalCompact,
    .version = 1,
    .displayName = "Local Compact (Token Optimized)",
    .structuralInstructions = "Concise, telegraphic prompt optimizing for local 3B model token budgets.",
    .formatXmlTags = false,
    .formatMarkdownSections = false,
    .compactEncoding = true
};

} // namespace

std::optional<RewriteProfileConfig> ProfileRegistry::getProfile(std::string_view profileId) noexcept {
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

std::vector<RewriteProfileConfig> ProfileRegistry::availableProfiles() noexcept {
    return {
        kGenericProfile,
        kAnthropicProfile,
        kOpenAiProfile,
        kLocalCompactProfile
    };
}

bool ProfileRegistry::isKnownProfile(std::string_view profileId) noexcept {
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
