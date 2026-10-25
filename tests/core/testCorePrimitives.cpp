#include "roclint/core/Json.hpp"
#include "roclint/core/Result.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/core/prompt_state/TextNormalization.hpp"
#include "roclint/core/schema/SchemaVisitor.hpp"
#include "support/check.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace roclint::core;
namespace fs = std::filesystem;

namespace {

constexpr const char* kEmptyDigest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr const char* kAbcDigest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

void testResultAndStatus() {
    Result<int> good = 7;
    ROCLINT_CHECK(good.ok());
    ROCLINT_CHECK(good.value() == 7);
    Result<int> bad = makeError(ErrorCode::NotFound, "missing");
    ROCLINT_CHECK(!bad.ok());
    ROCLINT_CHECK(bad.error().code == ErrorCode::NotFound);
    ROCLINT_CHECK(bad.error().message == "missing");

    ROCLINT_CHECK(Status::success().ok());
    Status failed = makeError(ErrorCode::Conflict, "taken");
    ROCLINT_CHECK(!failed.ok());
    ROCLINT_CHECK(failed.error().code == ErrorCode::Conflict);
    ROCLINT_CHECK(std::string(toString(ErrorCode::StaleVersion)).size() > 0);
}

void testLineEndingNormalization() {
    ROCLINT_CHECK(normalizeLineEndings("a\r\nb") == "a\nb");
    ROCLINT_CHECK(normalizeLineEndings("a\rb") == "a\nb");
    ROCLINT_CHECK(normalizeLineEndings("a\r\r\nb") == "a\n\nb");
    ROCLINT_CHECK(normalizeLineEndings("trailing\r") == "trailing\n");
    ROCLINT_CHECK(normalizeLineEndings("  keep \t spacing \n") == "  keep \t spacing \n");
    ROCLINT_CHECK(normalizeLineEndings("") == "");
    ROCLINT_CHECK(hasCanonicalLineEndings("a\nb"));
    ROCLINT_CHECK(!hasCanonicalLineEndings("a\r\nb"));
    ROCLINT_CHECK(!hasCanonicalLineEndings("a\rb"));
}

void testSha256Vectors() {
    ROCLINT_CHECK(crypto::Sha256::hexDigest("") == kEmptyDigest);
    ROCLINT_CHECK(crypto::Sha256::hexDigest("abc") == kAbcDigest);
    ROCLINT_CHECK(crypto::Sha256::hexDigest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    ROCLINT_CHECK(crypto::Sha256::hexDigest(std::string(1'000'000, 'a')) ==
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Incremental updates match a single update.
    crypto::Sha256 sha;
    sha.update("a");
    sha.update("bc");
    ROCLINT_CHECK(sha.finalizeHex() == kAbcDigest);
}

void testCanonicalHasherIsUnambiguous() {
    const auto ab_c = crypto::CanonicalHasher("d").field("ab").field("c").finalizeHex();
    const auto a_bc = crypto::CanonicalHasher("d").field("a").field("bc").finalizeHex();
    ROCLINT_CHECK(ab_c != a_bc);
    const auto otherDomain = crypto::CanonicalHasher("e").field("ab").field("c").finalizeHex();
    ROCLINT_CHECK(ab_c != otherDomain);
    const auto again = crypto::CanonicalHasher("d").field("ab").field("c").finalizeHex();
    ROCLINT_CHECK(ab_c == again);
    ROCLINT_CHECK(crypto::CanonicalHasher("d").number(1).finalizeHex() !=
                  crypto::CanonicalHasher("d").number(2).finalizeHex());
    ROCLINT_CHECK(crypto::CanonicalHasher("d").field("").finalizeHex() !=
                  crypto::CanonicalHasher("d").finalizeHex());
}

void testDigestNormalization() {
    auto bare = crypto::normalizeSha256Digest(kAbcDigest);
    ROCLINT_CHECK(bare.ok() && bare.value() == kAbcDigest);
    std::string upper = std::string("sha256:") + kAbcDigest;
    for (std::size_t i = 7; i < upper.size(); ++i) {
        if (upper[i] >= 'a' && upper[i] <= 'f') upper[i] = static_cast<char>(upper[i] - 'a' + 'A');
    }
    auto prefixed = crypto::normalizeSha256Digest(upper);
    ROCLINT_CHECK(prefixed.ok() && prefixed.value() == kAbcDigest);
    ROCLINT_CHECK(!crypto::normalizeSha256Digest("sha256:abc").ok());
    ROCLINT_CHECK(!crypto::normalizeSha256Digest(std::string(64, 'g')).ok());
    ROCLINT_CHECK(!crypto::normalizeSha256Digest("").ok());
}

void testFileHashing() {
    const fs::path dir = fs::temp_directory_path() / fs::path(u8"roclint_sha_\u00fcmlaut_\u6587");
    fs::create_directories(dir);
    const fs::path file = dir / "model.bin";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "abc";
    }
    auto digest = crypto::sha256File(file);
    ROCLINT_CHECK(digest.ok());
    ROCLINT_CHECK(digest.value() == kAbcDigest);
    ROCLINT_CHECK(crypto::verifyFileSha256(file, std::string("sha256:") + kAbcDigest).ok());

    auto mismatch = crypto::verifyFileSha256(file, kEmptyDigest);
    ROCLINT_CHECK(!mismatch.ok());
    ROCLINT_CHECK(mismatch.error().code == ErrorCode::ValidationFailed);

    auto malformed = crypto::verifyFileSha256(file, "not-a-digest");
    ROCLINT_CHECK(!malformed.ok());
    ROCLINT_CHECK(malformed.error().code == ErrorCode::InvalidInput);

    auto missing = crypto::sha256File(dir / "absent.bin");
    ROCLINT_CHECK(!missing.ok());
    ROCLINT_CHECK(missing.error().code == ErrorCode::NotFound);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void testJsonParsing() {
    auto ok = json::parse(R"({"a": [1, true, null, "x"], "b": {"c": -2.5e1}})");
    ROCLINT_CHECK(ok.ok());
    const auto& root = ok.value();
    ROCLINT_CHECK(root.find("a")->asArray().size() == 4);
    ROCLINT_CHECK(root.find("b")->find("c")->asNumber() == -25.0);

    ROCLINT_CHECK(!json::parse(R"({"a": 1)").ok());              // truncated
    ROCLINT_CHECK(!json::parse(R"({"a": 1} extra)").ok());       // trailing content
    ROCLINT_CHECK(!json::parse(R"({"a": 1, "a": 2})").ok());     // duplicate key
    ROCLINT_CHECK(!json::parse("{\"a\": \"\x01\"}").ok());       // unescaped control char
    ROCLINT_CHECK(!json::parse("{\"a\": \"\xff\"}").ok());       // invalid UTF-8
    ROCLINT_CHECK(!json::parse(R"({"a": 01})").ok());            // leading zero
    ROCLINT_CHECK(!json::parse("").ok());

    std::string deep(40, '[');
    deep += std::string(40, ']');
    ROCLINT_CHECK(!json::parse(deep).ok());
    ROCLINT_CHECK(json::parse(deep, json::ParseLimits{1024, 64}).ok());
    ROCLINT_CHECK(!json::parse(R"({"a": "0123456789"})", json::ParseLimits{8, 8}).ok());
}

void testJsonUnicodeEscapes() {
    // Backslashes are escaped explicitly: MSVC expands universal-character-names even in raw strings.
    auto basic = json::parse("{\"s\": \"caf\\u00e9 \\u6587\"}");
    ROCLINT_CHECK(basic.ok());
    ROCLINT_CHECK(basic.value().find("s")->asString() == "caf\xc3\xa9 \xe6\x96\x87");

    auto pair = json::parse("{\"s\": \"\\ud83d\\ude00\"}");
    ROCLINT_CHECK(pair.ok());
    ROCLINT_CHECK(pair.value().find("s")->asString() == "\xf0\x9f\x98\x80");

    ROCLINT_CHECK(!json::parse("{\"s\": \"\\ud83d\"}").ok()); // lone high surrogate
    ROCLINT_CHECK(!json::parse("{\"s\": \"\\u00zz\"}").ok());
}

void testJsonSerializeRoundTrip() {
    json::Value v = json::Value::object();
    v.set("text", json::Value::string("line\n\"quoted\"\t\xc3\xa9"));
    v.set("n", json::Value::number(3));
    v.set("flag", json::Value::boolean(false));
    json::Value arr = json::Value::array();
    arr.push(json::Value::null());
    v.set("list", std::move(arr));
    const auto text = json::serialize(v);
    auto back = json::parse(text);
    ROCLINT_CHECK(back.ok());
    ROCLINT_CHECK(back.value().find("text")->asString() == "line\n\"quoted\"\t\xc3\xa9");
    ROCLINT_CHECK(back.value().find("n")->asNumber() == 3.0);
    ROCLINT_CHECK(json::serialize(back.value()) == text);

    ROCLINT_CHECK(json::requireString(back.value(), "text").ok());
    ROCLINT_CHECK(!json::requireString(back.value(), "n").ok());
    ROCLINT_CHECK(json::requireUInt(back.value(), "n").value() == 3);
    ROCLINT_CHECK(!json::requireBool(back.value(), "missing").ok());
}

struct DummyEntity {
    std::string name{"test"};
    std::uint64_t count{42};
    double score{3.14159};
    bool active{true};
    std::array<double, 3> coords{1.0, 2.0, 3.0};
    std::optional<std::string> tag{"production"};

    template <typename Visitor>
    void visit(Visitor& v) const {
        v("name", name);
        v("count", count);
        v("score", score);
        v("active", active);
        v("coords", coords);
        v("tag", tag);
    }

    template <typename Visitor>
    void visit(Visitor& v) {
        v("name", name);
        v("count", count);
        v("score", score);
        v("active", active);
        v("coords", coords);
        v("tag", tag);
    }

    bool operator==(const DummyEntity&) const = default;
};

void testSchemaVisitorRoundTrip() {
    DummyEntity original;
    original.name = "alpha";
    original.count = 999;
    original.score = 0.875;
    original.active = false;
    original.coords = {10.5, 20.25, 30.75};
    original.tag = "calibrated";

    // HashVisitor produces deterministic hash
    crypto::CanonicalHasher h1("test.domain.v1");
    schema::HashVisitor v1(h1);
    original.visit(v1);
    const auto hash1 = h1.finalizeHex();

    crypto::CanonicalHasher h2("test.domain.v1");
    schema::HashVisitor v2(h2);
    original.visit(v2);
    const auto hash2 = h2.finalizeHex();
    ROCLINT_CHECK(hash1 == hash2);

    // JsonEncodeVisitor -> JsonDecodeVisitor round trip
    auto root = json::Value::object();
    schema::JsonEncodeVisitor enc(root);
    original.visit(enc);

    DummyEntity decoded;
    schema::JsonDecodeVisitor dec(root);
    decoded.visit(dec);
    ROCLINT_CHECK(dec.status().ok());
    ROCLINT_CHECK(decoded == original);

    // Missing required field fails cleanly
    auto badRoot = json::Value::object();
    badRoot.set("name", json::Value::string("incomplete"));
    DummyEntity failEntity;
    schema::JsonDecodeVisitor failDec(badRoot);
    failEntity.visit(failDec);
    ROCLINT_CHECK(!failDec.status().ok());
    ROCLINT_CHECK(failDec.status().error().code == ErrorCode::InvalidInput);
}

} // namespace

int main() {
    std::cout << "[Test CorePrimitives] Starting...\n";
    testResultAndStatus();
    testLineEndingNormalization();
    testSha256Vectors();
    testCanonicalHasherIsUnambiguous();
    testDigestNormalization();
    testFileHashing();
    testJsonParsing();
    testJsonUnicodeEscapes();
    testJsonSerializeRoundTrip();
    testSchemaVisitorRoundTrip();
    std::cout << "[Test CorePrimitives] All tests PASSED.\n";
    return 0;
}
