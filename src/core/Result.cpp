#include "roclint/core/Result.hpp"

namespace roclint::core {

const char* toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::StaleVersion: return "STALE_VERSION";
        case ErrorCode::InvalidInput: return "INVALID_INPUT";
        case ErrorCode::DbError: return "DB_ERROR";
        case ErrorCode::NotFound: return "NOT_FOUND";
        case ErrorCode::Conflict: return "CONFLICT";
        case ErrorCode::ProviderUnavailable: return "PROVIDER_UNAVAILABLE";
        case ErrorCode::ProviderFailed: return "PROVIDER_FAILED";
        case ErrorCode::ValidationFailed: return "VALIDATION_FAILED";
        case ErrorCode::PreconditionFailed: return "PRECONDITION_FAILED";
        case ErrorCode::Internal: return "INTERNAL";
    }
    return "UNKNOWN";
}

} // namespace roclint::core
