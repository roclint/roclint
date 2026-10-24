#include "roclint/core/crypto/Sha256.hpp"

#include <cstring>
#include <fstream>
#include <vector>

namespace roclint::core::crypto {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32U - n));
}

} // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::transform(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, std::size_t length) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    bitLength_ += static_cast<std::uint64_t>(length) * 8U;
    for (std::size_t i = 0; i < length; ++i) {
        buffer_[bufferLength_++] = bytes[i];
        if (bufferLength_ == buffer_.size()) {
            transform(buffer_.data());
            bufferLength_ = 0;
        }
    }
}

std::array<std::uint8_t, 32> Sha256::finalize() noexcept {
    const std::uint64_t totalBits = bitLength_;
    buffer_[bufferLength_++] = 0x80;
    if (bufferLength_ > 56) {
        while (bufferLength_ < 64) {
            buffer_[bufferLength_++] = 0;
        }
        transform(buffer_.data());
        bufferLength_ = 0;
    }
    while (bufferLength_ < 56) {
        buffer_[bufferLength_++] = 0;
    }
    for (int i = 7; i >= 0; --i) {
        buffer_[bufferLength_++] = static_cast<std::uint8_t>((totalBits >> (static_cast<unsigned>(i) * 8U)) & 0xFFU);
    }
    transform(buffer_.data());

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFU);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFU);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFU);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFU);
    }
    return digest;
}

std::string Sha256::finalizeHex() {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto digest = finalize();
    std::string out;
    out.reserve(64);
    for (const auto byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

std::string Sha256::hexDigest(std::string_view data) {
    Sha256 sha;
    sha.update(data);
    return sha.finalizeHex();
}

CanonicalHasher::CanonicalHasher(std::string_view domain) {
    field(domain);
}

CanonicalHasher& CanonicalHasher::field(std::string_view value) {
    const std::string prefix = std::to_string(value.size()) + ":";
    sha_.update(prefix);
    sha_.update(value);
    sha_.update(";", 1);
    return *this;
}

CanonicalHasher& CanonicalHasher::number(std::uint64_t value) {
    const std::string text = std::to_string(value);
    return field(std::string_view(text));
}

CanonicalHasher& CanonicalHasher::flag(bool value) {
    return field(std::string_view(value ? "1" : "0"));
}

std::string CanonicalHasher::finalizeHex() {
    return sha_.finalizeHex();
}

Result<std::string> normalizeSha256Digest(std::string_view digest) {
    constexpr std::string_view kPrefix = "sha256:";
    if (digest.size() > kPrefix.size() && digest.substr(0, kPrefix.size()) == kPrefix) {
        digest.remove_prefix(kPrefix.size());
    }
    if (digest.size() != 64) {
        return makeError(ErrorCode::InvalidInput, "SHA-256 digest must be 64 hex characters");
    }
    std::string out;
    out.reserve(64);
    for (const char c : digest) {
        if (c >= '0' && c <= '9') {
            out.push_back(c);
        } else if (c >= 'a' && c <= 'f') {
            out.push_back(c);
        } else if (c >= 'A' && c <= 'F') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            return makeError(ErrorCode::InvalidInput, "SHA-256 digest contains a non-hex character");
        }
    }
    return out;
}

Result<std::string> sha256File(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        const auto u8 = path.u8string();
        return makeError(ErrorCode::NotFound,
                         "Cannot open file for hashing: " + std::string(u8.begin(), u8.end()));
    }
    Sha256 sha;
    std::vector<char> buffer(1 << 16);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = in.gcount();
        if (got > 0) {
            sha.update(buffer.data(), static_cast<std::size_t>(got));
        }
    }
    if (in.bad()) {
        return makeError(ErrorCode::Internal, "Read error while hashing file");
    }
    return sha.finalizeHex();
}

Status verifyFileSha256(const std::filesystem::path& path, std::string_view expectedDigest) {
    auto expected = normalizeSha256Digest(expectedDigest);
    if (!expected.ok()) return expected.error();
    auto actual = sha256File(path);
    if (!actual.ok()) return actual.error();
    if (actual.value() != expected.value()) {
        return makeError(ErrorCode::ValidationFailed,
                         "SHA-256 mismatch: expected " + expected.value() + ", got " + actual.value());
    }
    return Status::success();
}

} // namespace roclint::core::crypto
