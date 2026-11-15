#include "roclint/retrieval/RetrievalTypes.hpp"

namespace roclint::retrieval {

const char* toString(SourceKind kind) noexcept {
    switch (kind) {
        case SourceKind::WorkspaceCode:     return "workspace_code";
        case SourceKind::WorkspaceDoc:      return "workspace_doc";
        case SourceKind::ImportedReference: return "imported_reference";
        case SourceKind::UserPreference:    return "user_preference";
        case SourceKind::WorkspaceDecision: return "workspace_decision";
    }
    return "unknown";
}

bool operator==(const IndexPolicy& a, const IndexPolicy& b) noexcept {
    return a.modelId == b.modelId &&
           a.modelRevision == b.modelRevision &&
           a.dimensions == b.dimensions &&
           a.normalized == b.normalized &&
           a.chunkerVersion == b.chunkerVersion &&
           a.preprocessVersion == b.preprocessVersion;
}

const char* toString(RetrievalStatus status) noexcept {
    switch (status) {
        case RetrievalStatus::Ready:            return "READY";
        case RetrievalStatus::Indexing:         return "INDEXING";
        case RetrievalStatus::Partial:          return "PARTIAL";
        case RetrievalStatus::NoRelevantContext: return "NO_RELEVANT_CONTEXT";
        case RetrievalStatus::Unavailable:      return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}

const char* toString(EmbeddingRuntimeState state) noexcept {
    switch (state) {
        case EmbeddingRuntimeState::Unloaded:   return "UNLOADED";
        case EmbeddingRuntimeState::Loading:    return "LOADING";
        case EmbeddingRuntimeState::Ready:      return "READY";
        case EmbeddingRuntimeState::Busy:       return "BUSY";
        case EmbeddingRuntimeState::Failed:     return "FAILED";
        case EmbeddingRuntimeState::Unavailable: return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}

} // namespace roclint::retrieval
