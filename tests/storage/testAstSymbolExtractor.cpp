#include "roclint/storage/AstSymbolExtractor.hpp"
#include "roclint/analysis/SourceSpan.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace roclint::storage;

namespace {

const AstSymbol* findSymbol(const std::vector<AstSymbol>& symbols, std::string_view qualified,
                            AstNodeKind kind) {
    const auto found = std::find_if(symbols.begin(), symbols.end(), [&](const AstSymbol& symbol) {
        return symbol.qualifiedName == qualified && symbol.kind == kind;
    });
    return found == symbols.end() ? nullptr : &*found;
}

const AstSymbol* findRole(const std::vector<AstSymbol>& symbols, std::string_view qualified,
                          AstSymbolRole role) {
    const auto found = std::find_if(symbols.begin(), symbols.end(), [&](const AstSymbol& symbol) {
        return symbol.qualifiedName == qualified && symbol.role == role;
    });
    return found == symbols.end() ? nullptr : &*found;
}

const AstSymbol* findSite(const std::vector<AstSymbol>& symbols, std::string_view qualified,
                          AstSymbolRole role, bool definition) {
    const auto found = std::find_if(symbols.begin(), symbols.end(), [&](const AstSymbol& symbol) {
        return symbol.qualifiedName == qualified && symbol.role == role &&
               symbol.isDefinition == definition;
    });
    return found == symbols.end() ? nullptr : &*found;
}

const AstSymbol* findByKeySite(const std::vector<AstSymbol>& symbols, std::string_view qualified,
                               AstSymbolRole role, std::string_view signatureKey, bool definition) {
    const auto found = std::find_if(symbols.begin(), symbols.end(), [&](const AstSymbol& symbol) {
        return symbol.qualifiedName == qualified && symbol.role == role &&
               symbol.signatureKey == signatureKey && symbol.isDefinition == definition;
    });
    return found == symbols.end() ? nullptr : &*found;
}

std::vector<const AstSymbol*> findAllQualified(const std::vector<AstSymbol>& symbols,
                                               std::string_view qualified) {
    std::vector<const AstSymbol*> found;
    for (const auto& symbol : symbols) {
        if (symbol.qualifiedName == qualified) found.push_back(&symbol);
    }
    return found;
}

std::string spanText(std::string_view source, const AstSymbol& symbol) {
    if (symbol.span.endByte > source.size() || symbol.span.startByte > symbol.span.endByte) return {};
    return std::string(source.substr(symbol.span.startByte, symbol.span.length()));
}

bool hasParameterPrefix(const std::vector<AstSymbol>& symbols, std::string_view prefix) {
    return std::any_of(symbols.begin(), symbols.end(), [&](const AstSymbol& symbol) {
        return symbol.role == AstSymbolRole::Parameter &&
               symbol.qualifiedName.substr(0, prefix.size()) == prefix;
    });
}

std::set<std::string> signatureKeys(const std::vector<AstSymbol>& symbols,
                                    std::string_view qualified) {
    std::set<std::string> keys;
    for (const auto& symbol : symbols) {
        if (symbol.qualifiedName == qualified) keys.insert(symbol.signatureKey);
    }
    return keys;
}

void checkInvariants(std::string_view source, const std::vector<AstSymbol>& symbols) {
    std::set<std::string> ids;
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        const auto& symbol = symbols[index];
        ROCLINT_CHECK(roclint::core::analysis::isValidSpan(source, symbol.span));
        ROCLINT_CHECK(symbol.signature.size() <= 1200);
        ROCLINT_CHECK(symbol.signatureKey.size() <= 1200);
        ROCLINT_CHECK(symbol.symbolId.starts_with("sym_") && symbol.symbolId.size() == 28);
        ROCLINT_CHECK(symbol.role != AstSymbolRole::Unknown);
        ROCLINT_CHECK(ids.insert(symbol.symbolId).second);
        if (symbol.role == AstSymbolRole::Parameter) ROCLINT_CHECK(symbol.parameterIndex >= 1);
        else ROCLINT_CHECK(symbol.parameterIndex == 0);
        if (index > 0) {
            const auto& previous = symbols[index - 1];
            ROCLINT_CHECK(previous.span.startByte <= symbol.span.startByte);
            if (previous.span.startByte == symbol.span.startByte)
                ROCLINT_CHECK(previous.qualifiedName <= symbol.qualifiedName);
        }
    }
}

void testSharedSymbolModelRolesAndKindCompatibility() {
    const std::string source =
        "namespace demo {\n"
        "struct Point { int x, y; };\n"
        "enum class Mode { Fast, Safe };\n"
        "using Alias = int;\n"
        "int globalCount;\n"
        "class Engine {\n"
        "public:\n"
        "  Engine();\n"
        "  ~Engine();\n"
        "  bool operator==(const Engine& other) const;\n"
        "  int run(int n) const;\n"
        "  int value;\n"
        "};\n"
        "}\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "roles.hpp");
    ROCLINT_CHECK(result.parseOk);
    checkInvariants(source, result.symbols);

    const auto* ns = findRole(result.symbols, "demo", AstSymbolRole::Namespace);
    const auto* point = findRole(result.symbols, "demo::Point", AstSymbolRole::Type);
    const auto* pointX = findRole(result.symbols, "demo::Point::x", AstSymbolRole::Field);
    const auto* pointY = findRole(result.symbols, "demo::Point::y", AstSymbolRole::Field);
    const auto* mode = findRole(result.symbols, "demo::Mode", AstSymbolRole::Type);
    const auto* fast = findRole(result.symbols, "demo::Mode::Fast", AstSymbolRole::Enumerator);
    const auto* alias = findRole(result.symbols, "demo::Alias", AstSymbolRole::TypeAlias);
    const auto* variable = findRole(result.symbols, "demo::globalCount", AstSymbolRole::Variable);
    const auto* engine = findRole(result.symbols, "demo::Engine", AstSymbolRole::Type);
    const auto* constructor = findRole(result.symbols, "demo::Engine::Engine", AstSymbolRole::Constructor);
    const auto* destructor = findRole(result.symbols, "demo::Engine::~Engine", AstSymbolRole::Destructor);
    const auto* equality = findRole(result.symbols, "demo::Engine::operator==", AstSymbolRole::Operator);
    const auto* method = findRole(result.symbols, "demo::Engine::run", AstSymbolRole::Method);
    const auto* field = findRole(result.symbols, "demo::Engine::value", AstSymbolRole::Field);
    const auto* parameter = findRole(result.symbols, "demo::Engine::run::n", AstSymbolRole::Parameter);
    ROCLINT_CHECK(ns && point && pointX && pointY && mode && fast && alias && variable && engine &&
                  constructor && destructor && equality && method && field && parameter);
    ROCLINT_CHECK(spanText(source, *pointX) == "x");
    ROCLINT_CHECK(spanText(source, *pointY) == "y");
    ROCLINT_CHECK(pointX->signature == "int x");
    ROCLINT_CHECK(pointY->signature == "int y");

    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Unknown)) == "UNKNOWN");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Namespace)) == "NAMESPACE");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Type)) == "TYPE");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Function)) == "FUNCTION");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Method)) == "METHOD");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Constructor)) == "CONSTRUCTOR");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Destructor)) == "DESTRUCTOR");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Operator)) == "OPERATOR");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Variable)) == "VARIABLE");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Field)) == "FIELD");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Parameter)) == "PARAMETER");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::Enumerator)) == "ENUMERATOR");
    ROCLINT_CHECK(std::string(toString(AstSymbolRole::TypeAlias)) == "TYPE_ALIAS");
    ROCLINT_CHECK(std::string(toString(AstNodeKind::Struct)) == "STRUCT");
    ROCLINT_CHECK(std::string(toString(AstNodeKind::Method)) == "METHOD");

    ROCLINT_CHECK(point->kind == AstNodeKind::Struct);
    ROCLINT_CHECK(mode->kind == AstNodeKind::Enum);
    ROCLINT_CHECK(alias->kind == AstNodeKind::TypeAlias);
    ROCLINT_CHECK(variable->kind == AstNodeKind::Unknown);
    ROCLINT_CHECK(constructor->kind == AstNodeKind::Method);
    ROCLINT_CHECK(destructor->kind == AstNodeKind::Method);
    ROCLINT_CHECK(equality->kind == AstNodeKind::Method);
    ROCLINT_CHECK(field->kind == AstNodeKind::Field);
    ROCLINT_CHECK(parameter->kind == AstNodeKind::Parameter);
    ROCLINT_CHECK(parameter->parameterIndex == 1);
    ROCLINT_CHECK(constructor->isDeclaration && !constructor->isDefinition);
    ROCLINT_CHECK(method->isDeclaration && !method->isDefinition);
    ROCLINT_CHECK(field->isDeclaration && !field->isDefinition);
}

void testGlobalAndNamespaceVariableDeclarators() {
    const std::string source =
        "namespace demo {\n"
        "int width, height;\n"
        "extern int externalVar;\n"
        "extern int externalWithValue = 3;\n"
        "static inline constexpr int limit = 5;\n"
        "int* pointer = nullptr;\n"
        "int& reference = width;\n"
        "const int fixed = 1;\n"
        "}\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "variables.cpp");
    ROCLINT_CHECK(result.parseOk);
    checkInvariants(source, result.symbols);
    const auto again = AstSymbolExtractor::extractSymbols(source, "variables.cpp");
    ROCLINT_CHECK(result.symbols == again.symbols);

    const auto* width = findRole(result.symbols, "demo::width", AstSymbolRole::Variable);
    const auto* height = findRole(result.symbols, "demo::height", AstSymbolRole::Variable);
    const auto* external = findRole(result.symbols, "demo::externalVar", AstSymbolRole::Variable);
    const auto* externalValue =
        findRole(result.symbols, "demo::externalWithValue", AstSymbolRole::Variable);
    const auto* limit = findRole(result.symbols, "demo::limit", AstSymbolRole::Variable);
    const auto* pointer = findRole(result.symbols, "demo::pointer", AstSymbolRole::Variable);
    const auto* reference = findRole(result.symbols, "demo::reference", AstSymbolRole::Variable);
    const auto* fixed = findRole(result.symbols, "demo::fixed", AstSymbolRole::Variable);
    const auto* ns = findRole(result.symbols, "demo", AstSymbolRole::Namespace);
    ROCLINT_CHECK(width && height && external && externalValue && limit && pointer && reference &&
                  fixed && ns);
    ROCLINT_CHECK(width->kind == AstNodeKind::Unknown);
    ROCLINT_CHECK(width->isDefinition && !width->isDeclaration);
    ROCLINT_CHECK(height->isDefinition && !height->isDeclaration);
    ROCLINT_CHECK(spanText(source, *width) == "width");
    ROCLINT_CHECK(spanText(source, *height) == "height");
    ROCLINT_CHECK(width->signature == "int width");
    ROCLINT_CHECK(height->signature == "int height");
    ROCLINT_CHECK(width->parentSymbolId == ns->symbolId);
    ROCLINT_CHECK(height->parentSymbolId == ns->symbolId);
    ROCLINT_CHECK(external->isDeclaration && !external->isDefinition);
    ROCLINT_CHECK(externalValue->isDefinition && !externalValue->isDeclaration);
    ROCLINT_CHECK(limit->isStatic && limit->isInline && limit->isConstexpr);
    ROCLINT_CHECK(pointer->signature.find("int*") != std::string::npos);
    ROCLINT_CHECK(pointer->isReference == false);
    ROCLINT_CHECK(reference->isReference);
    ROCLINT_CHECK(fixed->isConst);
    for (const auto* symbol : {width, height, external, externalValue, limit, pointer, reference, fixed})
        ROCLINT_CHECK(symbol->signatureKey.empty());
}

void testAnonymousNamespaceFileLocalIdentity() {
    const std::string source =
        "namespace demo {\n"
        "namespace {\n"
        "int hiddenCache;\n"
        "void hiddenFn(int n) { (void)n; }\n"
        "}\n"
        "}\n";
    const auto first = AstSymbolExtractor::extractSymbols(source, "src/first.cpp");
    const auto second = AstSymbolExtractor::extractSymbols(source, "src/second.cpp");
    ROCLINT_CHECK(first.parseOk && second.parseOk);
    checkInvariants(source, first.symbols);
    checkInvariants(source, second.symbols);

    const auto* anon = findRole(first.symbols, "demo::(anonymous namespace)", AstSymbolRole::Namespace);
    const auto* cache = findRole(first.symbols, "demo::(anonymous namespace)::hiddenCache",
                                 AstSymbolRole::Variable);
    const auto* function = findRole(first.symbols, "demo::(anonymous namespace)::hiddenFn",
                                    AstSymbolRole::Function);
    ROCLINT_CHECK(anon && cache && function);
    ROCLINT_CHECK(anon->name == "(anonymous namespace)");
    ROCLINT_CHECK(cache->parentSymbolId == anon->symbolId);
    ROCLINT_CHECK(function->parentSymbolId == anon->symbolId);
    ROCLINT_CHECK(cache->filePath == "src/first.cpp");
    ROCLINT_CHECK(cache->qualifiedName.find("demo::(anonymous namespace)::") == 0);

    const auto* otherCache = findRole(second.symbols, "demo::(anonymous namespace)::hiddenCache",
                                      AstSymbolRole::Variable);
    ROCLINT_CHECK(otherCache != nullptr);
    ROCLINT_CHECK(otherCache->qualifiedName == cache->qualifiedName);
    ROCLINT_CHECK(otherCache->filePath == "src/second.cpp");
    ROCLINT_CHECK(otherCache->symbolId != cache->symbolId);
}

void testOutOfClassMethodDefinitions() {
    const std::string source =
        "namespace demo {\n"
        "class Engine {\n"
        "public:\n"
        "  void run(int n) const noexcept;\n"
        "  int value;\n"
        "};\n"
        "void Engine::run(int n) const noexcept { return; }\n"
        "}\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "src/engine.cpp");
    ROCLINT_CHECK(result.parseOk);
    checkInvariants(source, result.symbols);

    const auto* engine = findRole(result.symbols, "demo::Engine", AstSymbolRole::Type);
    const auto* declaration =
        findSite(result.symbols, "demo::Engine::run", AstSymbolRole::Method, false);
    const auto* definition =
        findSite(result.symbols, "demo::Engine::run", AstSymbolRole::Method, true);
    ROCLINT_CHECK(engine && declaration && definition);
    ROCLINT_CHECK(declaration->isDeclaration && !declaration->isDefinition);
    ROCLINT_CHECK(definition->isDefinition && !definition->isDeclaration);
    ROCLINT_CHECK(declaration->parentSymbolId == engine->symbolId);
    ROCLINT_CHECK(definition->parentSymbolId == engine->symbolId);
    ROCLINT_CHECK(declaration->signatureKey == "demo::Engine::run(int) const noexcept");
    ROCLINT_CHECK(definition->signatureKey == declaration->signatureKey);
    ROCLINT_CHECK(definition->signature.find("return") == std::string::npos);
    ROCLINT_CHECK(definition->qualifiers == "const noexcept");
    ROCLINT_CHECK(definition->isConst && !definition->isVariadic);

    const auto parameters = findAllQualified(result.symbols, "demo::Engine::run::n");
    ROCLINT_CHECK(parameters.size() == 2);
    for (const auto* parameter : parameters) {
        ROCLINT_CHECK(parameter->role == AstSymbolRole::Parameter);
        ROCLINT_CHECK(parameter->parameterIndex == 1);
        ROCLINT_CHECK(parameter->isReference == false);
        ROCLINT_CHECK(parameter->parentSymbolId == declaration->symbolId ||
                      parameter->parentSymbolId == definition->symbolId);
    }
    ROCLINT_CHECK(parameters[0]->parentSymbolId != parameters[1]->parentSymbolId);
}

void testConstructorsDestructorsAndOperators() {
    const std::string source =
        "namespace demo {\n"
        "class Engine {\n"
        "public:\n"
        "  Engine();\n"
        "  explicit Engine(int value);\n"
        "  ~Engine();\n"
        "  bool operator==(const Engine& other) const;\n"
        "  int operator()(int a, double b = 2.0) const;\n"
        "  operator bool() const;\n"
        "  Engine& operator=(const Engine& other);\n"
        "  int value;\n"
        "};\n"
        "Engine::Engine() {}\n"
        "Engine::Engine(int value) {}\n"
        "Engine::~Engine() {}\n"
        "bool Engine::operator==(const Engine& other) const { return value == other.value; }\n"
        "int Engine::operator()(int a, double b) const { return a; }\n"
        "Engine::operator bool() const { return value != 0; }\n"
        "Engine& Engine::operator=(const Engine& other) { return *this; }\n"
        "bool operator==(const Engine& left, const Engine& right);\n"
        "}\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "operators.cpp");
    ROCLINT_CHECK(result.parseOk);
    checkInvariants(source, result.symbols);

    const auto* engine = findRole(result.symbols, "demo::Engine", AstSymbolRole::Type);
    const auto* constructor = findByKeySite(result.symbols, "demo::Engine::Engine",
                                            AstSymbolRole::Constructor, "demo::Engine::Engine()", false);
    const auto* explicitConstructor = findByKeySite(result.symbols, "demo::Engine::Engine",
                                                    AstSymbolRole::Constructor,
                                                    "demo::Engine::Engine(int)", false);
    const auto* destructor = findRole(result.symbols, "demo::Engine::~Engine", AstSymbolRole::Destructor);
    const auto* equality = findByKeySite(result.symbols, "demo::Engine::operator==",
                                         AstSymbolRole::Operator,
                                         "demo::Engine::operator==(const Engine&) const", false);
    const auto* call = findByKeySite(result.symbols, "demo::Engine::operator()", AstSymbolRole::Operator,
                                     "demo::Engine::operator()(int,double) const", false);
    const auto* conversion = findByKeySite(result.symbols, "demo::Engine::operator bool",
                                           AstSymbolRole::Operator,
                                           "demo::Engine::operator bool() const", false);
    const auto* assignment = findByKeySite(result.symbols, "demo::Engine::operator=",
                                           AstSymbolRole::Operator,
                                           "demo::Engine::operator=(const Engine&)", false);
    ROCLINT_CHECK(engine && constructor && explicitConstructor && destructor && equality && call &&
                  conversion && assignment);
    ROCLINT_CHECK(constructor->kind == AstNodeKind::Method);
    ROCLINT_CHECK(constructor->signatureKey == "demo::Engine::Engine()");
    ROCLINT_CHECK(explicitConstructor->signatureKey == "demo::Engine::Engine(int)");
    ROCLINT_CHECK(destructor->kind == AstNodeKind::Method);
    ROCLINT_CHECK(destructor->signatureKey == "demo::Engine::~Engine()");
    ROCLINT_CHECK(equality->signatureKey == "demo::Engine::operator==(const Engine&) const");
    ROCLINT_CHECK(call->signatureKey == "demo::Engine::operator()(int,double) const");
    ROCLINT_CHECK(conversion->signatureKey == "demo::Engine::operator bool() const");
    ROCLINT_CHECK(conversion->name == "operator bool");
    ROCLINT_CHECK(conversion->kind == AstNodeKind::Method);
    ROCLINT_CHECK(conversion->isConst);
    ROCLINT_CHECK(assignment->signatureKey == "demo::Engine::operator=(const Engine&)");
    ROCLINT_CHECK(findRole(result.symbols, "demo::Engine::operator bool", AstSymbolRole::Field) ==
                  nullptr);

    const auto* equalityDefinition =
        findByKeySite(result.symbols, "demo::Engine::operator==", AstSymbolRole::Operator,
                      "demo::Engine::operator==(const Engine&) const", true);
    const auto* callDefinition =
        findByKeySite(result.symbols, "demo::Engine::operator()", AstSymbolRole::Operator,
                      "demo::Engine::operator()(int,double) const", true);
    const auto* conversionDefinition =
        findByKeySite(result.symbols, "demo::Engine::operator bool", AstSymbolRole::Operator,
                      "demo::Engine::operator bool() const", true);
    const auto* assignmentDefinition =
        findByKeySite(result.symbols, "demo::Engine::operator=", AstSymbolRole::Operator,
                      "demo::Engine::operator=(const Engine&)", true);
    const auto* constructorDefinition =
        findByKeySite(result.symbols, "demo::Engine::Engine", AstSymbolRole::Constructor,
                      "demo::Engine::Engine()", true);
    const auto* explicitConstructorDefinition =
        findByKeySite(result.symbols, "demo::Engine::Engine", AstSymbolRole::Constructor,
                      "demo::Engine::Engine(int)", true);
    const auto* destructorDefinition =
        findSite(result.symbols, "demo::Engine::~Engine", AstSymbolRole::Destructor, true);
    ROCLINT_CHECK(equalityDefinition && callDefinition && conversionDefinition && assignmentDefinition &&
                  constructorDefinition && explicitConstructorDefinition && destructorDefinition);
    ROCLINT_CHECK(equalityDefinition->signatureKey == equality->signatureKey);
    ROCLINT_CHECK(callDefinition->signatureKey == call->signatureKey);
    ROCLINT_CHECK(conversionDefinition->signatureKey == conversion->signatureKey);
    ROCLINT_CHECK(assignmentDefinition->signatureKey == assignment->signatureKey);
    ROCLINT_CHECK(constructorDefinition->signatureKey == constructor->signatureKey);
    ROCLINT_CHECK(explicitConstructorDefinition->signatureKey == explicitConstructor->signatureKey);
    ROCLINT_CHECK(destructorDefinition->signatureKey == destructor->signatureKey);
    for (const auto* symbol : {equalityDefinition, callDefinition, conversionDefinition,
                               assignmentDefinition, constructorDefinition,
                               explicitConstructorDefinition, destructorDefinition}) {
        ROCLINT_CHECK(symbol->isDefinition && !symbol->isDeclaration);
        ROCLINT_CHECK(symbol->parentSymbolId == engine->symbolId);
        ROCLINT_CHECK(symbol->signature.find("return") == std::string::npos);
    }

    const auto callParameters = findAllQualified(result.symbols, "demo::Engine::operator()::a");
    const auto bParameters = findAllQualified(result.symbols, "demo::Engine::operator()::b");
    ROCLINT_CHECK(callParameters.size() == 2 && bParameters.size() == 2);
    for (const auto* parameter : callParameters) ROCLINT_CHECK(parameter->parameterIndex == 1);
    for (const auto* parameter : bParameters) ROCLINT_CHECK(parameter->parameterIndex == 2);
    const auto equalityParameters = findAllQualified(result.symbols, "demo::Engine::operator==::other");
    ROCLINT_CHECK(equalityParameters.size() == 2);
    for (const auto* parameter : equalityParameters) {
        ROCLINT_CHECK(parameter->isReference);
        ROCLINT_CHECK(parameter->isConst);
        ROCLINT_CHECK(parameter->parameterIndex == 1);
    }
    ROCLINT_CHECK(callDefinition->signatureKey.find('=') == std::string::npos);

    const auto* freeEquality = findRole(result.symbols, "demo::operator==", AstSymbolRole::Operator);
    const auto* demoNamespace = findRole(result.symbols, "demo", AstSymbolRole::Namespace);
    ROCLINT_CHECK(freeEquality != nullptr && demoNamespace != nullptr);
    ROCLINT_CHECK(freeEquality->kind == AstNodeKind::Function);
    ROCLINT_CHECK(freeEquality->signatureKey ==
                  "demo::operator==(const Engine&,const Engine&)");
    ROCLINT_CHECK(freeEquality->parentSymbolId == demoNamespace->symbolId);
}

void testTemplatesAndFunctionPointers() {
    const std::string source =
        "namespace demo {\n"
        "template <typename T>\n"
        "T clampValue(T value) { return value; }\n"
        "template <typename T>\n"
        "struct Box {\n"
        "  T item;\n"
        "  T get(T key) const;\n"
        "};\n"
        "template <typename T>\n"
        "T Box<T>::get(T key) const { return key; }\n"
        "template <typename T>\n"
        "using Ptr = T*;\n"
        "using Handler = void (*)(int);\n"
        "void (*handler)(int);\n"
        "class Owner {\n"
        "public:\n"
        "  using Callback = bool (*)(int);\n"
        "  void (*memberHandler)(double);\n"
        "  template <typename T>\n"
        "  T get(T key) const;\n"
        "};\n"
        "int (*factory())(int);\n"
        "int& referenceReturn();\n"
        "}\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "templates.cpp");
    ROCLINT_CHECK(result.parseOk);
    checkInvariants(source, result.symbols);

    const auto* clamp = findRole(result.symbols, "demo::clampValue", AstSymbolRole::Function);
    const auto* box = findRole(result.symbols, "demo::Box", AstSymbolRole::Type);
    const auto* item = findRole(result.symbols, "demo::Box::item", AstSymbolRole::Field);
    const auto* boxGetDeclaration = findByKeySite(result.symbols, "demo::Box::get", AstSymbolRole::Method,
                                                  "demo::Box::get(T) const", false);
    const auto* boxGetDefinition = findByKeySite(result.symbols, "demo::Box::get", AstSymbolRole::Method,
                                                 "demo::Box::get(T) const", true);
    const auto* pointerAlias = findRole(result.symbols, "demo::Ptr", AstSymbolRole::TypeAlias);
    const auto* handlerAlias = findRole(result.symbols, "demo::Handler", AstSymbolRole::TypeAlias);
    const auto* handler = findRole(result.symbols, "demo::handler", AstSymbolRole::Variable);
    const auto* callback = findRole(result.symbols, "demo::Owner::Callback", AstSymbolRole::TypeAlias);
    const auto* memberHandler = findRole(result.symbols, "demo::Owner::memberHandler", AstSymbolRole::Field);
    const auto* get = findRole(result.symbols, "demo::Owner::get", AstSymbolRole::Method);
    const auto* referenceReturn = findRole(result.symbols, "demo::referenceReturn", AstSymbolRole::Function);
    const auto* owner = findRole(result.symbols, "demo::Owner", AstSymbolRole::Type);
    ROCLINT_CHECK(clamp && box && item && boxGetDeclaration && boxGetDefinition && pointerAlias &&
                  handlerAlias && handler && callback && memberHandler && get && referenceReturn && owner);
    ROCLINT_CHECK(clamp->isTemplate);
    ROCLINT_CHECK(clamp->signatureKey == "demo::clampValue(T)");
    ROCLINT_CHECK(clamp->signature.find("return") == std::string::npos);
    ROCLINT_CHECK(box->kind == AstNodeKind::Struct && box->isTemplate);
    ROCLINT_CHECK(item->kind == AstNodeKind::Field);
    ROCLINT_CHECK(boxGetDeclaration->parentSymbolId == box->symbolId);
    ROCLINT_CHECK(boxGetDefinition->parentSymbolId == box->symbolId);
    ROCLINT_CHECK(boxGetDeclaration->signatureKey == boxGetDefinition->signatureKey);
    ROCLINT_CHECK(boxGetDefinition->isTemplate);
    ROCLINT_CHECK(boxGetDefinition->signature.find("return") == std::string::npos);
    ROCLINT_CHECK(findAllQualified(result.symbols, "demo::Box::get::key").size() == 2);
    ROCLINT_CHECK(pointerAlias->kind == AstNodeKind::TypeAlias && pointerAlias->isTemplate);
    ROCLINT_CHECK(pointerAlias->signature.find("using Ptr = T*;") != std::string::npos);
    ROCLINT_CHECK(pointerAlias->signatureKey.empty());
    ROCLINT_CHECK(handlerAlias->kind == AstNodeKind::TypeAlias);
    ROCLINT_CHECK(handlerAlias->signature.find("void (*)(int)") != std::string::npos);
    ROCLINT_CHECK(handlerAlias->signatureKey.empty());
    ROCLINT_CHECK(handlerAlias->isDeclaration && !handlerAlias->isDefinition);
    ROCLINT_CHECK(handler->kind == AstNodeKind::Unknown);
    ROCLINT_CHECK(handler->signature.find("(*handler)(int)") != std::string::npos);
    ROCLINT_CHECK(handler->signatureKey.empty());
    ROCLINT_CHECK(!hasParameterPrefix(result.symbols, "demo::handler::"));
    ROCLINT_CHECK(!hasParameterPrefix(result.symbols, "demo::Handler::"));
    ROCLINT_CHECK(callback->kind == AstNodeKind::TypeAlias);
    ROCLINT_CHECK(callback->parentSymbolId == owner->symbolId);
    ROCLINT_CHECK(memberHandler->kind == AstNodeKind::Field);
    ROCLINT_CHECK(memberHandler->signature.find("(*memberHandler)(double)") != std::string::npos);
    ROCLINT_CHECK(!hasParameterPrefix(result.symbols, "demo::Owner::memberHandler::"));
    ROCLINT_CHECK(get->isTemplate);
    ROCLINT_CHECK(get->signatureKey == "demo::Owner::get(T) const");
    ROCLINT_CHECK(get->isConst);
    ROCLINT_CHECK(referenceReturn->kind == AstNodeKind::Function);
    ROCLINT_CHECK(referenceReturn->signature.find("int&") != std::string::npos);
    ROCLINT_CHECK(referenceReturn->signatureKey == "demo::referenceReturn()");
    ROCLINT_CHECK(referenceReturn->isReference);
    ROCLINT_CHECK(std::none_of(result.symbols.begin(), result.symbols.end(), [](const AstSymbol& symbol) {
        return symbol.name == "factory";
    }));
    ROCLINT_CHECK(findRole(result.symbols, "demo::clampValue::value", AstSymbolRole::Parameter) !=
                  nullptr);
    ROCLINT_CHECK(findRole(result.symbols, "demo::Owner::get::key", AstSymbolRole::Parameter) !=
                  nullptr);
}

void testVariadicQualifiersAndOverloadKeys() {
    const std::string source =
        "namespace demo {\n"
        "struct Base {\n"
        "  void lvalue() &;\n"
        "  void rvalue() &&;\n"
        "  void noThrow() noexcept;\n"
        "  void throwing() noexcept(false);\n"
        "  void constVolatile() const volatile;\n"
        "  static int make(int count, ...);\n"
        "  inline constexpr int compute(int value) const;\n"
        "  template <typename... Args>\n"
        "  void pack(Args&&... args);\n"
        "};\n"
        "int freeVariadic(int count, ...);\n"
        "inline void inlineFree();\n"
        "constexpr int constexprFree(int value);\n"
        "struct Overloads {\n"
        "  void apply(int value);\n"
        "  void apply(long value);\n"
        "  void apply(int first, int second);\n"
        "  void apply(int value) const;\n"
        "  void apply(int value) &;\n"
        "  void apply(int value) noexcept;\n"
        "};\n"
        "void messy(int first, const char* second = nullptr);\n"
        "void messy(int, const char*) {}\n"
        "}\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "qualifiers.cpp");
    ROCLINT_CHECK(result.parseOk);
    checkInvariants(source, result.symbols);

    const auto* lvalue = findRole(result.symbols, "demo::Base::lvalue", AstSymbolRole::Method);
    const auto* rvalue = findRole(result.symbols, "demo::Base::rvalue", AstSymbolRole::Method);
    const auto* noThrow = findRole(result.symbols, "demo::Base::noThrow", AstSymbolRole::Method);
    const auto* throwing = findRole(result.symbols, "demo::Base::throwing", AstSymbolRole::Method);
    const auto* constVolatile =
        findRole(result.symbols, "demo::Base::constVolatile", AstSymbolRole::Method);
    const auto* make = findRole(result.symbols, "demo::Base::make", AstSymbolRole::Method);
    const auto* compute = findRole(result.symbols, "demo::Base::compute", AstSymbolRole::Method);
    const auto* pack = findRole(result.symbols, "demo::Base::pack", AstSymbolRole::Method);
    const auto* freeVariadic = findRole(result.symbols, "demo::freeVariadic", AstSymbolRole::Function);
    const auto* inlineFree = findRole(result.symbols, "demo::inlineFree", AstSymbolRole::Function);
    const auto* constexprFree = findRole(result.symbols, "demo::constexprFree", AstSymbolRole::Function);
    ROCLINT_CHECK(lvalue && rvalue && noThrow && throwing && constVolatile && make && compute && pack &&
                  freeVariadic && inlineFree && constexprFree);
    ROCLINT_CHECK(lvalue->qualifiers == "&" && lvalue->signatureKey == "demo::Base::lvalue() &");
    ROCLINT_CHECK(rvalue->qualifiers == "&&" && rvalue->signatureKey == "demo::Base::rvalue() &&");
    ROCLINT_CHECK(noThrow->qualifiers == "noexcept");
    ROCLINT_CHECK(throwing->qualifiers == "noexcept(false)");
    ROCLINT_CHECK(throwing->signatureKey == "demo::Base::throwing() noexcept(false)");
    ROCLINT_CHECK(constVolatile->isConst);
    ROCLINT_CHECK(constVolatile->qualifiers == "const volatile");
    ROCLINT_CHECK(make->isStatic && make->isVariadic);
    ROCLINT_CHECK(make->signatureKey == "demo::Base::make(int,...)");
    ROCLINT_CHECK(compute->isInline && compute->isConstexpr && compute->isConst);
    ROCLINT_CHECK(compute->qualifiers == "const");
    ROCLINT_CHECK(pack->isTemplate && pack->isVariadic);
    ROCLINT_CHECK(pack->signatureKey == "demo::Base::pack(Args&&...)");
    ROCLINT_CHECK(freeVariadic->isVariadic);
    ROCLINT_CHECK(freeVariadic->signatureKey == "demo::freeVariadic(int,...)");
    ROCLINT_CHECK(inlineFree->isInline);
    ROCLINT_CHECK(constexprFree->isConstexpr);
    const auto* makeCount = findRole(result.symbols, "demo::Base::make::count", AstSymbolRole::Parameter);
    const auto* packArgs = findRole(result.symbols, "demo::Base::pack::args", AstSymbolRole::Parameter);
    const auto* freeCount =
        findRole(result.symbols, "demo::freeVariadic::count", AstSymbolRole::Parameter);
    ROCLINT_CHECK(makeCount && packArgs && freeCount);
    ROCLINT_CHECK(makeCount->parameterIndex == 1);
    ROCLINT_CHECK(packArgs->isReference);
    ROCLINT_CHECK(packArgs->isVariadic);
    ROCLINT_CHECK(freeCount->parameterIndex == 1);

    const auto keys = signatureKeys(result.symbols, "demo::Overloads::apply");
    ROCLINT_CHECK(keys.size() == 6);
    ROCLINT_CHECK(keys.contains("demo::Overloads::apply(int)"));
    ROCLINT_CHECK(keys.contains("demo::Overloads::apply(long)"));
    ROCLINT_CHECK(keys.contains("demo::Overloads::apply(int,int)"));
    ROCLINT_CHECK(keys.contains("demo::Overloads::apply(int) const"));
    ROCLINT_CHECK(keys.contains("demo::Overloads::apply(int) &"));
    ROCLINT_CHECK(keys.contains("demo::Overloads::apply(int) noexcept"));

    const auto messy = findAllQualified(result.symbols, "demo::messy");
    ROCLINT_CHECK(messy.size() == 2);
    ROCLINT_CHECK(messy[0]->signatureKey == messy[1]->signatureKey);
    ROCLINT_CHECK(messy[0]->signatureKey == "demo::messy(int,const char*)");
    ROCLINT_CHECK(messy[0]->signatureKey.find('=') == std::string::npos);
    ROCLINT_CHECK((messy[0]->isDeclaration && messy[1]->isDefinition) ||
                  (messy[1]->isDeclaration && messy[0]->isDefinition));
}

void testPartialRecoveryPreservesDeclaration() {
    const std::string source =
        "namespace demo {\n"
        "void partial(int n) { int local = ; }\n"
        "int cleanAfter;\n"
        "class Holder {\n"
        "public:\n"
        "  void broken(int n) { int local = ; }\n"
        "  int healthy;\n"
        "};\n"
        "void badParam(int n, );\n"
        "int afterBadParam;\n"
        "void okParam(int n);\n"
        "int afterOkParam;\n"
        "}\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "recovered.cpp");
    ROCLINT_CHECK(!result.parseOk);
    ROCLINT_CHECK(result.recovered);
    checkInvariants(source, result.symbols);
    const auto again = AstSymbolExtractor::extractSymbols(source, "recovered.cpp");
    ROCLINT_CHECK(result.symbols == again.symbols);

    const auto* partial = findRole(result.symbols, "demo::partial", AstSymbolRole::Function);
    const auto* cleanAfter = findRole(result.symbols, "demo::cleanAfter", AstSymbolRole::Variable);
    const auto* holder = findRole(result.symbols, "demo::Holder", AstSymbolRole::Type);
    const auto* broken = findRole(result.symbols, "demo::Holder::broken", AstSymbolRole::Method);
    const auto* healthy = findRole(result.symbols, "demo::Holder::healthy", AstSymbolRole::Field);
    const auto* okParam = findRole(result.symbols, "demo::okParam", AstSymbolRole::Function);
    const auto* afterBadParam =
        findRole(result.symbols, "demo::afterBadParam", AstSymbolRole::Variable);
    const auto* afterOkParam = findRole(result.symbols, "demo::afterOkParam", AstSymbolRole::Variable);
    ROCLINT_CHECK(partial && cleanAfter && holder && broken && healthy && okParam && afterBadParam &&
                  afterOkParam);
    ROCLINT_CHECK(partial->isDeclaration && !partial->isDefinition);
    ROCLINT_CHECK(partial->signatureKey == "demo::partial(int)");
    ROCLINT_CHECK(partial->signature.find("local") == std::string::npos);
    ROCLINT_CHECK(partial->signature.find('=') == std::string::npos);
    ROCLINT_CHECK(spanText(source, *partial).find("local") == std::string::npos);
    ROCLINT_CHECK(broken->isDeclaration && !broken->isDefinition);
    ROCLINT_CHECK(broken->signatureKey == "demo::Holder::broken(int)");
    ROCLINT_CHECK(broken->signature.find("local") == std::string::npos);
    ROCLINT_CHECK(healthy->isDeclaration && !healthy->isDefinition);
    ROCLINT_CHECK(cleanAfter->isDefinition && !cleanAfter->isDeclaration);
    ROCLINT_CHECK(afterBadParam->isDefinition && afterOkParam->isDefinition);
    ROCLINT_CHECK(okParam->isDeclaration && !okParam->isDefinition);
    ROCLINT_CHECK(findRole(result.symbols, "demo::partial::n", AstSymbolRole::Parameter) != nullptr);
    ROCLINT_CHECK(findRole(result.symbols, "demo::Holder::broken::n", AstSymbolRole::Parameter) !=
                  nullptr);
    ROCLINT_CHECK(std::none_of(result.symbols.begin(), result.symbols.end(), [](const AstSymbol& symbol) {
        return symbol.name == "local" || symbol.name == "badParam";
    }));
    ROCLINT_CHECK(std::none_of(result.symbols.begin(), result.symbols.end(), [](const AstSymbol& symbol) {
        return symbol.qualifiedName.find("::local") != std::string::npos;
    }));
}

void testMacroGeneratedDeclarationsAreOmitted() {
    const std::string source =
        "#define DECLARE_FUNCTION(name) void name(int)\n"
        "DECLARE_FUNCTION(generated);\n"
        "#define FIELD(type, name) type name;\n"
        "class MacroHolder {\n"
        "  FIELD(int, generatedField)\n"
        "  int real;\n"
        "};\n"
        "int clean;\n";
    const auto result = AstSymbolExtractor::extractSymbols(source, "macros.cpp");
    ROCLINT_CHECK(!result.parseOk);
    ROCLINT_CHECK(result.recovered);
    checkInvariants(source, result.symbols);

    const auto* holder = findRole(result.symbols, "MacroHolder", AstSymbolRole::Type);
    const auto* real = findRole(result.symbols, "MacroHolder::real", AstSymbolRole::Field);
    const auto* clean = findRole(result.symbols, "clean", AstSymbolRole::Variable);
    ROCLINT_CHECK(holder && real && clean);
    for (const auto& symbol : result.symbols) {
        ROCLINT_CHECK(symbol.name != "DECLARE_FUNCTION");
        ROCLINT_CHECK(symbol.name != "generated");
        ROCLINT_CHECK(symbol.name != "FIELD");
        ROCLINT_CHECK(symbol.name != "generatedField");
    }
    ROCLINT_CHECK(!hasParameterPrefix(result.symbols, "MacroHolder::generatedField::"));
    ROCLINT_CHECK(holder->role == AstSymbolRole::Type);
    ROCLINT_CHECK(real->role == AstSymbolRole::Field);
}

// AST extraction, hierarchy, recovery, signatures and byte spans.
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
    const auto* mode = findSymbol(result.symbols, "demo::SessionManager::Mode", AstNodeKind::Enum);
    const auto* config = findSymbol(result.symbols, "demo::SessionManager::Config", AstNodeKind::Struct);
    const auto* method = findSymbol(result.symbols, "demo::SessionManager::readPacket", AstNodeKind::Method);
    const auto* offset = findSymbol(result.symbols, "demo::SessionManager::readPacket::offset",
                                    AstNodeKind::Parameter);
    const auto* flags = findSymbol(result.symbols, "demo::SessionManager::readPacket::flags",
                                   AstNodeKind::Parameter);
    const auto* timeout = findSymbol(result.symbols, "demo::SessionManager::timeout", AstNodeKind::Field);
    const auto* freeFn = findSymbol(result.symbols, "demo::freeFunction", AstNodeKind::Function);
    ROCLINT_CHECK(ns && cls && mode && config && method && offset && flags && timeout && freeFn);
    ROCLINT_CHECK(mode->parentSymbolId == cls->symbolId);
    ROCLINT_CHECK(config->parentSymbolId == cls->symbolId);
    ROCLINT_CHECK(cls->parentSymbolId == ns->symbolId);
    ROCLINT_CHECK(method->parentSymbolId == cls->symbolId);
    ROCLINT_CHECK(offset->parentSymbolId == method->symbolId);
    ROCLINT_CHECK(flags->parentSymbolId == method->symbolId);
    ROCLINT_CHECK(timeout->parentSymbolId == cls->symbolId);
    ROCLINT_CHECK(offset->typeName == "size_t");
    ROCLINT_CHECK(flags->typeName == "uint32_t");
    ROCLINT_CHECK(offset->parameterIndex == 1);
    ROCLINT_CHECK(flags->parameterIndex == 2);
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

// Existing macro-recovery regression: Qt macro invocations must never become symbols.
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

// Existing malformed-source regression: recovered output must have valid spans.
void testParserRejectsMalformedSource() {
    const auto result = AstSymbolExtractor::extractSymbols("class Broken { void f( {", "broken.cpp");
    ROCLINT_CHECK(!result.parseOk);
    ROCLINT_CHECK(result.recovered);
    for (const auto& symbol : result.symbols)
        ROCLINT_CHECK(roclint::core::analysis::isValidSpan("class Broken { void f( {", symbol.span));
}

// Existing body-free bounded-signature regression.
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

// Existing latency gate: richer extraction must stay bounded.
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
    testSharedSymbolModelRolesAndKindCompatibility();
    testGlobalAndNamespaceVariableDeclarators();
    testAnonymousNamespaceFileLocalIdentity();
    testOutOfClassMethodDefinitions();
    testConstructorsDestructorsAndOperators();
    testTemplatesAndFunctionPointers();
    testVariadicQualifiersAndOverloadKeys();
    testPartialRecoveryPreservesDeclaration();
    testMacroGeneratedDeclarationsAreOmitted();
    testHierarchicalCppExtraction();
    testParserRecoversMacroHeavyHeader();
    testParserRejectsMalformedSource();
    testBodyFreeBoundedSignatures();
    testExtractionLatency();
    std::cout << "AST symbol extraction, shared roles, overload signatures, recovery, byte spans and "
                 "body-free signatures passed.\n";
    return 0;
}
