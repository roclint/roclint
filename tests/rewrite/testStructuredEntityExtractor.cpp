#include "roclint/rewrite/StructuredEntityExtractor.hpp"
#include "roclint/analysis/SourceSpan.hpp"
#include "roclint/core/Json.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using roclint::core::rewrite::CanonicalIntent;
using roclint::core::rewrite::DetectedConflict;
using roclint::core::rewrite::IntentAction;
using roclint::core::rewrite::StructuredEntityExtractor;

namespace {

bool detected(std::string_view prompt) {
    return !StructuredEntityExtractor::analyze(prompt).detectedExplicitConflicts.empty();
}

const DetectedConflict* firstConflict(std::string_view prompt) {
    static thread_local auto report = StructuredEntityExtractor::analyze(prompt);
    report = StructuredEntityExtractor::analyze(prompt);
    return report.detectedExplicitConflicts.empty() ? nullptr : &report.detectedExplicitConflicts.front();
}

void checkFragmentsResolve(std::string_view prompt) {
    const auto report = StructuredEntityExtractor::analyze(prompt);
    ROCLINT_CHECK(!report.detectedExplicitConflicts.empty());
    for (const auto& conflict : report.detectedExplicitConflicts) {
        const auto first = roclint::core::analysis::findUniqueSpan(prompt, conflict.requirementA);
        const auto second = roclint::core::analysis::findUniqueSpan(prompt, conflict.requirementB);
        ROCLINT_CHECK(first.status == roclint::core::analysis::SpanMatchStatus::ExactMatch);
        ROCLINT_CHECK(second.status == roclint::core::analysis::SpanMatchStatus::ExactMatch);
        ROCLINT_CHECK(conflict.requirementA != conflict.requirementB);
        ROCLINT_CHECK(!conflict.subject.empty());
    }
}

std::string lowerCopy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return out;
}

bool containsCaseInsensitive(std::string_view text, std::string_view needle) {
    return lowerCopy(text).find(lowerCopy(needle)) != std::string::npos;
}

const CanonicalIntent* findIntent(const std::vector<CanonicalIntent>& intents, IntentAction action,
                                  std::string_view targetPart) {
    for (const auto& intent : intents) {
        if (intent.action == action && containsCaseInsensitive(intent.target, targetPart)) return &intent;
    }
    return nullptr;
}

void testMandatorySameTargetConflict() {
    const std::string prompt = "The refresh interval must be 15 seconds. The refresh interval must be 60 seconds.";
    ROCLINT_CHECK(detected(prompt));
    checkFragmentsResolve(prompt);
    const auto* conflict = firstConflict(prompt);
    ROCLINT_CHECK(conflict != nullptr);
    ROCLINT_CHECK(conflict->subject.find("refresh interval") != std::string::npos);
    ROCLINT_CHECK(conflict->requirementA.find("15 seconds") != std::string::npos);
    ROCLINT_CHECK(conflict->requirementB.find("60 seconds") != std::string::npos);
}

void testAlwaysIncludeOmitConflict() {
    const std::string prompt = "Always include empty categories. Always omit empty categories.";
    ROCLINT_CHECK(detected(prompt));
    checkFragmentsResolve(prompt);
    ROCLINT_CHECK(firstConflict(prompt)->subject.find("empty categor") != std::string::npos);
}

void testQuantifiedConflict() {
    const std::string prompt = "Use exactly one decimal place. Use exactly three decimal places.";
    ROCLINT_CHECK(detected(prompt));
    checkFragmentsResolve(prompt);
    ROCLINT_CHECK(firstConflict(prompt)->subject.find("decimal place") != std::string::npos);
}

void testFilenameExtensionConflict() {
    const std::string prompt = "Use extension .jpeg. Use extension .jpg.";
    ROCLINT_CHECK(detected(prompt));
    checkFragmentsResolve(prompt);
}

void testImperativeIncludeExcludeConflict() {
    const std::string prompt = "Include the sender address. Exclude the sender address.";
    ROCLINT_CHECK(detected(prompt));
    checkFragmentsResolve(prompt);
    ROCLINT_CHECK(firstConflict(prompt)->subject.find("sender address") != std::string::npos);
}

void testFilenameValueConflict() {
    const std::string prompt = "The output must be summary.txt. The output must be overview.txt.";
    ROCLINT_CHECK(detected(prompt));
    checkFragmentsResolve(prompt);
}

void testDayAndShapeConflicts() {
    ROCLINT_CHECK(detected("The task must start on Monday. The task must start on Sunday."));
    ROCLINT_CHECK(detected("The shape must be a circle. The shape must be a square."));
}

void testPermissionConflict() {
    const std::string prompt = "Only managers may approve. Every employee may approve.";
    ROCLINT_CHECK(detected(prompt));
    checkFragmentsResolve(prompt);
    ROCLINT_CHECK(firstConflict(prompt)->subject.find("approve") != std::string::npos);
}

void testVisibilityConflict() {
    const std::string prompt =
        "The modal must remain visible until dismissed. The modal must disappear after two seconds.";
    ROCLINT_CHECK(detected(prompt));
    checkFragmentsResolve(prompt);
}

void testDisjointScopesAreNotConflicts() {
    ROCLINT_CHECK(!detected("Enable sound for critical alerts. Disable sound for informational alerts."));
    ROCLINT_CHECK(!detected("Expand on desktop. Collapse on mobile."));
}

void testCompatibleRequirementsAreNotConflicts() {
    ROCLINT_CHECK(!detected("The button must be blue. The button must be visible."));
    ROCLINT_CHECK(!detected("The panel must be visible on desktop. The panel must be visible on mobile."));
    ROCLINT_CHECK(!detected("The header must include a logo. The footer must include a logo."));
    ROCLINT_CHECK(!detected("Use exactly one decimal place. Use exactly one decimal place."));
    ROCLINT_CHECK(!detected("Rename the local variable offset to cursor in readPacket."));
    ROCLINT_CHECK(!detected(""));
}

void testVerificationProbeCases() {
    // 1. Quoted examples / verbatim data
    ROCLINT_CHECK(!detected(
        "Print these examples verbatim: \"The interval must be 15 seconds. The interval must be 60 seconds.\""));
    ROCLINT_CHECK(!detected(
        "Here is an example: 'The interval must be 15 seconds. The interval must be 60 seconds.'"));
    ROCLINT_CHECK(!detected(
        "In the docstring write: ```The interval must be 15 seconds. The interval must be 60 seconds.```"));

    // 2. Equivalent units
    ROCLINT_CHECK(!detected("The timeout must be 60 seconds. The timeout must be 1 minute."));
    ROCLINT_CHECK(!detected("The duration must be 1 hour. The duration must be 60 minutes."));
    ROCLINT_CHECK(!detected("The latency must be 1000 ms. The latency must be 1 second."));

    // 3. Independent dimensions
    ROCLINT_CHECK(!detected("The panel must be 10 pixels wide. The panel must be 20 pixels high."));
    ROCLINT_CHECK(!detected("The window must be 100 px in width. The window must be 200 px in height."));

    // 4. Equivalent permissions
    ROCLINT_CHECK(!detected("Every employee may approve. All employees may approve."));
    ROCLINT_CHECK(!detected("Any user may edit. All users may edit."));
}

void testCanonicalIntentActionTriples() {
    const std::string prompt =
        "Add a Save button to Settings.qml. Remove the legacy cache. "
        "Rename offset to cursor in readPacket. Preserve the public API. "
        "The timeout must be 15 seconds.";
    const auto report = StructuredEntityExtractor::analyze(prompt);
    const auto& intents = report.canonicalIntents;
    ROCLINT_CHECK(intents.size() >= 5);

    const auto* add = findIntent(intents, IntentAction::ADD, "save button");
    ROCLINT_CHECK(add != nullptr);
    ROCLINT_CHECK(containsCaseInsensitive(add->value, "settings.qml"));
    const auto* remove = findIntent(intents, IntentAction::REMOVE, "legacy cache");
    ROCLINT_CHECK(remove != nullptr);
    const auto* rename = findIntent(intents, IntentAction::MODIFY, "offset");
    ROCLINT_CHECK(rename != nullptr);
    ROCLINT_CHECK(containsCaseInsensitive(rename->value, "cursor"));
    ROCLINT_CHECK(containsCaseInsensitive(rename->condition, "readpacket"));
    ROCLINT_CHECK(rename->property == "name");
    const auto* preserve = findIntent(intents, IntentAction::PRESERVE, "public api");
    ROCLINT_CHECK(preserve != nullptr);
    const auto* constrain = findIntent(intents, IntentAction::CONSTRAIN, "timeout");
    ROCLINT_CHECK(constrain != nullptr);
    ROCLINT_CHECK(containsCaseInsensitive(constrain->value, "15 seconds"));

    std::set<std::string> nodeIds;
    for (const auto& intent : intents) {
        ROCLINT_CHECK(intent.nodeId.starts_with("nid_"));
        ROCLINT_CHECK(intent.nodeId.size() == 28);
        ROCLINT_CHECK(!intent.target.empty());
        ROCLINT_CHECK(intent.disposition == "explicit_prompt");
        ROCLINT_CHECK(roclint::core::analysis::isValidSpan(prompt, intent.source));
        ROCLINT_CHECK(nodeIds.insert(intent.nodeId).second);
    }
}

void testCanonicalIntentNegationAndNestedConditions() {
    const std::string prompt =
        "Do not modify tests. "
        "MUST use JSON EXCEPT for logs ONLY_IF debug is enabled. "
        "Preserve this example as data: \"Remove the cache.\"";
    const auto report = StructuredEntityExtractor::analyze(prompt);
    const auto& intents = report.canonicalIntents;
    ROCLINT_CHECK(intents.size() >= 2);
    const auto* preserve = findIntent(intents, IntentAction::PRESERVE, "tests");
    ROCLINT_CHECK(preserve != nullptr);
    ROCLINT_CHECK(preserve->value == "unchanged");
    bool conditionalFound = false;
    for (const auto& intent : intents) {
        if (containsCaseInsensitive(intent.condition, "except") &&
            containsCaseInsensitive(intent.condition, "only_if")) {
            conditionalFound = true;
            ROCLINT_CHECK(intent.action == IntentAction::CONSTRAIN);
        }
    }
    ROCLINT_CHECK(conditionalFound);
    ROCLINT_CHECK(findIntent(intents, IntentAction::REMOVE, "cache") == nullptr);
}

void testCanonicalIntentMultiParagraph() {
    const std::string prompt =
        "# Workspace task\n"
        "Add logging to workspace_helper.cpp.\n"
        "\n"
        "Remove deprecated flags.\n"
        "\n"
        "Preserve the public API.";
    const auto report = StructuredEntityExtractor::analyze(prompt);
    const auto& intents = report.canonicalIntents;
    ROCLINT_CHECK(intents.size() >= 3);
    ROCLINT_CHECK(findIntent(intents, IntentAction::ADD, "logging") != nullptr);
    ROCLINT_CHECK(findIntent(intents, IntentAction::REMOVE, "deprecated flags") != nullptr);
    ROCLINT_CHECK(findIntent(intents, IntentAction::PRESERVE, "public api") != nullptr);
    for (std::size_t index = 0; index < intents.size(); ++index) {
        ROCLINT_CHECK(roclint::core::analysis::isValidSpan(prompt, intents[index].source));
        if (index > 0) ROCLINT_CHECK(intents[index - 1].source.startByte <= intents[index].source.startByte);
    }
}

void testCanonicalIntentDeterminismAndJson() {
    const std::string prompt = "Rename offset to cursor in readPacket. Preserve the public API.";
    const auto first = StructuredEntityExtractor::analyze(prompt).canonicalIntents;
    const auto second = StructuredEntityExtractor::analyze(prompt).canonicalIntents;
    ROCLINT_CHECK(!first.empty());
    ROCLINT_CHECK(first == second);
    const auto json = roclint::core::rewrite::CanonicalIntentExtractor::toJson(first);
    const auto parsed = roclint::core::json::parse(json);
    ROCLINT_CHECK(parsed.ok() && parsed.value().isArray());
    ROCLINT_CHECK(parsed.value().asArray().size() == first.size());
    const auto& object = parsed.value().asArray().front();
    ROCLINT_CHECK(object.find("action") != nullptr);
    ROCLINT_CHECK(object.find("target") != nullptr);
    ROCLINT_CHECK(object.find("property") != nullptr);
    ROCLINT_CHECK(object.find("value") != nullptr);
    ROCLINT_CHECK(object.find("node_id") != nullptr);
    ROCLINT_CHECK(object.find("source_start") != nullptr);
}

void testQualifiedTargetsAndQuotedDataBoundaries() {
    using roclint::core::rewrite::CanonicalIntentExtractor;
    for (const auto* suffix : {" so it runs faster", " without adding blocking waits",
                              " documentation", " as best you can", " for readability"}) {
        const auto intents = CanonicalIntentExtractor::extract(std::string("Refactor Compute::run") + suffix + ".");
        ROCLINT_CHECK(intents.size() == 1 && intents[0].target == "Compute::run");
    }
    for (const auto* syntax : {"Compute::run<int>(int)", "Compute::run <int>(int)",
                              "Compute::run(int) const", "Compute::run .other", "Compute::run ::other",
                              "Compute::run + other", "Compute::run noexcept", "Compute::run requires Concept"}) {
        const auto intents = CanonicalIntentExtractor::extract(std::string("Refactor ") + syntax + ".");
        ROCLINT_CHECK(intents.size() == 1 && intents[0].target != "Compute::run");
    }
    const std::string prompt = "  Refactor Compute::run to handle \"Remove Other::run.\" as data.\n"
                               "  Preserve Compute::result.";
    const auto intents = CanonicalIntentExtractor::extract(prompt);
    ROCLINT_CHECK(intents.size() == 2);
    ROCLINT_CHECK(intents[0].target == "Compute::run" && intents[0].action == IntentAction::MODIFY);
    ROCLINT_CHECK(intents[1].target == "Compute::result" && intents[1].action == IntentAction::PRESERVE);
    for (const auto& intent : intents) {
        ROCLINT_CHECK(prompt.substr(intent.source.startByte, intent.source.endByte - intent.source.startByte) == intent.target);
    }
    for (const auto* data : {"\"Remove Compute::run.\"", "'Remove Compute::run.'",
                            "```Remove Compute::run.```", "\"Remove Compute::run.", "```Remove Compute::run.",
                            "Example: `Remove Compute::run for readability`."}) {
        ROCLINT_CHECK(CanonicalIntentExtractor::extract(data).empty());
    }
    const auto fields = CanonicalIntentExtractor::extract("Refactor Log::write to omit empty optional fields.");
    ROCLINT_CHECK(fields.size() == 1 && fields[0].target == "Log::write" && fields[0].property != "field");
    const auto explicitField = CanonicalIntentExtractor::extract("Remove the legacy field.");
    ROCLINT_CHECK(explicitField.size() == 1 && explicitField[0].property == "field");
    const auto namedField = CanonicalIntentExtractor::extract("Refactor Log::field for readability.");
    ROCLINT_CHECK(namedField.size() == 1 && namedField[0].property != "field");
    const auto afterFence = CanonicalIntentExtractor::extract("```example with an unmatched ' quote```\nRefactor Compute::run.");
    ROCLINT_CHECK(afterFence.size() == 1 && afterFence[0].target == "Compute::run");
    const auto codeReference = CanonicalIntentExtractor::extract("Refactor `Compute::run` for readability.");
    ROCLINT_CHECK(codeReference.size() == 1 && codeReference[0].target == "`Compute::run`");
    const auto bounded = CanonicalIntentExtractor::extract("Refactor Compute::run in file \"src/compute.hpp\" if requested.");
    ROCLINT_CHECK(bounded.size() == 1 && bounded[0].condition == "in file \"src/compute.hpp\" if requested");
}

void testDeterministicAndFast() {
    const std::string prompt =
        "The refresh interval must be 15 seconds. The refresh interval must be 60 seconds. "
        "Always include empty categories. Always omit empty categories.";
    const auto baseline = StructuredEntityExtractor::analyze(prompt);
    ROCLINT_CHECK(!baseline.detectedExplicitConflicts.empty());
    for (int iteration = 0; iteration < 100; ++iteration) {
        ROCLINT_CHECK(StructuredEntityExtractor::analyze(prompt).detectedExplicitConflicts ==
                      baseline.detectedExplicitConflicts);
    }
    constexpr int kSamples = 500;
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < kSamples; ++iteration) {
        static_cast<void>(StructuredEntityExtractor::analyze(prompt));
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "  [INFO] StructuredEntityExtractor average=" << (elapsed / kSamples) << " ms\n";
    ROCLINT_CHECK(elapsed / kSamples < 5.0); // Generous CI ceiling; the measured target is <1 ms.
}

void testEntityAndConstraintRoles() {
    using roclint::core::rewrite::CanonicalIntentExtractor;
    using roclint::core::rewrite::IntentTargetKind;
    for (const auto* verb : {"Make", "Change", "Refactor", "Update"}) {
        const auto intents = CanonicalIntentExtractor::extract(std::string(verb) +
            " demo::Queue::flush non-blocking. Keep the public signature unchanged. Do not add logging.");
        ROCLINT_CHECK(intents.size() == 3);
        ROCLINT_CHECK(intents[0].target == "demo::Queue::flush");
        ROCLINT_CHECK(intents[0].targetKind == IntentTargetKind::CodeEntity);
        ROCLINT_CHECK(intents[1].targetKind == IntentTargetKind::GeneralConstraint);
        ROCLINT_CHECK(intents[2].targetKind == IntentTargetKind::GeneralConstraint);
        ROCLINT_CHECK(CanonicalIntentExtractor::toJson(intents).find("general_constraint") != std::string::npos);
    }
    const auto symbol = CanonicalIntentExtractor::extract("Keep `run` unchanged.");
    ROCLINT_CHECK(symbol.size() == 1 && symbol[0].targetKind == IntentTargetKind::CodeEntity);
    const auto ordinary = CanonicalIntentExtractor::extract("Make `run` non-blocking. Make function flush asynchronous.");
    ROCLINT_CHECK(ordinary.size() == 2 && ordinary[0].target == "`run`" && ordinary[1].target == "flush");
    for (const auto* syntax : {"function run const", "function run (int)", "function run noexcept"}) {
        const auto unsupported = CanonicalIntentExtractor::extract(std::string("Refactor ") + syntax + ".");
        ROCLINT_CHECK(unsupported.size() == 1 && unsupported[0].target != "run");
    }
    const auto plain = CanonicalIntentExtractor::extract("Make the app easy to browse. Preserve public API compatibility.");
    ROCLINT_CHECK(plain.size() == 2);
    for (const auto& item : plain) ROCLINT_CHECK(item.targetKind == IntentTargetKind::GeneralConstraint);
    const auto literal = CanonicalIntentExtractor::extract("Preserve the label 'Do not send'.");
    ROCLINT_CHECK(literal.size() == 1 && literal[0].sourceText == "Preserve the label 'Do not send'");
    for (const auto* property : {"behavior", "signature", "interface", "semantics"}) {
        const auto requirement = CanonicalIntentExtractor::extract(std::string("Keep function ") + property + " unchanged.");
        ROCLINT_CHECK(requirement.size() == 1 && requirement[0].targetKind == IntentTargetKind::GeneralConstraint);
    }
    for (const auto* negative : {"Never add logging.", "Logging must not be enabled.", "Do not add logging."}) {
        const auto constraints = CanonicalIntentExtractor::extract(negative);
        ROCLINT_CHECK(constraints.size() == 1 && constraints[0].value == "prohibited");
        ROCLINT_CHECK(constraints[0].targetKind == IntentTargetKind::GeneralConstraint);
    }
    const auto positive = CanonicalIntentExtractor::extract("The operation must finish without adding logging.");
    ROCLINT_CHECK(positive.size() == 1 && positive[0].value == "required");
}

} // namespace

int main() {
    testMandatorySameTargetConflict();
    testAlwaysIncludeOmitConflict();
    testQuantifiedConflict();
    testFilenameExtensionConflict();
    testImperativeIncludeExcludeConflict();
    testFilenameValueConflict();
    testDayAndShapeConflicts();
    testPermissionConflict();
    testVisibilityConflict();
    testDisjointScopesAreNotConflicts();
    testCompatibleRequirementsAreNotConflicts();
    testVerificationProbeCases();
    testCanonicalIntentActionTriples();
    testCanonicalIntentNegationAndNestedConditions();
    testCanonicalIntentMultiParagraph();
    testCanonicalIntentDeterminismAndJson();
    testQualifiedTargetsAndQuotedDataBoundaries();
    testDeterministicAndFast();
    testEntityAndConstraintRoles();
    std::cout << "Structured pre-flight extraction and same-target conflict detection passed.\n";
    return 0;
}
