#pragma once

#include "roclint/core/Result.hpp"
#include "roclint/core/prompt_state/PromptRevision.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace roclint::core {

enum class RevisionStatus {
    Clean,
    DraftDirty
};

[[nodiscard]] const char* toString(RevisionStatus status) noexcept;

class RevisionState {
public:
    static constexpr std::uint64_t kMaxStateVersion = 9007199254740991ULL; // Exact in QML/JSON and SQLite.
    // highestIssuedSequence is the largest revision sequence ever issued for the session; it must
    // come from storage on restore so that restoring an older revision never reissues an ID.
    explicit RevisionState(const PromptRevision& revision,
                           std::uint64_t highestIssuedSequence = 1,
                           std::uint64_t stateVersion = 1,
                           std::uint64_t draftEditGeneration = 0,
                           std::uint64_t proposalEditGeneration = 0);

    [[nodiscard]] static RevisionId makeRevisionId(const SessionId& sessionId, std::uint64_t sequence);

    [[nodiscard]] bool isStale(std::uint64_t expectedVersion) const noexcept;

    // Commits canonical (LF) text as a new immutable revision. Returns the new revision ID, or
    // StaleVersion / InvalidInput without mutating state.
    [[nodiscard]] Result<RevisionId> commitEdit(std::uint64_t expectedVersion,
                                                std::string_view text,
                                                std::int64_t createdAtMs);

    // Records that the live draft diverged from the committed revision.
    [[nodiscard]] Status noteDraftEdit(std::uint64_t expectedVersion);

    // Advances the authoritative state without issuing another text revision.
    [[nodiscard]] Status advanceState(std::uint64_t expectedVersion);

    // Records that a new rewrite proposal (or an edit to one) was produced.
    [[nodiscard]] Status noteProposalEdit(std::uint64_t expectedVersion);

    [[nodiscard]] std::uint64_t stateVersion() const noexcept { return stateVersion_; }
    [[nodiscard]] const PromptRevision& currentRevision() const noexcept { return currentRevision_; }
    [[nodiscard]] RevisionStatus status() const noexcept { return status_; }
    [[nodiscard]] bool isDraftDirty() const noexcept { return status_ == RevisionStatus::DraftDirty; }
    [[nodiscard]] std::uint64_t draftEditGeneration() const noexcept { return draftEditGeneration_; }
    [[nodiscard]] std::uint64_t proposalEditGeneration() const noexcept { return proposalEditGeneration_; }
    [[nodiscard]] std::uint64_t highestIssuedSequence() const noexcept { return highestIssuedSequence_; }

private:
    PromptRevision currentRevision_;
    std::uint64_t highestIssuedSequence_{1};
    std::uint64_t stateVersion_{1};
    std::uint64_t draftEditGeneration_{0};
    std::uint64_t proposalEditGeneration_{0};
    RevisionStatus status_{RevisionStatus::Clean};
};

} // namespace roclint::core
