#pragma once
#include "roclint/analysis/PromptTriage.hpp"
#include "roclint/analysis/ReviewPolicy.hpp"

#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/core/Result.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::bridge {

// Narrow, deterministic guard for grounded options that reverse an unquoted explicit color
// requirement. This is not a general natural-language or rewrite verifier.
[[nodiscard]] std::optional<std::string> explicitColorConstraintViolation(
    std::string_view frozenPrompt, const core::analysis::ChoiceAlternative& alternative,
    std::string_view question = {});

// Runtime lifecycle states for the local Granite model (C04).
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

inline constexpr const char* kLoopbackOllamaTransport = "loopback_http_ollama";
inline constexpr std::size_t kMaxDiscoveryResponseBytes = 64 * 1024;
inline constexpr std::size_t kMaxModelStringBytes = 4 * 1024;
inline constexpr std::size_t kMaxDiscoveryGroups = 3;
inline constexpr std::size_t kMaxDiscoveryItemsPerKind = 16;

// Identity of the loaded model as reported by the runtime, never assumed.
struct GraniteModelInfo {
    std::string modelId{};
    std::string digest{};
    std::string quantization{};
    std::string transport{kLoopbackOllamaTransport};
};

struct GraniteOptionDiscoveryRequest {
    std::string sessionId{};
    std::string revisionId{};
    std::string promptString{};               // Frozen revision text (canonical LF)
    std::string hostContextCategory{"UNKNOWN"};
    std::optional<core::analysis::PromptTriage> promptTriage{};
    std::vector<std::string> workspaceFiles{};
    std::vector<std::string> workspaceSymbols{};
    bool isNonTechnicalPrompt{false};
    core::analysis::ReviewTrigger reviewTrigger{core::analysis::ReviewTrigger::Automatic};
};

enum class DiscoveryOutcome {
    OptionsGenerated,
    NoAmbiguity,
    NeedsClarification
};

[[nodiscard]] constexpr std::string_view toString(DiscoveryOutcome outcome) noexcept {
    switch (outcome) {
        case DiscoveryOutcome::OptionsGenerated: return "options_generated";
        case DiscoveryOutcome::NoAmbiguity: return "no_ambiguity";
        case DiscoveryOutcome::NeedsClarification: return "needs_clarification";
    }
    return "unknown";
}

struct GraniteOptionDiscoveryResponse {
    DiscoveryOutcome outcome{DiscoveryOutcome::NoAmbiguity};
    core::analysis::NormalizedAnalysis analysis{}; // Sealed and bound to the request's revision
    std::size_t ungroundedEvidenceCount{0};
    GraniteModelInfo model{};
    double latencyMs{0.0};
    core::analysis::AssistanceKind assistanceKind{core::analysis::AssistanceKind::None};
};

// Builds the instruction text sent to the model. User text is embedded as escaped JSON only.
[[nodiscard]] std::string buildDiscoveryPrompt(const GraniteOptionDiscoveryRequest& req);

// JSON Schema for the discovery reply, used by runtimes that support schema-constrained output.
// The reply is still strictly validated by parseDiscoveryResponse.
[[nodiscard]] const std::string& discoveryResponseSchema();
// Restricts evidence quotations to bounded, unique literal excerpts of this frozen prompt.
// This proves quotation provenance only, not semantic relevance of model suggestions.
[[nodiscard]] std::string discoveryResponseSchema(std::string_view frozenPromptText);

// Strictly parses a model response (C03/C04). Provider IDs are ignored; IDs are host-minted.
// Evidence excerpts are resolved against frozenPromptText; unresolvable excerpts are dropped so
// the materiality policy rejects the item as ungrounded.
[[nodiscard]] core::Result<GraniteOptionDiscoveryResponse> parseDiscoveryResponse(std::string_view jsonResponse,
                                                                                  std::string_view sessionId,
                                                                                  std::string_view baseRevisionId,
                                                                                  std::string_view frozenPromptText);

// Blocking model call: receives the compiled instruction text, returns the raw model output.
// Must be invoked off the GUI thread.
using GraniteTransport = std::function<core::Result<std::string>(std::string_view instruction)>;

class GraniteAdapter {
public:
    class BusyLease {
    public:
        BusyLease(BusyLease&& other) noexcept;
        BusyLease& operator=(BusyLease&&) = delete;
        BusyLease(const BusyLease&) = delete;
        BusyLease& operator=(const BusyLease&) = delete;
        ~BusyLease();

        // Marks the runtime FAILED when the lease is released.
        void markFailed(std::string reason);

    private:
        friend class GraniteAdapter;
        explicit BusyLease(GraniteAdapter* owner) noexcept : owner_(owner) {}
        GraniteAdapter* owner_{nullptr};
        bool failed_{false};
        std::string reason_{};
    };

    GraniteAdapter() = default;

    [[nodiscard]] GraniteRuntimeState state() const noexcept { return state_.load(); }
    [[nodiscard]] GraniteModelInfo modelInfo() const;
    [[nodiscard]] std::string lastError() const;

    // Unloaded|Failed -> Loading. Returns false from any other state.
    [[nodiscard]] bool beginLoading();
    // Loading -> Ready.
    [[nodiscard]] bool completeLoading(GraniteModelInfo info);
    // Loading -> Failed.
    void failLoading(std::string reason);
    // Loading -> Unloaded after the owning runtime acknowledges cancellation/resource release.
    // A stale loader completion still requires rejection by its generation-bound supervisor.
    [[nodiscard]] bool cancelLoading();
    // Failed -> Unloaded, allowing a fresh load attempt.
    [[nodiscard]] bool recover();
    // Ready|Failed -> Unloaded (model switch). Rejected while Loading or Busy.
    [[nodiscard]] bool unload();

    // Ready -> Busy. Rejected (nullopt) while Unloaded, Loading, Busy or Failed.
    [[nodiscard]] std::optional<BusyLease> tryAcquire();

    // Runs one discovery call: acquires the runtime, calls the transport, parses the response.
    // Provider failures mark FAILED. Cancellation, bounded input/output/deadline rejection
    // and malformed model output fail the call without poisoning a healthy loaded runtime.
    [[nodiscard]] core::Result<GraniteOptionDiscoveryResponse> discoverOptions(const GraniteOptionDiscoveryRequest& req,
                                                                               const GraniteTransport& transport);

private:
    void release(bool failed, std::string reason);

    std::atomic<GraniteRuntimeState> state_{GraniteRuntimeState::Unloaded};
    mutable std::mutex infoMutex_{};
    GraniteModelInfo info_{};
    std::string lastError_{};
};

} // namespace roclint::bridge
