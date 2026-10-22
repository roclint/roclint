#include "roclint/review/ApprovalRecord.hpp"

#include <chrono>

namespace roclint::core::review {

namespace {

constexpr std::uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

inline std::uint64_t hashString(std::uint64_t h, std::string_view s) noexcept {
    for (unsigned char c : s) {
        h ^= static_cast<std::uint64_t>(c);
        h *= FNV_PRIME;
    }
    return h;
}

inline std::uint64_t hashU64(std::uint64_t h, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v & 0xFF);
        h *= FNV_PRIME;
        v >>= 8;
    }
    return h;
}

} // namespace

std::uint64_t ApprovalRecord::computeBindingHash() const noexcept {
    std::uint64_t h = FNV_OFFSET_BASIS;
    h = hashString(h, id);
    h = hashString(h, sessionId.value());
    h = hashString(h, revisionId.value());
    h = hashString(h, snapshotId);
    h = hashU64(h, snapshotHash);
    h = hashString(h, intentId);
    h = hashString(h, proposalId);
    h = hashU64(h, proposalHash);
    h = hashString(h, approvedText);
    h = hashU64(h, static_cast<std::uint64_t>(editGeneration));
    return h;
}

ApprovalRecord ApprovalFactory::createApproval(
    const SessionId& sessionId,
    const RevisionId& revisionId,
    std::string_view snapshotId,
    std::uint64_t snapshotHash,
    std::string_view intentId,
    std::string_view proposalId,
    std::uint64_t proposalHash,
    std::string_view approvedText,
    int editGeneration,
    std::int64_t nowMs) noexcept {

    if (nowMs == 0) {
        nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    ApprovalRecord record{
        .id = "appr_" + revisionId.value() + "_" + std::to_string(nowMs),
        .sessionId = sessionId,
        .revisionId = revisionId,
        .snapshotId = std::string(snapshotId),
        .snapshotHash = snapshotHash,
        .intentId = std::string(intentId),
        .proposalId = std::string(proposalId),
        .proposalHash = proposalHash,
        .approvedText = std::string(approvedText),
        .editGeneration = editGeneration,
        .createdAtMs = nowMs,
        .isCurrent = true
    };
    record.bindingHash = record.computeBindingHash();
    return record;
}

bool ApprovalFactory::isApprovalValid(
    const ApprovalRecord& approval,
    std::string_view currentRevisionId,
    std::string_view currentProposalId,
    std::uint64_t currentProposalHash) noexcept {

    if (!approval.isCurrent) {
        return false;
    }
    if (approval.revisionId.value() != currentRevisionId) {
        return false;
    }
    if (approval.proposalId != currentProposalId) {
        return false;
    }
    if (approval.proposalHash != currentProposalHash) {
        return false;
    }
    if (approval.bindingHash != approval.computeBindingHash()) {
        return false;
    }
    return true;
}

} // namespace roclint::core::review
