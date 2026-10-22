#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::bridge {

//Runtime lifecycle states for local IBM Granite SLM 
enum class GraniteRuntimeState {
    Unloaded,
    Loading,
    Ready,
    Busy,
    Failed
};

[[nodiscard]] constexpr std::string_view toString(GraniteRuntimeState state) noexcept {
    switch (state) {
        case GraniteRuntimeState::Unloaded: return "UNLOADED";
        case GraniteRuntimeState::Loading:  return "LOADING";
        case GraniteRuntimeState::Ready:    return "READY";
        case GraniteRuntimeState::Busy:     return "BUSY";
        case GraniteRuntimeState::Failed:   return "FAILED";
    }
    return "UNKNOWN";
}

//typed request payload for granite_discover_options (Contract C04).
struct GraniteOptionDiscoveryRequest {
    std::string sessionId{};
    std::string revisionId{};
    std::string promptString{};
    std::string s1Category{"action"};          // "action", "target", "rule_conflict"
    std::string hostContextCategory{"UNKNOWN"};// Optional coarse category: CODE_EDITOR, TERMINAL, etc.
};

//Structured result from granite_discover_options.
struct GraniteOptionDiscoveryResponse {
    bool success{false};
    std::string errorMessage{};
    std::optional<core::analysis::ChoiceGroup> choiceGroup{std::nullopt};
    std::string modelId{"ibm-granite/granite-4.2-3b-GGUF"};
    std::string quantization{"Q8_0"};
    double latencyMs{0.0};
};

//Serializes a discovery request to standard v1 JSON.
[[nodiscard]] std::string serializeDiscoveryRequest(const GraniteOptionDiscoveryRequest& req);

// Parses a model JSON response into bounded typed C++ structures (Contract C03/C04).
[[nodiscard]] GraniteOptionDiscoveryResponse parseDiscoveryResponse(std::string_view jsonResponse, std::string_view baseRevisionId);

//Local Granite Analysis Adapter managing local model option discovery and lifecycle.
class GraniteAdapter {
public:
    using MockInferenceHandler = std::function<std::string(const GraniteOptionDiscoveryRequest&)>;

    GraniteAdapter();
    explicit GraniteAdapter(std::string modelPath, std::string quantization = "Q8_0");

    [[nodiscard]] GraniteRuntimeState state() const noexcept { return state_; }
    [[nodiscard]] const std::string& modelPath() const noexcept { return modelPath_; }
    [[nodiscard]] const std::string& quantization() const noexcept { return quantization_; }

    void setState(GraniteRuntimeState newState) noexcept { state_ = newState; }

    // Configures a mock handler for deterministic unit tests and offline testing.
    void setMockHandler(MockInferenceHandler handler) {
        mockHandler_ = std::move(handler);
    }

    // Dispatches option discovery against the local Granite runtime (Contract C04).
    [[nodiscard]] GraniteOptionDiscoveryResponse discoverOptions(const GraniteOptionDiscoveryRequest& req);

private:
    GraniteRuntimeState state_{GraniteRuntimeState::Ready};
    std::string modelPath_{"ibm-granite/granite-4.2-3b-GGUF"};
    std::string quantization_{"Q8_0"};
    MockInferenceHandler mockHandler_{nullptr};
};

} 
