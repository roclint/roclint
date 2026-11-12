#pragma once

#include "roclint/core/Json.hpp"
#include "roclint/core/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core {

// Who originated the requirement?
enum class Origin : std::uint8_t {
    User,
    Domain,
    Environment,
    Model
};

// How was the requirement derived?
enum class Derivation : std::uint8_t {
    Direct,
    Paraphrased,
    Entailed,
    Assumed
};

// What authority does the requirement have?
enum class Enforcement : std::uint8_t {
    Hard,
    Conditional,
    Advisory
};

// Is something required or forbidden?
enum class Polarity : std::uint8_t {
    Require,
    Prohibit
};

// Has independent verification established
// that the cited evidence supports the claim?
enum class EvidenceStatus : std::uint8_t {
    Unverified,
    Supported,
    Rejected
};

// Byte offsets into an immutable source document.
// endByte is exclusive.
struct SourceSpan {
    std::string sourceId;
    std::size_t beginByte = 0;
    std::size_t endByte = 0;

    bool operator==(const SourceSpan&) const = default;
};

struct ScopedConstraint {
    std::string id;
    std::string rule;

    // Safe defaults: assumptions carry no veto power.
    Origin origin = Origin::Model;
    Derivation derivation = Derivation::Assumed;
    Enforcement enforcement = Enforcement::Advisory;

    Polarity polarity = Polarity::Require;
    EvidenceStatus evidenceStatus = EvidenceStatus::Unverified;

    // E.g. "presentation", "routing", "security".
    // Descriptive only; not proof of orthogonality.
    std::string aspect;

    std::vector<SourceSpan> evidence;

    // Registered predicate identifier, not executable
    // code or arbitrary LLM-generated instructions.
    std::optional<std::string> appliesWhen;

    // Reference to RFC, specification, tool evidence.
    std::optional<std::string> authorityRef;

    bool operator==(const ScopedConstraint&) const = default;
};

[[nodiscard]] const char* toString(Origin origin) noexcept;
[[nodiscard]] const char* toString(Derivation derivation) noexcept;
[[nodiscard]] const char* toString(Enforcement enforcement) noexcept;
[[nodiscard]] const char* toString(Polarity polarity) noexcept;
[[nodiscard]] const char* toString(EvidenceStatus status) noexcept;

[[nodiscard]] Result<Origin> parseOrigin(std::string_view text) noexcept;
[[nodiscard]] Result<Derivation> parseDerivation(std::string_view text) noexcept;
[[nodiscard]] Result<Enforcement> parseEnforcement(std::string_view text) noexcept;
[[nodiscard]] Result<Polarity> parsePolarity(std::string_view text) noexcept;
[[nodiscard]] Result<EvidenceStatus> parseEvidenceStatus(std::string_view text) noexcept;

[[nodiscard]] json::Value toJson(const SourceSpan& span);
[[nodiscard]] Result<SourceSpan> sourceSpanFromJson(const json::Value& val);

[[nodiscard]] json::Value toJson(const ScopedConstraint& constraint);
[[nodiscard]] Result<ScopedConstraint> scopedConstraintFromJson(const json::Value& val);

// Promotion safeguard: Model candidates cannot acquire Hard enforcement or User origin
// unless independent verification marks evidence as Supported.
[[nodiscard]] bool canPromoteToEnforceable(const ScopedConstraint& candidate) noexcept;

} // namespace roclint::core
