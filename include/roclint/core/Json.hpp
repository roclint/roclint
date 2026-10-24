#pragma once

#include "roclint/core/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace roclint::core::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Member = std::pair<std::string, Value>;
    using Object = std::vector<Member>;

    Value() = default;

    [[nodiscard]] static Value null() { return Value{}; }
    [[nodiscard]] static Value boolean(bool b);
    [[nodiscard]] static Value number(double n);
    [[nodiscard]] static Value string(std::string s);
    [[nodiscard]] static Value array();
    [[nodiscard]] static Value object();

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }

    [[nodiscard]] bool asBool() const noexcept { return bool_; }
    [[nodiscard]] double asNumber() const noexcept { return number_; }
    [[nodiscard]] const std::string& asString() const noexcept { return string_; }
    [[nodiscard]] const Array& asArray() const noexcept { return array_; }
    [[nodiscard]] const Object& asObject() const noexcept { return object_; }

    // Returns nullptr when this is not an object or the key is absent.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;

    Value& set(std::string key, Value value);
    Value& push(Value value);

private:
    Type type_{Type::Null};
    bool bool_{false};
    double number_{0.0};
    std::string string_{};
    Array array_{};
    Object object_{};
};

struct ParseLimits {
    std::size_t maxBytes{64 * 1024};
    std::size_t maxDepth{32};
};

// Strict RFC 8259 parser: rejects truncation, trailing content, duplicate keys, invalid UTF-8,
// unescaped control characters, and inputs exceeding the configured limits. \uXXXX escapes,
// including surrogate pairs, are decoded to UTF-8.
[[nodiscard]] Result<Value> parse(std::string_view text, ParseLimits limits = {});

[[nodiscard]] std::string serialize(const Value& value);

// Typed accessors used by schema readers.
[[nodiscard]] Result<std::string> requireString(const Value& object, std::string_view key);
[[nodiscard]] Result<std::uint64_t> requireUInt(const Value& object, std::string_view key);
[[nodiscard]] Result<bool> requireBool(const Value& object, std::string_view key);
[[nodiscard]] Result<const Value*> requireArray(const Value& object, std::string_view key);

} // namespace roclint::core::json
