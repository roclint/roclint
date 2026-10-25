#pragma once

#include "roclint/core/Json.hpp"
#include "roclint/core/Result.hpp"
#include "roclint/core/crypto/Sha256.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace roclint::core::schema {

inline void hashDouble(crypto::CanonicalHasher& hash, double value) noexcept {
    hash.number(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
}

// ============================================================================
// HashVisitor: feeds struct members into CanonicalHasher in strict order
// ============================================================================
class HashVisitor {
public:
    explicit HashVisitor(crypto::CanonicalHasher& hasher) noexcept : hasher_(hasher) {}

    template <typename T>
    void operator()(std::string_view /*name*/, const T& value) {
        visit(value);
    }

private:
    void visit(std::string_view s) { hasher_.field(s); }
    void visit(const std::string& s) { hasher_.field(s); }
    void visit(bool b) { hasher_.flag(b); }
    void visit(double d) { hashDouble(hasher_, d); }

    template <std::integral I>
    void visit(I n) { hasher_.number(static_cast<std::uint64_t>(n)); }

    template <typename E>
    requires std::is_enum_v<E>
    void visit(E e) { hasher_.number(static_cast<std::uint64_t>(e)); }

    template <typename T, std::size_t N>
    void visit(const std::array<T, N>& arr) {
        for (const auto& item : arr) visit(item);
    }

    template <typename T>
    void visit(const std::vector<T>& vec) {
        hasher_.number(vec.size());
        for (const auto& item : vec) visit(item);
    }

    template <typename T>
    void visit(const std::optional<T>& opt) {
        hasher_.flag(opt.has_value());
        if (opt.has_value()) visit(*opt);
    }

    crypto::CanonicalHasher& hasher_;
};

// ============================================================================
// JsonEncodeVisitor: serializes struct members into roclint::core::json::Value
// ============================================================================
class JsonEncodeVisitor {
public:
    explicit JsonEncodeVisitor(json::Value& root) noexcept : root_(root) {}

    template <typename T>
    void operator()(std::string_view name, const T& value) {
        root_.set(std::string(name), encode(value));
    }

private:
    static json::Value encode(std::string_view s) { return json::Value::string(std::string(s)); }
    static json::Value encode(const std::string& s) { return json::Value::string(s); }
    static json::Value encode(bool b) { return json::Value::boolean(b); }
    static json::Value encode(double d) { return json::Value::number(d); }

    template <std::integral I>
    static json::Value encode(I n) { return json::Value::number(static_cast<double>(n)); }

    template <typename E>
    requires std::is_enum_v<E>
    static json::Value encode(E e) { return json::Value::number(static_cast<double>(e)); }

    template <typename T, std::size_t N>
    static json::Value encode(const std::array<T, N>& arr) {
        auto out = json::Value::array();
        for (const auto& item : arr) out.push(encode(item));
        return out;
    }

    template <typename T>
    static json::Value encode(const std::vector<T>& vec) {
        auto out = json::Value::array();
        for (const auto& item : vec) out.push(encode(item));
        return out;
    }

    template <typename T>
    static json::Value encode(const std::optional<T>& opt) {
        if (!opt.has_value()) return json::Value::null();
        return encode(*opt);
    }

    json::Value& root_;
};

// ============================================================================
// JsonDecodeVisitor: populates struct members from roclint::core::json::Value
// ============================================================================
class JsonDecodeVisitor {
public:
    explicit JsonDecodeVisitor(const json::Value& root) noexcept : root_(root) {}

    template <typename T>
    void operator()(std::string_view name, T& target) {
        if (!status_.ok()) return;
        const auto* val = root_.find(name);
        if (!val) {
            // Optionals are allowed to be absent
            if constexpr (is_optional_v<T>) {
                target = std::nullopt;
                return;
            }
            status_ = makeError(ErrorCode::InvalidInput, "Missing required field: " + std::string(name));
            return;
        }
        decodeField(*val, name, target);
    }

    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    template <typename T>
    static constexpr bool is_optional_v = false;
    template <typename T>
    static constexpr bool is_optional_v<std::optional<T>> = true;

    void decodeField(const json::Value& val, std::string_view name, std::string& target) {
        if (!val.isString()) {
            status_ = makeError(ErrorCode::InvalidInput, "Expected string for field: " + std::string(name));
            return;
        }
        target = val.asString();
    }

    void decodeField(const json::Value& val, std::string_view name, bool& target) {
        if (!val.isBool()) {
            status_ = makeError(ErrorCode::InvalidInput, "Expected boolean for field: " + std::string(name));
            return;
        }
        target = val.asBool();
    }

    void decodeField(const json::Value& val, std::string_view name, double& target) {
        if (!val.isNumber()) {
            status_ = makeError(ErrorCode::InvalidInput, "Expected number for field: " + std::string(name));
            return;
        }
        target = val.asNumber();
    }

    template <std::integral I>
    void decodeField(const json::Value& val, std::string_view name, I& target) {
        if (!val.isNumber()) {
            status_ = makeError(ErrorCode::InvalidInput, "Expected integer for field: " + std::string(name));
            return;
        }
        const double d = val.asNumber();
        if (d < 0 || d > 9007199254740991.0 || std::floor(d) != d) {
            status_ = makeError(ErrorCode::InvalidInput, "Invalid integer bounds for field: " + std::string(name));
            return;
        }
        target = static_cast<I>(d);
    }

    template <typename E>
    requires std::is_enum_v<E>
    void decodeField(const json::Value& val, std::string_view name, E& target) {
        if (!val.isNumber()) {
            status_ = makeError(ErrorCode::InvalidInput, "Expected enum integer for field: " + std::string(name));
            return;
        }
        target = static_cast<E>(static_cast<std::uint64_t>(val.asNumber()));
    }

    template <typename T, std::size_t N>
    void decodeField(const json::Value& val, std::string_view name, std::array<T, N>& target) {
        if (!val.isArray()) {
            status_ = makeError(ErrorCode::InvalidInput, "Expected array for field: " + std::string(name));
            return;
        }
        const auto& arr = val.asArray();
        if (arr.size() != N) {
            status_ = makeError(ErrorCode::InvalidInput, "Array size mismatch for field: " + std::string(name));
            return;
        }
        for (std::size_t i = 0; i < N && status_.ok(); ++i) {
            decodeField(arr[i], name, target[i]);
        }
    }

    template <typename T>
    void decodeField(const json::Value& val, std::string_view name, std::vector<T>& target) {
        if (!val.isArray()) {
            status_ = makeError(ErrorCode::InvalidInput, "Expected array for field: " + std::string(name));
            return;
        }
        target.clear();
        for (const auto& item : val.asArray()) {
            T decoded{};
            decodeField(item, name, decoded);
            if (!status_.ok()) return;
            target.push_back(std::move(decoded));
        }
    }

    template <typename T>
    void decodeField(const json::Value& val, std::string_view name, std::optional<T>& target) {
        if (val.isNull()) {
            target = std::nullopt;
            return;
        }
        T decoded{};
        decodeField(val, name, decoded);
        if (status_.ok()) target = std::move(decoded);
    }

    const json::Value& root_;
    Status status_{Status::success()};
};

} // namespace roclint::core::schema
