#include "roclint/core/prompt_state/RevisionState.hpp"

#include "roclint/core/prompt_state/TextNormalization.hpp"

#include <exception>
#include <limits>

namespace roclint::core {

const char* toString(RevisionStatus status) noexcept {
    switch (status) {
        case RevisionStatus::Clean: return "CLEAN";
        case RevisionStatus::DraftDirty: return "DRAFT_DIRTY";
    }
    return "UNKNOWN";
}

RevisionState::RevisionState(const PromptRevision& revision,
                             std::uint64_t highestIssuedSequence,
                             std::uint64_t stateVersion,
                             std::uint64_t draftEditGeneration,
                             std::uint64_t proposalEditGeneration)
    : currentRevision_(revision),
      highestIssuedSequence_(highestIssuedSequence == 0 ? 1 : highestIssuedSequence),
      stateVersion_(stateVersion == 0 ? 1 : stateVersion),
      draftEditGeneration_(draftEditGeneration),
      proposalEditGeneration_(proposalEditGeneration) {}

RevisionId RevisionState::makeRevisionId(const SessionId& sessionId, std::uint64_t sequence) {
    return RevisionId("rev_" + sessionId.value() + "_" + std::to_string(sequence));
}

bool RevisionState::isStale(std::uint64_t expectedVersion) const noexcept {
    return expectedVersion != stateVersion_;
}

Result<RevisionId> RevisionState::commitEdit(std::uint64_t expectedVersion,
                                             std::string_view text,
                                             std::int64_t createdAtMs) {
    if (isStale(expectedVersion)) {
        return makeError(ErrorCode::StaleVersion,
                         "Expected state version " + std::to_string(expectedVersion) + " but current is " +
                             std::to_string(stateVersion_));
    }
    if (createdAtMs < 0) {
        return makeError(ErrorCode::InvalidInput, "createdAtMs cannot be negative");
    }
    if (stateVersion_ >= kMaxStateVersion ||
        highestIssuedSequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return makeError(ErrorCode::Internal, "State version or revision sequence exhausted");
    }

    const std::uint64_t nextSequence = highestIssuedSequence_ + 1;
    try {
        RevisionId newId = makeRevisionId(currentRevision_.sessionId(), nextSequence);
        PromptRevision next(currentRevision_.sessionId(), newId, currentRevision_.revisionId(),
                            normalizeLineEndings(text), createdAtMs);
        currentRevision_ = std::move(next);
        highestIssuedSequence_ = nextSequence;
        ++stateVersion_;
        status_ = RevisionStatus::Clean;
        return newId;
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::InvalidInput, ex.what());
    }
}

Status RevisionState::noteDraftEdit(std::uint64_t expectedVersion) {
    if (isStale(expectedVersion)) {
        return makeError(ErrorCode::StaleVersion,
                         "Draft edit observed state version " + std::to_string(expectedVersion) +
                             " but current is " + std::to_string(stateVersion_));
    }
    if (draftEditGeneration_ == std::numeric_limits<std::uint64_t>::max()) return makeError(ErrorCode::Internal, "Draft generation exhausted");
    if (auto status = advanceState(expectedVersion); !status.ok()) return status;
    status_ = RevisionStatus::DraftDirty;
    ++draftEditGeneration_;
    return Status::success();
}

Status RevisionState::advanceState(std::uint64_t expectedVersion) {
    if (isStale(expectedVersion)) return makeError(ErrorCode::StaleVersion, "The command observed an earlier state version");
    if (stateVersion_ >= kMaxStateVersion) return makeError(ErrorCode::Internal, "State version exhausted");
    ++stateVersion_;
    return Status::success();
}

Status RevisionState::noteProposalEdit(std::uint64_t expectedVersion) {
    if (proposalEditGeneration_ == std::numeric_limits<std::uint64_t>::max()) return makeError(ErrorCode::Internal, "Proposal generation exhausted");
    if (auto status = advanceState(expectedVersion); !status.ok()) return status;
    ++proposalEditGeneration_;
    return Status::success();
}

} // namespace roclint::core
