#pragma once

#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"

#include <optional>
#include <string>

namespace roclint::bridge {

struct GraniteRewriteRequest {
    std::string sessionId{};
    std::string revisionId{};
    std::string promptString{};
    core::intent::ConfirmedIntent confirmedIntent{};
    std::string rewriteProfileId{"GENERIC@1"};
    std::string modelQuantization{"Q8_0"};
};

struct GraniteRewriteResponse {
    bool success{false};
    std::optional<core::rewrite::RewriteProposal> proposal{};
    std::string errorMessage{};
};

class GraniteRewriteAdapter {
public:
    explicit GraniteRewriteAdapter(std::string modelPath = "");

    [[nodiscard]] GraniteRewriteResponse synthesizeRewrite(
        const GraniteRewriteRequest& request) noexcept;

    [[nodiscard]] std::string activeQuantization() const noexcept { return "Q8_0"; }
    [[nodiscard]] std::string modelPath() const noexcept { return m_modelPath; }

private:
    std::string m_modelPath;
};

} // namespace roclint::bridge
