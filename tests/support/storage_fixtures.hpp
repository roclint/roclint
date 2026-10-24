#pragma once

#include "support/fixtures.hpp"
#include "roclint/storage/PromptRepository.hpp"

namespace fx {
inline void recordSnapshotAttempts(roclint::storage::PromptRepository& repository, const an::AnalysisSnapshot& snapshot) {
    using namespace roclint::storage;
    const auto existing = repository.getModelAttempts(roclint::core::SessionId(snapshot.normalized.sessionId)).value();
    auto record = [&](const std::string& id, const std::string& stage, const std::string& model) {
        if (id.empty()) return;
        for (const auto& attempt : existing) if (attempt.attemptId == id) return;
        ModelAttemptRecord attempt{id, snapshot.normalized.sessionId, snapshot.normalized.baseRevisionId, stage};
        attempt.transport = "test_fixture";
        attempt.modelId = model;
        ROCLINT_CHECK(repository.beginModelAttempt(attempt).ok());
        ROCLINT_CHECK(repository.completeModelAttempt(id, AttemptStatus::Succeeded, model, "fixture", "", 1).ok());
    };
    record(snapshot.discoveryAttemptId, "discovery", "fixture_discovery_model");
    record(snapshot.scorerAttemptId, snapshot.evaluation.executionStatus == an::ExecutionStatus::ExecutedMock ? "test_mock_scoring" :
           snapshot.evaluation.engine == an::ScorerEngine::JuliaLocal ? "julia_scoring" : "jev_scoring", snapshot.evaluation.modelVersion);
}

} // namespace fx
