#pragma once

#include "roclint/core/Result.hpp"

#include <array>
#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace roclint::core::crypto {

class Sha256 {
public:
    Sha256() noexcept;

    void update(const void* data, std::size_t length) noexcept;
    void update(std::string_view data) noexcept { update(data.data(), data.size()); }

    [[nodiscard]] std::array<std::uint8_t, 32> finalize() noexcept;
    [[nodiscard]] std::string finalizeHex();

    [[nodiscard]] static std::string hexDigest(std::string_view data);

private:
    void transform(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t bitLength_{0};
    std::size_t bufferLength_{0};
};

// Collision-resistant canonical encoder: every field is fed as "<byte length>:<bytes>;" so that
// distinct field sequences can never produce the same byte stream.
class CanonicalHasher {
public:
    explicit CanonicalHasher(std::string_view domain);

    CanonicalHasher& field(std::string_view value);
    CanonicalHasher& number(std::uint64_t value);
    CanonicalHasher& flag(bool value);

    [[nodiscard]] std::string finalizeHex();

private:
    Sha256 sha_;
};

// Streams a file through SHA-256 and returns the lowercase hex digest.
[[nodiscard]] Result<std::string> sha256File(const std::filesystem::path& path);

// Accepts "<64 hex>" or "sha256:<64 hex>" (case-insensitive). Fails with ValidationFailed on a
// mismatch and InvalidInput on a malformed expectation.
[[nodiscard]] Status verifyFileSha256(const std::filesystem::path& path, std::string_view expectedDigest);

// Normalizes "sha256:<hex>" / "<hex>" to lowercase bare hex; InvalidInput when malformed.
[[nodiscard]] Result<std::string> normalizeSha256Digest(std::string_view digest);

} // namespace roclint::core::crypto
