#include "roclint/storage/AstSymbolExtractor.hpp"
#include "roclint/analysis/SourceSpan.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

using namespace roclint::storage;

namespace {

const AstSymbol* findSymbol(const std::vector<AstSymbol>& symbols, std::string_view qualified,
                            AstNodeKind kind) {
    const auto found = std::find_if(symbols.begin(), symbols.end(), [&](const AstSymbol& symbol) {
        return symbol.qualifiedName == qualified && symbol.kind == kind;
    });
    return found == symbols.end() ? nullptr : &*found;
}

void testHierarchicalCppExtraction() {
    const std::string source =
        "namespace demo {\n"
        "class SessionManager {\n"
        "public:\n"
        "  enum Mode { Fast, Safe };\n"
        "  struct Config { int retries; };\n"
        "  bool readPacket(size_t offset, uint32_t flags);\n"
        "  int timeout;\n"
        "};\n"
        "int freeFunction(int value) { return value + 1; }\n"
        "}\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "src/SessionManager.h");
    ROCLINT_CHECK(result.parseOk);
    ROCLINT_CHECK(result.error.empty());
    ROCLINT_CHECK(result.symbols.size() >= 9);

    const auto* ns = findSymbol(result.symbols, "demo", AstNodeKind::Namespace);
    const auto* cls = findSymbol(result.symbols, "demo::SessionManager", AstNodeKind::Class);
    const auto* method = findSymbol(result.symbols, "demo::SessionManager::readPacket", AstNodeKind::Method);
    const auto* offset = findSymbol(result.symbols, "demo::SessionManager::readPacket::offset",
                                    AstNodeKind::Parameter);
    const auto* flags = findSymbol(result.symbols, "demo::SessionManager::readPacket::flags",
                                   AstNodeKind::Parameter);
    const auto* timeout = findSymbol(result.symbols, "demo::SessionManager::timeout", AstNodeKind::Field);
    const auto* freeFn = findSymbol(result.symbols, "demo::freeFunction", AstNodeKind::Function);
    ROCLINT_CHECK(ns && cls && method && offset && flags && timeout && freeFn);
    ROCLINT_CHECK(cls->parentSymbolId == ns->symbolId);
    ROCLINT_CHECK(method->parentSymbolId == cls->symbolId);
    ROCLINT_CHECK(offset->parentSymbolId == method->symbolId);
    ROCLINT_CHECK(flags->parentSymbolId == method->symbolId);
    ROCLINT_CHECK(timeout->parentSymbolId == cls->symbolId);
    ROCLINT_CHECK(offset->typeName == "size_t");
    ROCLINT_CHECK(flags->typeName == "uint32_t");
    ROCLINT_CHECK(method->signature.find("readPacket") != std::string::npos);
    ROCLINT_CHECK(method->signature.find("return value") == std::string::npos);
    ROCLINT_CHECK(freeFn->signature.find("return value") == std::string::npos);
    ROCLINT_CHECK(method->startLine == 6 && offset->startLine == 6);
    ROCLINT_CHECK(offset->span.startByte < offset->span.endByte);
    ROCLINT_CHECK(offset->symbolId.starts_with("sym_") && offset->symbolId.size() == 28);
    for (const auto& symbol : result.symbols) {
        ROCLINT_CHECK(roclint::core::analysis::isValidSpan(source, symbol.span));
    }
}

void testParserRecoversMacroHeavyHeader() {
    const std::string source =
        "class Widget : public Base {\n"
        "    Q_PROPERTY(bool visible READ isVisible NOTIFY visibilityChanged)\n"
        "public:\n"
        "    bool isVisible() const;\n"
        "    int count;\n"
        "};\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "src/Widget.h");
    ROCLINT_CHECK(!result.parseOk);
    ROCLINT_CHECK(result.recovered);
    const auto* widget = findSymbol(result.symbols, "Widget", AstNodeKind::Class);
    const auto* visible = findSymbol(result.symbols, "Widget::isVisible", AstNodeKind::Method);
    const auto* count = findSymbol(result.symbols, "Widget::count", AstNodeKind::Field);
    ROCLINT_CHECK(widget && visible && count);
    for (const auto& symbol : result.symbols) {
        ROCLINT_CHECK(symbol.name != "Q_PROPERTY");
        ROCLINT_CHECK(symbol.name != "READ");
        ROCLINT_CHECK(symbol.name != "NOTIFY");
        ROCLINT_CHECK(roclint::core::analysis::isValidSpan(source, symbol.span));
    }
}

void testParserRejectsMalformedSource() {
    const auto result = AstSymbolExtractor::extractSymbols("class Broken { void f( {", "broken.cpp");
    ROCLINT_CHECK(!result.parseOk);
    ROCLINT_CHECK(result.recovered);
    for (const auto& symbol : result.symbols)
        ROCLINT_CHECK(roclint::core::analysis::isValidSpan("class Broken { void f( {", symbol.span));
}

void testBodyFreeBoundedSignatures() {
    const std::string longString(1500, 'x');
    const std::string longName(1500, 'n');
    const std::string source =
        "class Handler {\n"
        "public:\n"
        "  int value = [] { return 12345; }();\n"
        "  int braced{[] { return 6789; }()};\n"
        "  int longText = \"" + longString + "\";\n"
        "  int " + longName + ";\n"
        "  Handler() : value([] { return 7; }()) {}\n"
        "  void accept(int first = [] { return 99; }());\n"
        "};\n"
        "using Callback = decltype([] { return 42; });\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "bodies.hpp");
    ROCLINT_CHECK(result.parseOk || result.recovered);
    const auto* value = findSymbol(result.symbols, "Handler::value", AstNodeKind::Field);
    const auto* braced = findSymbol(result.symbols, "Handler::braced", AstNodeKind::Field);
    const auto* longText = findSymbol(result.symbols, "Handler::longText", AstNodeKind::Field);
    const auto* longField = findSymbol(result.symbols, "Handler::" + longName, AstNodeKind::Field);
    const auto* constructor = findSymbol(result.symbols, "Handler::Handler", AstNodeKind::Method);
    const auto* accept = findSymbol(result.symbols, "Handler::accept", AstNodeKind::Method);
    const auto* first = findSymbol(result.symbols, "Handler::accept::first", AstNodeKind::Parameter);
    const auto* alias = findSymbol(result.symbols, "Callback", AstNodeKind::TypeAlias);
    ROCLINT_CHECK(value && braced && longText && longField && constructor && accept && first && alias);
    for (const auto* symbol : {value, braced, longText, longField, constructor, accept, first, alias}) {
        ROCLINT_CHECK(symbol->signature.find("return") == std::string::npos);
        ROCLINT_CHECK(symbol->signature.size() <= 1200);
    }
    ROCLINT_CHECK(accept->signature.find("first") != std::string::npos);
    ROCLINT_CHECK(first->signature.find('=') == std::string::npos);
    ROCLINT_CHECK(longField->signature.size() <= 1200);
}

void testExtractionLatency() {
    const std::string source =
        "namespace demo { class A { bool f(int alpha, int beta); int value; }; "
        "int g(int x) { return x; } }";
    constexpr int kSamples = 100;
    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < kSamples; ++index) {
        const auto result = AstSymbolExtractor::extractSymbols(source, "latency.cpp");
        ROCLINT_CHECK(result.parseOk);
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count() / kSamples;
    std::cout << "  [INFO] AST extraction average=" << elapsed << " ms\n";
    ROCLINT_CHECK(elapsed < 5.0); // Generous CI ceiling; target is <1.5 ms.
}

} // namespace

int main() {
    testHierarchicalCppExtraction();
    testParserRecoversMacroHeavyHeader();
    testParserRejectsMalformedSource();
    testBodyFreeBoundedSignatures();
    testExtractionLatency();
    std::cout << "AST symbol extraction, hierarchy, recovery, signatures and byte spans passed.\n";
    return 0;
}
