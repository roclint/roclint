#include "roclint/storage/AstSymbolExtractor.hpp"

#include "roclint/core/crypto/Sha256.hpp"

#include <tree_sitter/api.h>
#include <tree-sitter-cpp.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace roclint::storage {
namespace {

constexpr std::size_t kMaximumSignatureBytes = 1200;
constexpr std::size_t kMaximumSignatureKeyBytes = 1200;
constexpr std::size_t kMaximumTypeSyntaxBytes = 512;
constexpr std::size_t kMaximumQualifierBytes = 200;
constexpr const char* kAnonymousNamespaceName = "(anonymous namespace)";

struct Scope {
    std::string qualifiedName{};
    std::string parentSymbolId{};
    bool classScope{false};
};

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

bool isWordCharacter(char character) {
    const auto code = static_cast<unsigned char>(character);
    return std::isalnum(code) != 0 || character == '_' || code >= 0x80;
}

bool containsWord(std::string_view text, std::string_view word) {
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && !isWordCharacter(text[index])) ++index;
        const auto begin = index;
        while (index < text.size() && isWordCharacter(text[index])) ++index;
        if (text.substr(begin, index - begin) == word) return true;
    }
    return false;
}

// Collapses whitespace and removes insignificant spaces around punctuation.
// It deliberately does not tokenize literals: this operates on declaration
// syntax, never on implementation bodies.
std::string normalizeSyntax(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool pendingSpace = false;
    for (const char character : text) {
        if (std::isspace(static_cast<unsigned char>(character))) {
            if (!normalized.empty()) pendingSpace = true;
            continue;
        }
        if (pendingSpace) {
            const char previous = normalized.back();
            const bool previousWord = isWordCharacter(previous);
            const bool nextWord = isWordCharacter(character);
            const bool separatesWords = previousWord && nextWord;
            const bool separatesTypeQualifier =
                nextWord && (previous == '*' || previous == '&' || previous == '>' || previous == ')' ||
                             previous == ']');
            if (separatesWords || separatesTypeQualifier) normalized.push_back(' ');
            pendingSpace = false;
        }
        normalized.push_back(character);
    }
    return normalized;
}

std::string boundedText(std::string text, std::size_t maximumBytes) {
    text = trim(text);
    if (text.size() > maximumBytes) {
        auto cut = maximumBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
        text = trim(text);
    }
    return text;
}

std::string boundedSignature(std::string text) {
    return boundedText(std::move(text), kMaximumSignatureBytes);
}

bool validNode(TSNode node) {
    return !ts_node_is_null(node) && !ts_node_is_missing(node) && ts_node_type(node) != nullptr;
}

std::string_view nodeType(TSNode node) {
    const auto* type = ts_node_type(node);
    return type != nullptr ? std::string_view(type) : std::string_view{};
}

std::string nodeText(TSNode node, std::string_view source) {
    if (!validNode(node)) return {};
    const auto begin = ts_node_start_byte(node);
    const auto end = ts_node_end_byte(node);
    if (end < begin || end > source.size()) return {};
    return std::string(source.substr(begin, end - begin));
}

TSNode field(TSNode node, const char* name) {
    if (!validNode(node)) return {};
    return ts_node_child_by_field_name(node, name,
                                       static_cast<std::uint32_t>(std::char_traits<char>::length(name)));
}

TSNode findChildType(TSNode node, std::string_view type) {
    if (!validNode(node)) return {};
    for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
        const auto child = ts_node_named_child(node, index);
        if (validNode(child) && nodeType(child) == type) return child;
    }
    return {};
}

TSNode findTypeNode(TSNode node, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return {};
    const auto type = nodeType(node);
    if (type == "type_identifier" || type == "primitive_type" || type == "sized_type_specifier" ||
        type == "qualified_identifier" || type == "template_type" || type == "decltype" ||
        type == "placeholder_type_specifier") return node;
    for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
        const auto found = findTypeNode(ts_node_named_child(node, index), depth + 1);
        if (validNode(found)) return found;
    }
    return {};
}

std::string makeSymbolId(std::string_view filePath, std::string_view qualifiedName, std::size_t begin,
                         std::size_t end) {
    const auto digest = core::crypto::Sha256::hexDigest(
        std::string(filePath) + "|" + std::string(qualifiedName) + "|" + std::to_string(begin) + ":" +
        std::to_string(end));
    return "sym_" + digest.substr(0, 24);
}

std::string lastSegment(std::string_view qualified) {
    const auto separator = qualified.rfind("::");
    return separator == std::string_view::npos ? std::string(qualified)
                                               : std::string(qualified.substr(separator + 2));
}

std::string qualify(std::string_view scope, std::string_view name) {
    std::string_view localName = name;
    if (localName.starts_with("::")) return std::string(localName.substr(2));
    if (localName.empty()) return std::string(scope);
    if (scope.empty()) return std::string(localName);
    if (localName.size() > scope.size() && localName.substr(0, scope.size()) == scope &&
        localName.substr(scope.size(), 2) == "::") {
        return std::string(localName);
    }
    return std::string(scope) + "::" + std::string(localName);
}

bool looksLikeOperator(std::string_view leaf) {
    if (leaf == "operator") return true;
    if (!leaf.starts_with("operator")) return false;
    if (leaf.size() == 8) return true;
    return !isWordCharacter(leaf[8]);
}

bool isNameNodeType(std::string_view type) {
    return type == "identifier" || type == "field_identifier" || type == "type_identifier" ||
           type == "qualified_identifier" || type == "namespace_identifier" || type == "operator_name" ||
           type == "operator_cast" || type == "destructor_name" || type == "template_function" ||
           type == "template_method" || type == "template_type" || type == "nested_namespace_specifier" ||
           type == "dependent_name";
}

bool isDeclaratorNodeType(std::string_view type) {
    return isNameNodeType(type) || type == "pointer_declarator" || type == "reference_declarator" ||
           type == "array_declarator" || type == "parenthesized_declarator" ||
           type == "attributed_declarator" || type == "function_declarator" ||
           type == "variadic_declarator" || type == "abstract_pointer_declarator" ||
           type == "abstract_reference_declarator" || type == "abstract_array_declarator" ||
           type == "abstract_parenthesized_declarator" || type == "abstract_function_declarator" ||
           type == "structured_binding_declarator";
}

TSNode firstDeclaratorChild(TSNode node) {
    if (!validNode(node)) return {};
    const auto type = nodeType(node);
    if (type == "init_declarator" || type == "function_declarator" || type == "abstract_function_declarator" ||
        type == "pointer_declarator" || type == "abstract_pointer_declarator" || type == "array_declarator" ||
        type == "abstract_array_declarator") {
        return field(node, "declarator");
    }
    if (type == "parenthesized_declarator" || type == "abstract_parenthesized_declarator" ||
        type == "reference_declarator" || type == "abstract_reference_declarator" ||
        type == "attributed_declarator") {
        for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
            const auto child = ts_node_named_child(node, index);
            if (validNode(child) && isDeclaratorNodeType(nodeType(child))) return child;
        }
        return {};
    }
    if (type == "variadic_declarator") {
        for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
            const auto child = ts_node_named_child(node, index);
            if (validNode(child) && (nodeType(child) == "identifier" || nodeType(child) == "field_identifier"))
                return child;
        }
    }
    return {};
}

std::string nameTextFor(TSNode node, std::string_view source, std::uint32_t depth = 0);
std::string signatureFor(TSNode node, std::string_view source, bool excludeInitializers);

std::string leafNameText(TSNode node, std::string_view source) {
    if (!validNode(node)) return {};
    const auto type = nodeType(node);
    if (type == "qualified_identifier") {
        const auto name = field(node, "name");
        if (validNode(name)) return leafNameText(name, source);
    }
    if (type == "template_function" || type == "template_method" || type == "template_type") {
        const auto name = field(node, "name");
        if (validNode(name)) return leafNameText(name, source);
    }
    return nameTextFor(node, source);
}

std::string nameTextFor(TSNode node, std::string_view source, std::uint32_t depth) {
    if (!validNode(node) || depth > 64) return {};
    const auto type = nodeType(node);
    if (type == "qualified_identifier") {
        const auto name = field(node, "name");
        if (validNode(name)) {
            const auto scope = field(node, "scope");
            const auto scopeText = validNode(scope) ? nameTextFor(scope, source, depth + 1) : std::string{};
            const auto nameText = nameTextFor(name, source, depth + 1);
            if (scopeText.empty()) return nameText;
            if (nameText.empty()) return scopeText;
            return scopeText + "::" + nameText;
        }
    }
    if (type == "operator_cast") {
        const auto castType = trim(signatureFor(field(node, "type"), source, false));
        return castType.empty() ? std::string("operator") : "operator " + castType;
    }
    if (type == "template_function" || type == "template_method" || type == "template_type") {
        const auto name = field(node, "name");
        if (validNode(name)) return nameTextFor(name, source, depth + 1);
    }
    return trim(nodeText(node, source));
}

TSNode findDeclaredNameNode(TSNode node, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return {};
    if (isNameNodeType(nodeType(node))) return node;
    const auto child = firstDeclaratorChild(node);
    if (validNode(child)) return findDeclaredNameNode(child, depth + 1);
    return {};
}

struct DeclaredName {
    TSNode node{};
    std::string text{};
    std::string leaf{};
};

DeclaredName declaredName(TSNode declarator, std::string_view source) {
    const auto node = findDeclaredNameNode(declarator);
    if (!validNode(node)) return {};
    return DeclaredName{node, nameTextFor(node, source), leafNameText(node, source)};
}

std::string explicitScopeText(TSNode nameNode, std::string_view source) {
    if (!validNode(nameNode) || nodeType(nameNode) != "qualified_identifier") return {};
    const auto scope = field(nameNode, "scope");
    return validNode(scope) ? nameTextFor(scope, source) : std::string{};
}

TSNode findOperatorCast(TSNode node, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return {};
    const auto type = nodeType(node);
    if (type == "operator_cast") return node;
    if (type == "qualified_identifier") return findOperatorCast(field(node, "name"), depth + 1);
    return {};
}

bool containsAnyType(TSNode node, std::initializer_list<std::string_view> types, std::uint32_t depth = 0,
                     bool skipParameterLists = true) {
    if (!validNode(node) || depth > 64) return false;
    const auto current = nodeType(node);
    for (const auto type : types) {
        if (current == type) return true;
    }
    if (skipParameterLists && (current == "parameter_list" || current == "template_argument_list")) return false;
    for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
        if (containsAnyType(ts_node_named_child(node, index), types, depth + 1, skipParameterLists)) return true;
    }
    return false;
}

bool hasFunctionDeclaratorBelow(TSNode node, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return false;
    const auto type = nodeType(node);
    if (type == "function_declarator" || type == "abstract_function_declarator") return true;
    if (type == "parameter_list" || type == "template_argument_list") return false;
    for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
        if (hasFunctionDeclaratorBelow(ts_node_named_child(node, index), depth + 1)) return true;
    }
    return false;
}

bool hasParenthesizedPointer(TSNode node, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return false;
    const auto type = nodeType(node);
    if (type == "parenthesized_declarator" || type == "abstract_parenthesized_declarator") {
        return containsAnyType(node,
                               {"pointer_declarator", "reference_declarator", "abstract_pointer_declarator",
                                "abstract_reference_declarator"},
                               depth + 1, true);
    }
    const auto child = firstDeclaratorChild(node);
    if (validNode(child)) return hasParenthesizedPointer(child, depth + 1);
    return false;
}

TSNode findOutermostFunctionDeclarator(TSNode node, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return {};
    const auto type = nodeType(node);
    if (type == "function_declarator" || type == "abstract_function_declarator") return node;
    if (type == "operator_cast") return findOutermostFunctionDeclarator(field(node, "declarator"), depth + 1);
    if (type == "qualified_identifier") return findOutermostFunctionDeclarator(findOperatorCast(node), depth + 1);
    const auto child = firstDeclaratorChild(node);
    if (validNode(child)) return findOutermostFunctionDeclarator(child, depth + 1);
    return {};
}

enum class DeclaratorForm : std::uint8_t {
    Function,
    FunctionPointerVariable,
    Variable,
    Unsupported
};

struct DeclaratorClassification {
    DeclaratorForm form{DeclaratorForm::Variable};
    TSNode functionDeclarator{};
};

DeclaratorClassification classifyDeclarator(TSNode core) {
    if (!validNode(core)) return {};
    const auto type = nodeType(core);
    if (type == "operator_cast") return {DeclaratorForm::Function, core};
    if (type == "qualified_identifier") {
        const auto operatorCast = findOperatorCast(core);
        if (validNode(operatorCast)) return {DeclaratorForm::Function, operatorCast};
        return {DeclaratorForm::Variable, {}};
    }
    const auto functionDeclarator = findOutermostFunctionDeclarator(core);
    if (!validNode(functionDeclarator)) return {DeclaratorForm::Variable, {}};
    const auto inner = field(functionDeclarator, "declarator");
    if (!validNode(inner)) return {DeclaratorForm::Function, functionDeclarator};
    if (!hasParenthesizedPointer(inner)) return {DeclaratorForm::Function, functionDeclarator};
    if (hasFunctionDeclaratorBelow(inner)) return {DeclaratorForm::Unsupported, functionDeclarator};
    return {DeclaratorForm::FunctionPointerVariable, functionDeclarator};
}

struct SignatureRange {
    std::size_t begin{0};
    std::size_t end{0};
};

void addClippedRange(std::size_t begin, std::size_t end, std::size_t rangeBegin, std::size_t rangeEnd,
                     std::vector<SignatureRange>& ranges) {
    begin = std::max(begin, rangeBegin);
    end = std::min(end, rangeEnd);
    if (end > begin) ranges.push_back({begin, end});
}

// Collects executable or redundant ranges so signatures never carry function
// bodies, lambda bodies, member initializers, default arguments or comments.
void collectSignatureExclusions(TSNode node, std::string_view source, std::size_t start, std::size_t end,
                                bool excludeInitializers, std::vector<SignatureRange>& ranges,
                                std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 128) return;
    const auto nodeStart = ts_node_start_byte(node);
    const auto nodeEnd = ts_node_end_byte(node);
    if (nodeEnd <= start || nodeStart >= end) return;
    const auto type = nodeType(node);
    auto add = [&](std::size_t begin, std::size_t rangeEnd) {
        addClippedRange(begin, rangeEnd, start, end, ranges);
    };
    auto addInitializer = [&](TSNode value) {
        auto begin = static_cast<std::size_t>(ts_node_start_byte(value));
        auto probe = begin;
        while (probe > start && std::isspace(static_cast<unsigned char>(source[probe - 1]))) --probe;
        if (probe > start && source[probe - 1] == '=') begin = probe - 1;
        add(begin, ts_node_end_byte(value));
    };
    if (type == "comment") {
        add(nodeStart, nodeEnd);
        return;
    }
    if (type == "lambda_expression") {
        if (const auto body = field(node, "body"); validNode(body))
            add(ts_node_start_byte(body), ts_node_end_byte(body));
    }
    if (excludeInitializers) {
        if (const auto value = field(node, "default_value"); validNode(value)) addInitializer(value);
        if (const auto value = field(node, "value"); validNode(value)) addInitializer(value);
    }
    for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index)
        collectSignatureExclusions(ts_node_named_child(node, index), source, start, end, excludeInitializers,
                                   ranges, depth + 1);
}

void collectAttributeExclusions(TSNode node, std::size_t start, std::size_t end,
                                std::vector<SignatureRange>& ranges, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return;
    const auto nodeStart = ts_node_start_byte(node);
    const auto nodeEnd = ts_node_end_byte(node);
    if (nodeEnd <= start || nodeStart >= end) return;
    const auto type = nodeType(node);
    if (type == "attribute_specifier" || type == "attribute_declaration") {
        addClippedRange(nodeStart, nodeEnd, start, end, ranges);
        return;
    }
    for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index)
        collectAttributeExclusions(ts_node_named_child(node, index), start, end, ranges, depth + 1);
}

std::string concatenateExcluding(std::string_view source, std::size_t start, std::size_t end,
                                 std::vector<SignatureRange> ranges) {
    if (end <= start || end > source.size()) return {};
    std::sort(ranges.begin(), ranges.end(), [](const SignatureRange& left, const SignatureRange& right) {
        if (left.begin != right.begin) return left.begin < right.begin;
        return left.end < right.end;
    });
    std::string text;
    text.reserve(end - start);
    std::size_t cursor = start;
    for (const auto& range : ranges) {
        const auto begin = std::max(range.begin, start);
        const auto rangeEnd = std::min(range.end, end);
        if (rangeEnd <= begin) continue;
        if (begin > cursor) text.append(source.substr(cursor, begin - cursor));
        if (rangeEnd > cursor) cursor = rangeEnd;
    }
    if (cursor < end) text.append(source.substr(cursor, end - cursor));
    return text;
}

std::string signatureInRange(TSNode node, std::string_view source, std::size_t start, std::size_t end,
                             bool excludeInitializers) {
    if (!validNode(node) || end <= start || end > source.size()) return {};
    std::vector<SignatureRange> excluded;
    collectSignatureExclusions(node, source, start, end, excludeInitializers, excluded);
    return boundedSignature(concatenateExcluding(source, start, end, std::move(excluded)));
}

std::string signatureFor(TSNode node, std::string_view source, bool excludeInitializers) {
    if (!validNode(node)) return {};
    std::size_t end = ts_node_end_byte(node);
    if (const auto body = field(node, "body"); validNode(body))
        end = std::min<std::size_t>(end, ts_node_start_byte(body));
    if (const auto initializers = findChildType(node, "field_initializer_list"); validNode(initializers))
        end = std::min<std::size_t>(end, ts_node_start_byte(initializers));
    return signatureInRange(node, source, ts_node_start_byte(node), end, excludeInitializers);
}

std::string typeSyntaxForParameter(TSNode parameter, TSNode nameNode, std::string_view source) {
    const auto start = static_cast<std::size_t>(ts_node_start_byte(parameter));
    const auto end = static_cast<std::size_t>(ts_node_end_byte(parameter));
    std::vector<SignatureRange> excluded;
    collectSignatureExclusions(parameter, source, start, end, true, excluded);
    collectAttributeExclusions(parameter, start, end, excluded);
    if (validNode(nameNode)) {
        excluded.push_back({static_cast<std::size_t>(ts_node_start_byte(nameNode)),
                            static_cast<std::size_t>(ts_node_end_byte(nameNode))});
    }
    return boundedText(normalizeSyntax(concatenateExcluding(source, start, end, std::move(excluded))),
                       kMaximumTypeSyntaxBytes);
}

struct FunctionQualifiers {
    bool isConst{false};
    std::string text{};
};

FunctionQualifiers collectFunctionQualifiers(TSNode declarator, std::string_view source) {
    FunctionQualifiers result;
    if (!validNode(declarator)) return result;
    bool hasConst = false;
    bool hasVolatile = false;
    std::string reference;
    std::string exception;
    for (std::uint32_t index = 0; index < ts_node_named_child_count(declarator); ++index) {
        const auto child = ts_node_named_child(declarator, index);
        if (!validNode(child)) continue;
        const auto type = nodeType(child);
        if (type == "type_qualifier") {
            const auto text = normalizeSyntax(signatureFor(child, source, false));
            if (text == "const") hasConst = true;
            else if (text == "volatile") hasVolatile = true;
        } else if (type == "ref_qualifier") {
            const auto text = normalizeSyntax(signatureFor(child, source, false));
            if (!text.empty()) reference = text;
        } else if (type == "noexcept" || type == "throw_specifier") {
            const auto text = normalizeSyntax(signatureFor(child, source, false));
            if (!text.empty()) exception = text;
        }
    }
    auto append = [&](std::string_view token) {
        if (token.empty()) return;
        if (!result.text.empty()) result.text.push_back(' ');
        result.text.append(token);
    };
    if (hasConst) append("const");
    if (hasVolatile) append("volatile");
    append(reference);
    append(exception);
    result.isConst = hasConst;
    result.text = boundedText(std::move(result.text), kMaximumQualifierBytes);
    return result;
}

bool hasDirectSpecifier(TSNode node, std::string_view token, std::string_view source) {
    if (!validNode(node)) return false;
    for (std::uint32_t index = 0; index < ts_node_child_count(node); ++index) {
        const auto child = ts_node_child(node, index);
        if (!validNode(child)) continue;
        const auto type = nodeType(child);
        if (type == token) return true;
        if (type == "storage_class_specifier" || type == "type_qualifier" ||
            type == "virtual_specifier" || type == "explicit_function_specifier") {
            if (containsWord(normalizeSyntax(nodeText(child, source)), token)) return true;
        }
    }
    return false;
}

bool hasDirectTypeQualifier(TSNode node, std::string_view token, std::string_view source) {
    if (!validNode(node)) return false;
    for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
        const auto child = ts_node_named_child(node, index);
        if (!validNode(child) || nodeType(child) != "type_qualifier") continue;
        if (containsWord(normalizeSyntax(nodeText(child, source)), token)) return true;
    }
    return false;
}

bool hasConstQualifier(TSNode node, std::string_view source, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return false;
    const auto type = nodeType(node);
    if (type == "parameter_list" || type == "template_argument_list" || type == "lambda_expression" ||
        type == "compound_statement" || type == "class_specifier" || type == "struct_specifier" ||
        type == "union_specifier" || type == "enum_specifier") {
        return false;
    }
    if (type == "type_qualifier" && containsWord(normalizeSyntax(nodeText(node, source)), "const")) return true;
    for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
        if (hasConstQualifier(ts_node_named_child(node, index), source, depth + 1)) return true;
    }
    return false;
}

std::string typeNameFor(TSNode typeNode, std::string_view source) {
    if (!validNode(typeNode)) return {};
    const auto type = nodeType(typeNode);
    if (type == "class_specifier" || type == "struct_specifier" || type == "enum_specifier" ||
        type == "union_specifier") {
        const auto name = field(typeNode, "name");
        return validNode(name) ? nameTextFor(name, source) : std::string{};
    }
    return trim(nodeText(typeNode, source));
}

TSNode declarationEndNode(TSNode node) {
    TSNode end{};
    if (const auto body = field(node, "body"); validNode(body)) end = body;
    if (const auto initializers = findChildType(node, "field_initializer_list"); validNode(initializers)) {
        if (!validNode(end) || ts_node_start_byte(initializers) < ts_node_start_byte(end)) end = initializers;
    }
    return end;
}

struct ParameterData {
    TSNode node{};
    std::string name{};
    std::string typeName{};
    std::string signature{};
    std::string typeSyntax{};
    std::uint16_t index{0};
    bool isConst{false};
    bool isReference{false};
    bool isVariadic{false};
};

struct ParameterListData {
    std::vector<ParameterData> parameters{};
    std::vector<std::string> typeSyntax{};
    bool hasVariadic{false};
    bool hasError{false};
};

bool isParameterNode(std::string_view type) {
    return type == "parameter_declaration" || type == "optional_parameter_declaration" ||
           type == "variadic_parameter_declaration";
}

ParameterListData collectParameters(TSNode parameterList, std::string_view source) {
    ParameterListData data;
    if (!validNode(parameterList) || nodeType(parameterList) != "parameter_list") {
        data.hasError = true;
        return data;
    }
    if (ts_node_has_error(parameterList) || ts_node_is_missing(parameterList)) {
        data.hasError = true;
        return data;
    }
    std::uint32_t position = 0;
    for (std::uint32_t index = 0; index < ts_node_child_count(parameterList); ++index) {
        const auto child = ts_node_child(parameterList, index);
        if (!validNode(child)) continue;
        const auto type = nodeType(child);
        if (type == "...") {
            data.hasVariadic = true;
            data.typeSyntax.emplace_back("...");
            continue;
        }
        if (!isParameterNode(type)) {
            if (ts_node_is_named(child) && type != "comment") data.hasError = true;
            continue;
        }
        if (position >= 65535) {
            data.hasError = true;
            break;
        }
        ++position;
        if (ts_node_is_missing(child) || ts_node_is_error(child) || ts_node_has_error(child)) {
            data.hasError = true;
            continue;
        }
        const auto declarator = field(child, "declarator");
        const auto nameNode = findDeclaredNameNode(declarator);
        ParameterData parameter;
        parameter.node = child;
        parameter.index = static_cast<std::uint16_t>(position);
        parameter.name = validNode(nameNode) ? leafNameText(nameNode, source) : std::string{};
        auto typeNode = field(child, "type");
        if (!validNode(typeNode)) typeNode = findTypeNode(declarator);
        parameter.typeName = typeNameFor(typeNode, source);
        parameter.signature = signatureFor(child, source, true);
        parameter.typeSyntax = typeSyntaxForParameter(child, nameNode, source);
        if (parameter.typeSyntax.empty()) data.hasError = true;
        parameter.isConst = hasDirectTypeQualifier(child, "const", source) ||
                            hasConstQualifier(typeNode, source) || hasConstQualifier(declarator, source);
        parameter.isReference = containsAnyType(
            declarator, {"reference_declarator", "abstract_reference_declarator"}, 0, true);
        parameter.isVariadic = type == "variadic_parameter_declaration" ||
                               containsAnyType(declarator, {"variadic_declarator"}, 0, true);
        if (parameter.isVariadic) data.hasVariadic = true;
        data.typeSyntax.push_back(parameter.typeSyntax);
        data.parameters.push_back(std::move(parameter));
    }
    return data;
}

std::string makeSignatureKey(const std::string& qualifiedName, const std::vector<std::string>& typeSyntax,
                             const std::string& qualifiers) {
    std::string key = qualifiedName;
    key.push_back('(');
    for (std::size_t index = 0; index < typeSyntax.size(); ++index) {
        if (index > 0) key.push_back(',');
        key.append(typeSyntax[index]);
    }
    key.push_back(')');
    if (!qualifiers.empty()) {
        key.push_back(' ');
        key.append(qualifiers);
    }
    return boundedText(std::move(key), kMaximumSignatureKeyBytes);
}

struct FunctionDeclaratorInfo {
    TSNode parameterSource{};
    TSNode parameterList{};
};

FunctionDeclaratorInfo functionInfoFor(TSNode core, TSNode functionDeclarator) {
    if (validNode(functionDeclarator) && nodeType(functionDeclarator) == "function_declarator") {
        return {functionDeclarator, field(functionDeclarator, "parameters")};
    }
    auto operatorCast = findOperatorCast(core);
    if (!validNode(operatorCast)) return {};
    const auto abstract = field(operatorCast, "declarator");
    if (validNode(abstract) && nodeType(abstract) == "abstract_function_declarator") {
        return {abstract, field(abstract, "parameters")};
    }
    return {operatorCast, {}};
}

bool isConstructorName(const DeclaredName& name, std::string_view explicitScope, const Scope& scope) {
    if (name.leaf.empty() || looksLikeOperator(name.leaf) || name.leaf.starts_with("~")) return false;
    std::string owner;
    if (!explicitScope.empty()) owner = lastSegment(explicitScope);
    else if (scope.classScope) owner = lastSegment(scope.qualifiedName);
    if (owner.empty()) return false;
    return name.leaf == owner;
}

struct Extractor {
    std::string_view source;
    std::string filePath;
    std::vector<AstSymbol> symbols;
    std::map<std::string, std::string> explicitParents;

    void noteExplicitParent(AstSymbol& symbol, const Scope& scope, std::string_view explicitScope) {
        if (explicitScope.empty()) return;
        symbol.parentSymbolId.clear();
        explicitParents.emplace(symbol.symbolId, qualify(scope.qualifiedName, explicitScope));
    }

    AstSymbol makeSymbol(TSNode spanNode, TSNode endBoundary, std::string name, std::string qualified,
                         AstNodeKind kind, AstSymbolRole role, const Scope& scope, std::string signature = {},
                         std::string typeName = {}) {
        AstSymbol symbol;
        symbol.name = std::move(name);
        symbol.qualifiedName = std::move(qualified);
        symbol.kind = kind;
        symbol.role = role;
        symbol.parentSymbolId = scope.parentSymbolId;
        symbol.filePath = filePath;
        const auto begin = static_cast<std::size_t>(ts_node_start_byte(spanNode));
        auto end = static_cast<std::size_t>(ts_node_end_byte(spanNode));
        auto startPoint = ts_node_start_point(spanNode);
        auto endPoint = ts_node_end_point(spanNode);
        if (validNode(endBoundary)) {
            const auto boundary = static_cast<std::size_t>(ts_node_start_byte(endBoundary));
            if (boundary > begin && boundary <= end) {
                end = boundary;
                endPoint = ts_node_start_point(endBoundary);
            }
        }
        symbol.span = {begin, end};
        symbol.startLine = startPoint.row + 1;
        symbol.startColumn = startPoint.column + 1;
        symbol.endLine = endPoint.row + 1;
        symbol.endColumn = endPoint.column + 1;
        symbol.signature = boundedSignature(std::move(signature));
        symbol.typeName = std::move(typeName);
        symbol.symbolId = makeSymbolId(filePath, symbol.qualifiedName, symbol.span.startByte, symbol.span.endByte);
        return symbol;
    }

    void applyDeclarationSpecifiers(AstSymbol& symbol, TSNode node) {
        symbol.isStatic = hasDirectSpecifier(node, "static", source);
        symbol.isInline = hasDirectSpecifier(node, "inline", source);
        symbol.isConstexpr = hasDirectSpecifier(node, "constexpr", source);
    }

    void addParameters(const ParameterListData& parameters, const std::string& functionQualified,
                       const std::string& functionId) {
        const Scope parameterScope{functionQualified, functionId, false};
        for (const auto& parameter : parameters.parameters) {
            if (symbols.size() >= 10000) return;
            if (parameter.name.empty()) continue;
            auto symbol = makeSymbol(parameter.node, {}, parameter.name, qualify(functionQualified, parameter.name),
                                     AstNodeKind::Parameter, AstSymbolRole::Parameter, parameterScope,
                                     parameter.signature, parameter.typeName);
            symbol.isDeclaration = true;
            symbol.isDefinition = false;
            symbol.isTemplate = false;
            symbol.isStatic = false;
            symbol.isInline = false;
            symbol.isConstexpr = false;
            symbol.isVariadic = parameter.isVariadic;
            symbol.isReference = parameter.isReference;
            symbol.isConst = parameter.isConst;
            symbol.parameterIndex = parameter.index;
            symbol.signatureKey.clear();
            symbol.qualifiers.clear();
            symbols.push_back(std::move(symbol));
        }
    }

    void extractFunction(TSNode node, TSNode core, TSNode functionDeclarator, const Scope& scope,
                         bool isTemplate, bool definitionSite) {
        if (symbols.size() >= 10000) return;
        const auto nameInfo = declaredName(core, source);
        if (nameInfo.leaf.empty()) return;
        const auto explicitScope = explicitScopeText(nameInfo.node, source);
        if (definitionSite) {
            const auto declaratorEnd = static_cast<std::size_t>(ts_node_end_byte(core));
            for (std::uint32_t index = 0; index < ts_node_child_count(node); ++index) {
                const auto child = ts_node_child(node, index);
                if (!validNode(child)) continue;
                const auto* fieldName = ts_node_field_name_for_child(node, index);
                if (fieldName != nullptr &&
                    (std::string_view(fieldName) == "body" || std::string_view(fieldName) == "field_initializer_list"))
                    continue;
                // Anything after the declarator is implementation syntax: the
                // body, a constructor initializer list, or an unlabelled ERROR
                // recovered for the body. It cannot invalidate the declaration.
                if (ts_node_start_byte(child) >= declaratorEnd) continue;
                if (ts_node_is_missing(child) || ts_node_has_error(child)) return;
            }
        }

        ParameterListData parameters;
        const auto info = functionInfoFor(core, functionDeclarator);
        if (validNode(info.parameterList)) {
            parameters = collectParameters(info.parameterList, source);
            if (parameters.hasError) return;
        } else if (!validNode(info.parameterSource) || nodeType(info.parameterSource) != "operator_cast") {
            return;
        }

        const auto qualifiers = collectFunctionQualifiers(info.parameterSource, source);
        const auto body = field(node, "body");
        const bool definition = definitionSite && validNode(body) && !ts_node_has_error(node);

        const bool destructorName = nameInfo.leaf.starts_with("~");
        const bool operatorName = looksLikeOperator(nameInfo.leaf);
        const bool constructor = !destructorName && !operatorName &&
                                !validNode(field(node, "type")) &&
                                isConstructorName(nameInfo, explicitScope, scope);
        AstSymbolRole role = AstSymbolRole::Function;
        if (destructorName) role = AstSymbolRole::Destructor;
        else if (operatorName) role = AstSymbolRole::Operator;
        else if (constructor) role = AstSymbolRole::Constructor;
        else if (scope.classScope || !explicitScope.empty()) role = AstSymbolRole::Method;
        const bool memberContext = scope.classScope || !explicitScope.empty();
        const auto kind = (role == AstSymbolRole::Method || role == AstSymbolRole::Constructor ||
                           role == AstSymbolRole::Destructor ||
                           (role == AstSymbolRole::Operator && memberContext))
                              ? AstNodeKind::Method
                              : AstNodeKind::Function;

        const auto qualified = qualify(scope.qualifiedName, nameInfo.text);
        std::string signature;
        if (!definitionSite || definition) {
            signature = signatureFor(node, source, true);
        } else {
            const auto boundary = declarationEndNode(node);
            const auto end = validNode(boundary) ? static_cast<std::size_t>(ts_node_start_byte(boundary))
                                                 : static_cast<std::size_t>(ts_node_end_byte(core));
            signature = signatureInRange(node, source, ts_node_start_byte(node), end, true);
            if (signature.empty()) signature = signatureFor(node, source, true);
        }
        auto symbol = makeSymbol(node, definition || !definitionSite ? TSNode{} : declarationEndNode(node),
                                 nameInfo.leaf, qualified, kind, role, scope, std::move(signature),
                                 typeNameFor(field(node, "type"), source));
        symbol.isDeclaration = !definition;
        symbol.isDefinition = definition;
        symbol.isTemplate = isTemplate;
        applyDeclarationSpecifiers(symbol, node);
        symbol.isVariadic = parameters.hasVariadic;
        symbol.isReference = containsAnyType(
            core, {"reference_declarator", "abstract_reference_declarator"}, 0, true);
        symbol.isConst = qualifiers.isConst;
        symbol.parameterIndex = 0;
        symbol.signatureKey = makeSignatureKey(qualified, parameters.typeSyntax, qualifiers.text);
        symbol.qualifiers = qualifiers.text;
        noteExplicitParent(symbol, scope, explicitScope);
        const auto symbolId = symbol.symbolId;
        symbols.push_back(std::move(symbol));
        addParameters(parameters, qualified, symbolId);
    }

    std::string dataSignature(TSNode node, TSNode core) {
        std::uint32_t declaratorCount = 0;
        std::size_t firstDeclaratorStart = 0;
        for (std::uint32_t index = 0; index < ts_node_child_count(node); ++index) {
            const auto* fieldName = ts_node_field_name_for_child(node, index);
            if (fieldName == nullptr || std::string_view(fieldName) != "declarator") continue;
            const auto child = ts_node_child(node, index);
            if (!validNode(child)) continue;
            if (declaratorCount == 0) firstDeclaratorStart = ts_node_start_byte(child);
            ++declaratorCount;
        }
        if (declaratorCount <= 1) return signatureFor(node, source, true);
        const auto nodeStart = static_cast<std::size_t>(ts_node_start_byte(node));
        auto prefix = signatureInRange(node, source, nodeStart, firstDeclaratorStart, true);
        auto declaratorText = signatureInRange(node, source, ts_node_start_byte(core), ts_node_end_byte(core), true);
        if (prefix.empty()) return boundedSignature(std::move(declaratorText));
        if (!declaratorText.empty()) {
            prefix.push_back(' ');
            prefix.append(declaratorText);
        }
        return boundedSignature(std::move(prefix));
    }

    void extractData(TSNode node, TSNode direct, TSNode core, const Scope& scope, bool isField,
                     bool isTemplate) {
        if (symbols.size() >= 10000) return;
        const auto nameInfo = declaredName(core, source);
        if (nameInfo.leaf.empty()) return;
        const auto explicitScope = explicitScopeText(nameInfo.node, source);
        const bool memberLike = isField || scope.classScope || !explicitScope.empty();
        const auto role = memberLike ? AstSymbolRole::Field : AstSymbolRole::Variable;
        const auto kind = memberLike ? AstNodeKind::Field : AstNodeKind::Unknown;
        const auto qualified = qualify(scope.qualifiedName, nameInfo.text);
        auto symbol = makeSymbol(core, {}, nameInfo.leaf, qualified, kind, role, scope,
                                 dataSignature(node, core), typeNameFor(field(node, "type"), source));
        const bool isExtern = hasDirectSpecifier(node, "extern", source);
        const bool hasInitializer = nodeType(direct) == "init_declarator";
        bool definition = !isExtern || hasInitializer;
        if (isField || scope.classScope) definition = false;
        symbol.isDeclaration = !definition;
        symbol.isDefinition = definition;
        symbol.isTemplate = isTemplate;
        applyDeclarationSpecifiers(symbol, node);
        symbol.isVariadic = false;
        symbol.isReference = containsAnyType(
            core, {"reference_declarator", "abstract_reference_declarator"}, 0, true);
        symbol.isConst = hasDirectTypeQualifier(node, "const", source) ||
                         hasConstQualifier(field(node, "type"), source) || hasConstQualifier(core, source);
        symbol.parameterIndex = 0;
        symbol.signatureKey.clear();
        symbol.qualifiers.clear();
        noteExplicitParent(symbol, scope, explicitScope);
        symbols.push_back(std::move(symbol));
    }

    void handleDeclaration(TSNode node, const Scope& scope, std::uint32_t depth, bool isTemplate, bool isField) {
        if (!validNode(node) || ts_node_is_missing(node)) return;
        const bool nodeHasError = ts_node_has_error(node);
        bool hasDeclarator = false;
        for (std::uint32_t index = 0; index < ts_node_child_count(node); ++index) {
            const auto* fieldName = ts_node_field_name_for_child(node, index);
            if (fieldName != nullptr && std::string_view(fieldName) == "declarator") {
                hasDeclarator = true;
                break;
            }
        }
        for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
            const auto child = ts_node_named_child(node, index);
            if (!validNode(child)) continue;
            const auto type = nodeType(child);
            if (type == "class_specifier" || type == "struct_specifier" || type == "enum_specifier") {
                const bool hasBody = validNode(field(child, "body"));
                if (hasBody || !hasDeclarator) visit(child, scope, depth + 1, isTemplate);
                continue;
            }
            if (type == "alias_declaration" || type == "type_definition" || type == "template_declaration") {
                visit(child, scope, depth + 1, isTemplate);
            }
        }
        if (nodeHasError) return;
        for (std::uint32_t index = 0; index < ts_node_child_count(node); ++index) {
            if (symbols.size() >= 10000) return;
            const auto* fieldName = ts_node_field_name_for_child(node, index);
            if (fieldName == nullptr || std::string_view(fieldName) != "declarator") continue;
            const auto direct = ts_node_child(node, index);
            if (!validNode(direct)) continue;
            auto core = direct;
            if (nodeType(core) == "init_declarator") core = field(core, "declarator");
            if (!validNode(core)) continue;
            const auto classification = classifyDeclarator(core);
            if (classification.form == DeclaratorForm::Unsupported) continue;
            if (classification.form == DeclaratorForm::Function) {
                extractFunction(node, core, classification.functionDeclarator, scope, isTemplate, false);
            } else {
                extractData(node, direct, core, scope, isField, isTemplate);
            }
        }
    }

    void visitNamespace(TSNode node, const Scope& scope, std::uint32_t depth) {
        const auto nameNode = field(node, "name");
        if (validNode(nameNode) && ts_node_has_error(nameNode)) return;
        std::string name = validNode(nameNode) ? nameTextFor(nameNode, source) : std::string{};
        if (name.empty()) name = kAnonymousNamespaceName;
        const auto qualified = qualify(scope.qualifiedName, name);
        const bool inlineNamespace = hasDirectSpecifier(node, "inline", source);
        auto symbol = makeSymbol(node, {}, name, qualified, AstNodeKind::Namespace, AstSymbolRole::Namespace,
                                 scope, (inlineNamespace ? std::string("inline namespace ") : std::string("namespace ")) + name);
        symbol.isDefinition = true;
        symbol.isDeclaration = false;
        symbol.isInline = inlineNamespace;
        const auto symbolId = symbol.symbolId;
        symbols.push_back(std::move(symbol));
        if (const auto body = field(node, "body"); validNode(body)) {
            visit(body, Scope{qualified, symbolId, false}, depth + 1, false);
        }
    }

    void visitAggregate(TSNode node, const Scope& scope, std::uint32_t depth, bool isTemplate, bool isEnum) {
        const auto nameNode = field(node, "name");
        if (!validNode(nameNode) || ts_node_has_error(nameNode)) return;
        const auto name = leafNameText(nameNode, source);
        if (name.empty()) return;
        const auto explicitScope = explicitScopeText(nameNode, source);
        const auto qualified = qualify(scope.qualifiedName, nameTextFor(nameNode, source));
        const bool hasBody = validNode(field(node, "body"));
        const auto kind = isEnum ? AstNodeKind::Enum
                                 : nodeType(node) == "class_specifier" ? AstNodeKind::Class : AstNodeKind::Struct;
        const auto signaturePrefix = isEnum ? std::string("enum ")
                                            : nodeType(node) == "class_specifier" ? std::string("class ")
                                                                                  : std::string("struct ");
        auto symbol = makeSymbol(node, {}, name, qualified, kind, AstSymbolRole::Type, scope,
                                 signaturePrefix + name);
        symbol.isDeclaration = !hasBody;
        symbol.isDefinition = hasBody;
        symbol.isTemplate = isTemplate;
        noteExplicitParent(symbol, scope, explicitScope);
        const auto symbolId = symbol.symbolId;
        symbols.push_back(std::move(symbol));
        if (hasBody) {
            visit(field(node, "body"), Scope{qualified, symbolId, !isEnum}, depth + 1, false);
        }
    }

    void visit(TSNode node, const Scope& scope, std::uint32_t depth = 0, bool isTemplate = false) {
        if (!validNode(node) || ts_node_is_error(node) || depth > 128 || symbols.size() >= 10000) return;
        const auto type = nodeType(node);

        if (type == "namespace_definition") {
            visitNamespace(node, scope, depth);
            return;
        }
        if (type == "class_specifier" || type == "struct_specifier") {
            visitAggregate(node, scope, depth, isTemplate, false);
            return;
        }
        if (type == "enum_specifier") {
            visitAggregate(node, scope, depth, isTemplate, true);
            return;
        }
        if (type == "enumerator") {
            if (ts_node_has_error(node)) return;
            const auto nameNode = field(node, "name");
            if (!validNode(nameNode)) return;
            const auto name = trim(nodeText(nameNode, source));
            if (name.empty()) return;
            const auto qualified = qualify(scope.qualifiedName, name);
            auto symbol = makeSymbol(node, {}, name, qualified, AstNodeKind::Enumerator,
                                     AstSymbolRole::Enumerator, scope, signatureFor(node, source, false));
            symbol.isDeclaration = false;
            symbol.isDefinition = true;
            symbols.push_back(std::move(symbol));
            return;
        }
        if (type == "template_declaration") {
            for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
                const auto child = ts_node_named_child(node, index);
                if (!validNode(child)) continue;
                const auto childType = nodeType(child);
                if (childType == "declaration" || childType == "function_definition" ||
                    childType == "class_specifier" || childType == "struct_specifier" ||
                    childType == "enum_specifier" || childType == "alias_declaration" ||
                    childType == "type_definition" || childType == "template_declaration") {
                    visit(child, scope, depth + 1, true);
                }
            }
            return;
        }
        if (type == "function_definition") {
            const auto core = field(node, "declarator");
            if (!validNode(core)) return;
            const auto classification = classifyDeclarator(core);
            if (classification.form != DeclaratorForm::Function) return;
            extractFunction(node, core, classification.functionDeclarator, scope, isTemplate, true);
            return;
        }
        if (type == "declaration" || type == "field_declaration") {
            handleDeclaration(node, scope, depth, isTemplate, type == "field_declaration");
            return;
        }
        if (type == "alias_declaration") {
            if (ts_node_has_error(node)) return;
            const auto nameNode = field(node, "name");
            if (!validNode(nameNode)) return;
            const auto name = leafNameText(nameNode, source);
            if (name.empty()) return;
            const auto qualified = qualify(scope.qualifiedName, nameTextFor(nameNode, source));
            auto symbol = makeSymbol(node, {}, name, qualified, AstNodeKind::TypeAlias, AstSymbolRole::TypeAlias,
                                     scope, signatureFor(node, source, true));
            symbol.isDeclaration = true;
            symbol.isDefinition = false;
            symbol.isTemplate = isTemplate;
            symbols.push_back(std::move(symbol));
            return;
        }
        if (type == "type_definition") {
            for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
                const auto child = ts_node_named_child(node, index);
                if (!validNode(child)) continue;
                const auto childType = nodeType(child);
                if (childType == "class_specifier" || childType == "struct_specifier" ||
                    childType == "enum_specifier") {
                    if (validNode(field(child, "body"))) visit(child, scope, depth + 1, isTemplate);
                }
            }
            return;
        }
        if (type == "using_declaration" || type == "friend_declaration" || type == "namespace_alias_definition") {
            // Using directives/declarations and friend declarations do not declare a
            // member of the enclosing scope in this model. Typedef aliases are also
            // omitted; `using Name = ...` is handled as alias_declaration.
            return;
        }
        if (type == "translation_unit" || type == "declaration_list" || type == "field_declaration_list" ||
            type == "enumerator_list" || type == "linkage_specification" || type == "preproc_ifdef" ||
            type == "preproc_if" || type == "preproc_else" || type == "preproc_elif" ||
            type == "preproc_elifdef") {
            for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index)
                visit(ts_node_named_child(node, index), scope, depth + 1, isTemplate);
            return;
        }
    }
};

} // namespace

AstExtractionResult AstSymbolExtractor::extractSymbols(std::string_view sourceCode, std::string_view filePath) {
    AstExtractionResult result;
    if (sourceCode.size() > 2 * 1024 * 1024) {
        result.error = "Source file exceeds the 2 MiB AST limit";
        return result;
    }
    TSParser* parser = ts_parser_new();
    if (parser == nullptr) {
        result.error = "Tree-sitter parser allocation failed";
        return result;
    }
    if (!ts_parser_set_language(parser, tree_sitter_cpp())) {
        ts_parser_delete(parser);
        result.error = "Tree-sitter C++ grammar could not be loaded";
        return result;
    }
    TSTree* tree = ts_parser_parse_string(parser, nullptr, sourceCode.data(),
                                          static_cast<std::uint32_t>(sourceCode.size()));
    if (tree == nullptr) {
        ts_parser_delete(parser);
        result.error = "Tree-sitter parse failed";
        return result;
    }
    const auto root = ts_tree_root_node(tree);
    if (ts_node_has_error(root)) {
        // Tree-sitter can reject Qt/Windows macro invocations while still parsing
        // surrounding declarations. Record the degraded quality, skip ERROR
        // subtrees during traversal, and let the index decide whether any usable
        // symbol remains. A clean parse is never reported for recovered files.
        result.recovered = true;
    }

    Extractor extractor{sourceCode, std::string(filePath), {}};
    extractor.visit(root, {});
    result.parseOk = !result.recovered;
    result.symbols = std::move(extractor.symbols);
    if (!result.parseOk && result.symbols.empty()) {
        result.error = "Tree-sitter reported a syntax error and recovered no symbols";
    }

    std::map<std::string, std::size_t> scopes;
    for (std::size_t index = 0; index < result.symbols.size(); ++index) {
        const auto& symbol = result.symbols[index];
        if (symbol.role != AstSymbolRole::Namespace && symbol.role != AstSymbolRole::Type) continue;
        const auto [existing, inserted] = scopes.emplace(symbol.qualifiedName, index);
        if (!inserted && !result.symbols[existing->second].isDefinition && symbol.isDefinition)
            existing->second = index;
    }
    for (auto& symbol : result.symbols) {
        if (!symbol.parentSymbolId.empty()) continue;
        const auto pending = extractor.explicitParents.find(symbol.symbolId);
        if (pending == extractor.explicitParents.end()) continue;
        const auto parent = scopes.find(pending->second);
        if (parent != scopes.end()) symbol.parentSymbolId = result.symbols[parent->second].symbolId;
    }
    std::sort(result.symbols.begin(), result.symbols.end(), [](const AstSymbol& left, const AstSymbol& right) {
        if (left.span.startByte != right.span.startByte) return left.span.startByte < right.span.startByte;
        return left.qualifiedName < right.qualifiedName;
    });
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return result;
}

} // namespace roclint::storage
