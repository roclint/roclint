#pragma once
#include "GraniteEngine.hpp"
#include <QJsonObject>
#include <QString>
#include <algorithm>
#include <cmath>

namespace roclint::runtime {
struct GraniteReasoningPlan {
    bool enabled{false};
    int reasoningTokens{0};
    int answerTokens{0};
    QString profile;
    int reservedTokens() const { return reasoningTokens + answerTokens + (enabled ? 1 : 0); }
};
inline GraniteReasoningPlan graniteReasoningPlan(const QJsonObject& manifest, const QString& operation,
                                                const QString& instructions, bool referenceDisabled = false) {
    const auto policy = manifest.value("reasoning").toObject();
    GraniteReasoningPlan plan;
    plan.profile = instructions.startsWith(QLatin1String("<instructions>\nInstruction contract: granite.goldfish_")) ? QStringLiteral("audit")
        : operation == QLatin1String("granite_word_refinement") ? QStringLiteral("word")
        : operation == QLatin1String("granite_rewrite") ? QStringLiteral("rewrite") : QStringLiteral("analysis");
    const auto profile = policy.value("profiles").toObject().value(plan.profile).toObject();
    const auto integer = [](const QJsonValue& value) {
        const auto n = value.toDouble(-1);
        if (!value.isDouble() || !std::isfinite(n) || n != std::floor(n) || n < 0 || n > 8192)
            throw GraniteFailure(QStringLiteral("CONFIGURATION"), "Invalid bounded reasoning token budget.");
        return static_cast<int>(n);
    };
    plan.enabled = policy.value("enabled").toBool() && !referenceDisabled;
    plan.reasoningTokens = plan.enabled ? integer(profile.value("reasoning_tokens")) : 0;
    plan.answerTokens = integer(profile.value("answer_tokens"));
    const int cap = integer(manifest.value("output_tokens"));
    if (plan.answerTokens < 1 || (plan.enabled && plan.reasoningTokens < 1) || plan.reservedTokens() > cap)
        throw GraniteFailure(QStringLiteral("CONFIGURATION"), "Reasoning and answer budgets exceed the native output reservation.");
    return plan;
}
} // namespace roclint::runtime
