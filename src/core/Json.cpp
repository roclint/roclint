#include "roclint/core/Json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace roclint::core::json {

Value Value::boolean(bool b) {
    Value v;
    v.type_ = Type::Bool;
    v.bool_ = b;
    return v;
}

Value Value::number(double n) {
    Value v;
    v.type_ = Type::Number;
    v.number_ = n;
    return v;
}

Value Value::string(std::string s) {
    Value v;
    v.type_ = Type::String;
    v.string_ = std::move(s);
    return v;
}

Value Value::array() {
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.type_ = Type::Object;
    return v;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const auto& member : object_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

Value& Value::set(std::string key, Value value) {
    type_ = Type::Object;
    for (auto& member : object_) {
        if (member.first == key) {
            member.second = std::move(value);
            return *this;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
    return *this;
}

Value& Value::push(Value value) {
    type_ = Type::Array;
    array_.push_back(std::move(value));
    return *this;
}

namespace {

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    Parser(std::string_view src, ParseLimits limits) : src_(src), limits_(limits) {}

    Result<Value> run() {
        skipWs();
        auto value = parseValue(0);
        if (!value.ok()) {
            return value;
        }
        skipWs();
        if (pos_ != src_.size()) {
            return fail("Trailing content after JSON document");
        }
        return value;
    }

private:
    Error fail(const std::string& what) const {
        return makeError(ErrorCode::InvalidInput, "JSON parse error at byte " + std::to_string(pos_) + ": " + what);
    }

    void skipWs() noexcept {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consumeLiteral(std::string_view lit) noexcept {
        if (src_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }

    Result<Value> parseValue(std::size_t depth) {
        if (depth > limits_.maxDepth) {
            return fail("Maximum nesting depth exceeded");
        }
        if (pos_ >= src_.size()) {
            return fail("Unexpected end of input");
        }
        const char c = src_[pos_];
        switch (c) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': {
                auto s = parseString();
                if (!s.ok()) {
                    return s.error();
                }
                return Value::string(std::move(s).value());
            }
            case 't':
                if (consumeLiteral("true")) return Value::boolean(true);
                return fail("Invalid literal");
            case 'f':
                if (consumeLiteral("false")) return Value::boolean(false);
                return fail("Invalid literal");
            case 'n':
                if (consumeLiteral("null")) return Value::null();
                return fail("Invalid literal");
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return parseNumber();
                }
                return fail(std::string("Unexpected character '") + c + "'");
        }
    }

    Result<Value> parseObject(std::size_t depth) {
        ++pos_;
        Value obj = Value::object();
        skipWs();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return obj;
        }
        while (true) {
            skipWs();
            if (pos_ >= src_.size() || src_[pos_] != '"') {
                return fail("Expected string key");
            }
            auto key = parseString();
            if (!key.ok()) {
                return key.error();
            }
            if (obj.find(key.value()) != nullptr) {
                return fail("Duplicate key '" + key.value() + "'");
            }
            skipWs();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                return fail("Expected ':' after key");
            }
            ++pos_;
            skipWs();
            auto value = parseValue(depth + 1);
            if (!value.ok()) {
                return value;
            }
            obj.set(std::move(key).value(), std::move(value).value());
            skipWs();
            if (pos_ >= src_.size()) {
                return fail("Unterminated object");
            }
            if (src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (src_[pos_] == '}') {
                ++pos_;
                return obj;
            }
            return fail("Expected ',' or '}' in object");
        }
    }

    Result<Value> parseArray(std::size_t depth) {
        ++pos_;
        Value arr = Value::array();
        skipWs();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return arr;
        }
        while (true) {
            skipWs();
            auto value = parseValue(depth + 1);
            if (!value.ok()) {
                return value;
            }
            arr.push(std::move(value).value());
            skipWs();
            if (pos_ >= src_.size()) {
                return fail("Unterminated array");
            }
            if (src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (src_[pos_] == ']') {
                ++pos_;
                return arr;
            }
            return fail("Expected ',' or ']' in array");
        }
    }

    bool readHex4(std::uint32_t& out) noexcept {
        if (pos_ + 4 > src_.size()) {
            return false;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = src_[pos_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<std::uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<std::uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<std::uint32_t>(h - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    // Validates one raw UTF-8 sequence starting at pos_ and appends it.
    bool copyUtf8(std::string& out) noexcept {
        const auto lead = static_cast<unsigned char>(src_[pos_]);
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if (lead < 0x80) {
            len = 1;
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            len = 2;
            cp = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
            cp = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
            cp = lead & 0x07;
        } else {
            return false;
        }
        if (pos_ + len > src_.size()) {
            return false;
        }
        for (std::size_t i = 1; i < len; ++i) {
            const auto cont = static_cast<unsigned char>(src_[pos_ + i]);
            if ((cont & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cont & 0x3F);
        }
        const bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
        if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        out.append(src_.substr(pos_, len));
        pos_ += len;
        return true;
    }

    Result<std::string> parseString() {
        ++pos_;
        std::string out;
        while (pos_ < src_.size()) {
            const auto c = static_cast<unsigned char>(src_[pos_]);
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c < 0x20) {
                return fail("Unescaped control character in string");
            }
            if (c != '\\') {
                if (!copyUtf8(out)) {
                    return fail("Invalid UTF-8 in string");
                }
                continue;
            }
            ++pos_;
            if (pos_ >= src_.size()) {
                return fail("Unterminated escape sequence");
            }
            const char esc = src_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!readHex4(cp)) {
                        return fail("Invalid \\u escape");
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        std::uint32_t low = 0;
                        if (!consumeLiteral("\\u") || !readHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return fail("Unpaired high surrogate in \\u escape");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("Unpaired low surrogate in \\u escape");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("Invalid escape character");
            }
        }
        return fail("Unterminated string");
    }

    Result<Value> parseNumber() {
        const std::size_t start = pos_;
        if (src_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= src_.size()) {
            return fail("Truncated number");
        }
        if (src_[pos_] == '0') {
            ++pos_;
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        } else {
            return fail("Invalid number");
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            const std::size_t fracStart = pos_;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
            if (pos_ == fracStart) {
                return fail("Invalid fraction");
            }
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            const std::size_t expStart = pos_;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
            if (pos_ == expStart) {
                return fail("Invalid exponent");
            }
        }
        const std::string token(src_.substr(start, pos_ - start));
        const double value = std::strtod(token.c_str(), nullptr);
        if (!std::isfinite(value)) {
            return fail("Number out of range");
        }
        return Value::number(value);
    }

    std::string_view src_;
    ParseLimits limits_;
    std::size_t pos_{0};
};

void serializeString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    out.push_back('"');
}

void serializeInto(std::string& out, const Value& v) {
    switch (v.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Value::Type::Number: {
            const double n = v.asNumber();
            if (!std::isfinite(n)) {
                out += "null";
            } else if (std::floor(n) == n && std::fabs(n) < 9007199254740992.0) {
                out += std::to_string(static_cast<long long>(n));
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", n);
                out += buf;
            }
            break;
        }
        case Value::Type::String: serializeString(out, v.asString()); break;
        case Value::Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& item : v.asArray()) {
                if (!first) out.push_back(',');
                first = false;
                serializeInto(out, item);
            }
            out.push_back(']');
            break;
        }
        case Value::Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& member : v.asObject()) {
                if (!first) out.push_back(',');
                first = false;
                serializeString(out, member.first);
                out.push_back(':');
                serializeInto(out, member.second);
            }
            out.push_back('}');
            break;
        }
    }
}

} // namespace

Result<Value> parse(std::string_view text, ParseLimits limits) {
    if (text.size() > limits.maxBytes) {
        return makeError(ErrorCode::InvalidInput,
                         "JSON input of " + std::to_string(text.size()) + " bytes exceeds limit of " +
                             std::to_string(limits.maxBytes) + " bytes");
    }
    Parser parser(text, limits);
    return parser.run();
}

std::string serialize(const Value& value) {
    std::string out;
    serializeInto(out, value);
    return out;
}

Result<std::string> requireString(const Value& object, std::string_view key) {
    const Value* v = object.find(key);
    if (v == nullptr || !v->isString()) {
        return makeError(ErrorCode::InvalidInput, "Missing or non-string field '" + std::string(key) + "'");
    }
    return v->asString();
}

Result<std::uint64_t> requireUInt(const Value& object, std::string_view key) {
    const Value* v = object.find(key);
    if (v == nullptr || !v->isNumber() || v->asNumber() < 0.0 || std::floor(v->asNumber()) != v->asNumber() ||
        v->asNumber() >= 9007199254740992.0) {
        return makeError(ErrorCode::InvalidInput, "Missing or non-integer field '" + std::string(key) + "'");
    }
    return static_cast<std::uint64_t>(v->asNumber());
}

Result<bool> requireBool(const Value& object, std::string_view key) {
    const Value* v = object.find(key);
    if (v == nullptr || !v->isBool()) {
        return makeError(ErrorCode::InvalidInput, "Missing or non-boolean field '" + std::string(key) + "'");
    }
    return v->asBool();
}

Result<const Value*> requireArray(const Value& object, std::string_view key) {
    const Value* v = object.find(key);
    if (v == nullptr || !v->isArray()) {
        return makeError(ErrorCode::InvalidInput, "Missing or non-array field '" + std::string(key) + "'");
    }
    return v;
}

} // namespace roclint::core::json
