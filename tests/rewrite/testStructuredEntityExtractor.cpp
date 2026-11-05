#include "roclint/rewrite/StructuredEntityExtractor.hpp"
#include "roclint/analysis/SourceSpan.hpp"
#include "support/check.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

using roclint::core::rewrite::DetectedConflict;
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
    testDeterministicAndFast();
    std::cout << "Structured pre-flight extraction and same-target conflict detection passed.\n";
    return 0;
}
