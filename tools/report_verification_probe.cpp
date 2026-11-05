#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/rewrite/StructuredEntityExtractor.hpp"
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace roclint;
    const std::vector<std::pair<std::string, std::string>> cases{
        {"baseline_conflict", "The refresh interval must be 15 seconds. The refresh interval must be 60 seconds."},
        {"equivalent_units", "The timeout must be 60 seconds. The timeout must be 1 minute."},
        {"independent_dimensions", "The panel must be 10 pixels wide. The panel must be 20 pixels high."},
        {"disjoint_mandatory_scope", "The sound must be enabled for critical alerts. The sound must be disabled for informational alerts."},
        {"equivalent_permissions", "Every employee may approve. All employees may approve."},
        {"quoted_data", "Print these examples verbatim: \"The interval must be 15 seconds. The interval must be 60 seconds.\""},
        {"same_scope_opposite_actions", "Expand on desktop. Collapse on desktop."},
        {"nonadjacent_paragraphs", "The refresh interval must be 15 seconds.\n\nWrite a short manual.\n\nThe refresh interval must be 60 seconds."},
        {"disjoint_imperative_scope", "Enable sound for critical alerts. Disable sound for informational alerts."}
    };
    for (const auto& [id, prompt] : cases) {
        bridge::GraniteAdapter adapter;
        if (!adapter.beginLoading() || !adapter.completeLoading({"granite", "sha256:abc", "Q8_0", bridge::kLoopbackOllamaTransport})) return 1;
        int calls = 0;
        const auto started = std::chrono::steady_clock::now();
        const auto result = adapter.discoverOptions({"sess_probe", "rev_probe_1", prompt, "UNKNOWN"},
            [&](std::string_view) -> core::Result<std::string> {
                ++calls;
                return std::string(R"({"status":"no_ambiguity","assistance_kind":"none"})");
            });
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        std::cout << id << "|ok=" << result.ok() << "|transport_calls=" << calls;
        if (result.ok()) std::cout << "|conflicts=" << result.value().analysis.conflicts.size()
            << "|sealed=" << core::analysis::isSnapshotSealed(result.value().analysis);
        std::cout << "|ms=" << ms << '\n';
    }
}
