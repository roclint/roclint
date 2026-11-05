#include "roclint/storage/AstSymbolExtractor.hpp"

#include "roclint/core/crypto/Sha256.hpp"

#include <tree_sitter/api.h>
#include <tree-sitter-cpp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace roclint::storage {
namespace {

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

TSNode findDeclarator(TSNode node, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return {};
    if (nodeType(node) == "function_declarator") return node;
    for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
        const auto found = findDeclarator(ts_node_named_child(node, index), depth + 1);
        if (validNode(found)) return found;
    }
    return {};
}

TSNode findNameNode(TSNode node, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return {};
    const auto type = nodeType(node);
    if (type == "identifier" || type == "field_identifier" || type == "type_identifier" ||
        type == "qualified_identifier" || type == "namespace_identifier" || type == "operator_name" ||
        type == "operator_cast" || type == "destructor_name") return node;
    for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
        const auto found = findNameNode(ts_node_named_child(node, index), depth + 1);
        if (validNode(found)) return found;
    }
    return {};
}

TSNode findTypeNode(TSNode node, std::uint32_t depth = 0) {
    if (!validNode(node) || depth > 64) return {};
    const auto type = nodeType(node);
    if (type == "type_identifier" || type == "primitive_type" || type == "sized_type_specifier" ||
        type == "qualified_identifier" || type == "template_type" || type == "decltype") return node;
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

std::string joinQualified(std::string_view parent, std::string_view name) {
    if (parent.empty()) return std::string(name);
    if (name.empty()) return std::string(parent);
    if (name.find("::") != std::string_view::npos) return std::string(name);
    return std::string(parent) + "::" + std::string(name);
}

std::string signatureFor(TSNode node, std::string_view source) {
    if (!validNode(node)) return {};
    auto end = ts_node_end_byte(node);
    const auto body = field(node, "body");
    if (validNode(body)) end = ts_node_start_byte(body);
    const auto start = ts_node_start_byte(node);
    if (end < start || end > source.size()) return {};
    auto signature = trim(source.substr(start, end - start));
    if (signature.size() > 1200) signature.resize(1200);
    return signature;
}

struct Extractor {
    std::string_view source;
    std::string filePath;
    std::vector<AstSymbol> symbols;

    AstSymbol makeSymbol(TSNode node, std::string name, std::string qualified, AstNodeKind kind,
                         const Scope& scope, std::string signature = {}, std::string typeName = {},
                         bool isConst = false) {
        AstSymbol symbol;
        symbol.name = std::move(name);
        symbol.qualifiedName = std::move(qualified);
        symbol.kind = kind;
        symbol.parentSymbolId = scope.parentSymbolId;
        symbol.filePath = filePath;
        symbol.span = {ts_node_start_byte(node), ts_node_end_byte(node)};
        const auto start = ts_node_start_point(node);
        const auto end = ts_node_end_point(node);
        symbol.startLine = start.row + 1;
        symbol.startColumn = start.column + 1;
        symbol.endLine = end.row + 1;
        symbol.endColumn = end.column + 1;
        symbol.signature = std::move(signature);
        symbol.typeName = std::move(typeName);
        symbol.isConst = isConst;
        symbol.symbolId = makeSymbolId(filePath, symbol.qualifiedName, symbol.span.startByte, symbol.span.endByte);
        return symbol;
    }

    void addParameters(TSNode function, const Scope& functionScope, std::string_view functionQualified) {
        const auto declarator = findDeclarator(function);
        if (!validNode(declarator)) return;
        const auto parameters = findChildType(declarator, "parameter_list");
        if (!validNode(parameters)) return;
        for (std::uint32_t index = 0; index < ts_node_named_child_count(parameters); ++index) {
            const auto parameter = ts_node_named_child(parameters, index);
            if (nodeType(parameter) != "parameter_declaration") continue;
            const auto nameNode = findNameNode(field(parameter, "declarator"));
            const auto name = nodeText(nameNode, source);
            if (name.empty()) continue;
            auto typeNode = field(parameter, "type");
            if (!validNode(typeNode)) typeNode = findTypeNode(field(parameter, "declarator"));
            const auto typeName = trim(nodeText(typeNode, source));
            const auto text = nodeText(parameter, source);
            const bool isConst = text.find("const") != std::string::npos;
            const auto qualified = joinQualified(functionQualified, name);
            auto symbol = makeSymbol(parameter, name, qualified, AstNodeKind::Parameter, functionScope,
                                     trim(text), typeName, isConst);
            symbol.parentSymbolId = functionScope.parentSymbolId;
            symbols.push_back(std::move(symbol));
        }
    }

    void visit(TSNode node, const Scope& scope, std::uint32_t depth = 0) {
        if (!validNode(node) || ts_node_is_error(node) || depth > 128 || symbols.size() >= 10000) return;
        const auto type = nodeType(node);
        if (type == "namespace_definition") {
            const auto name = nodeText(field(node, "name"), source);
            if (name.empty()) return;
            const auto qualified = joinQualified(scope.qualifiedName, name);
            const auto symbol = makeSymbol(node, name, qualified, AstNodeKind::Namespace, scope,
                                           "namespace " + name);
            const auto id = symbol.symbolId;
            symbols.push_back(symbol);
            const Scope childScope{qualified, id, scope.classScope};
            for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
                const auto child = ts_node_named_child(node, index);
                if (nodeType(child) == "declaration_list") visit(child, childScope, depth + 1);
            }
            return;
        }
        if (type == "declaration_list") {
            for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index)
                visit(ts_node_named_child(node, index), scope, depth + 1);
            return;
        }
        if (type == "class_specifier" || type == "struct_specifier") {
            const auto name = nodeText(field(node, "name"), source);
            if (name.empty()) return;
            const auto qualified = joinQualified(scope.qualifiedName, name);
            const auto kind = type == "class_specifier" ? AstNodeKind::Class : AstNodeKind::Struct;
            const auto symbol = makeSymbol(node, name, qualified, kind, scope,
                                           (kind == AstNodeKind::Class ? "class " : "struct ") + name);
            const auto id = symbol.symbolId;
            symbols.push_back(symbol);
            const Scope childScope{qualified, id, true};
            const auto body = field(node, "body");
            if (validNode(body)) visit(body, childScope, depth + 1);
            return;
        }
        if (type == "enum_specifier") {
            const auto name = nodeText(field(node, "name"), source);
            if (name.empty()) return;
            const auto qualified = joinQualified(scope.qualifiedName, name);
            const auto symbol = makeSymbol(node, name, qualified, AstNodeKind::Enum, scope, "enum " + name);
            const auto id = symbol.symbolId;
            symbols.push_back(symbol);
            const Scope childScope{qualified, id, scope.classScope};
            const auto body = field(node, "body");
            if (validNode(body)) visit(body, childScope, depth + 1);
            return;
        }
        if (type == "enumerator") {
            if (ts_node_has_error(node)) return;
            const auto name = nodeText(field(node, "name"), source);
            if (name.empty()) return;
            const auto qualified = joinQualified(scope.qualifiedName, name);
            symbols.push_back(makeSymbol(node, name, qualified, AstNodeKind::Enumerator, scope,
                                         trim(nodeText(node, source))));
            return;
        }
        if (type == "function_definition") {
            if (ts_node_has_error(node)) return;
            const auto declarator = findDeclarator(node);
            if (!validNode(declarator)) return;
            auto nameNode = field(declarator, "declarator");
            if (!validNode(nameNode)) nameNode = ts_node_named_child(declarator, 0);
            const auto nameText = nodeText(nameNode, source);
            if (nameText.empty()) return;
            const auto qualified = nameText.find("::") != std::string::npos
                ? nameText : joinQualified(scope.qualifiedName, nameText);
            const auto kind = scope.classScope || nameText.find("::") != std::string::npos
                ? AstNodeKind::Method : AstNodeKind::Function;
            const auto signature = signatureFor(node, source);
            const auto typeName = trim(nodeText(field(node, "type"), source));
            const auto symbol = makeSymbol(node, nameText.substr(nameText.find_last_of(':') + 1), qualified,
                                           kind, scope, signature, typeName,
                                           signature.find(" const") != std::string::npos);
            const auto id = symbol.symbolId;
            symbols.push_back(symbol);
            addParameters(node, Scope{qualified, id, false}, qualified);
            return;
        }
        if (type == "declaration" || type == "field_declaration") {
            bool composite = false;
            for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
                const auto childType = nodeType(ts_node_named_child(node, index));
                if (childType == "enum_specifier" || childType == "class_specifier" ||
                    childType == "struct_specifier" || childType == "alias_declaration" ||
                    childType == "type_definition" || childType == "using_declaration") {
                    composite = true;
                    break;
                }
            }
            if (composite) {
                for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index)
                    visit(ts_node_named_child(node, index), scope, depth + 1);
                return;
            }
            if (ts_node_has_error(node)) return;
            const auto declarator = findDeclarator(node);
            if (validNode(declarator)) {
                auto nameNode = field(declarator, "declarator");
                if (!validNode(nameNode)) nameNode = ts_node_named_child(declarator, 0);
                const auto nameText = nodeText(nameNode, source);
                if (nameText.empty()) return;
                const auto qualified = joinQualified(scope.qualifiedName, nameText);
                const auto signature = signatureFor(node, source);
                const auto typeName = trim(nodeText(field(node, "type"), source));
                const auto symbol = makeSymbol(node, nameText, qualified,
                                               scope.classScope ? AstNodeKind::Method : AstNodeKind::Function,
                                               scope, signature, typeName,
                                               signature.find(" const") != std::string::npos);
                const auto id = symbol.symbolId;
                symbols.push_back(symbol);
                addParameters(node, Scope{qualified, id, false}, qualified);
            } else if (scope.classScope) {
                const auto name = nodeText(findNameNode(field(node, "declarator")), source);
                if (name.empty()) return;
                const auto qualified = joinQualified(scope.qualifiedName, name);
                const auto typeName = trim(nodeText(field(node, "type"), source));
                const auto text = nodeText(node, source);
                symbols.push_back(makeSymbol(node, name, qualified, AstNodeKind::Field, scope, trim(text),
                                             typeName, text.find("const") != std::string::npos));
            }
            return;
        }
        if (type == "alias_declaration" || type == "type_definition" || type == "using_declaration") {
            if (ts_node_has_error(node)) return;
            const auto name = nodeText(findNameNode(field(node, "name")), source);
            if (name.empty()) return;
            const auto qualified = joinQualified(scope.qualifiedName, name);
            symbols.push_back(makeSymbol(node, name, qualified, AstNodeKind::TypeAlias, scope,
                                         trim(nodeText(node, source))));
            return;
        }
        for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index)
            visit(ts_node_named_child(node, index), scope, depth + 1);
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

    std::map<std::string, std::size_t> byQualified;
    for (std::size_t index = 0; index < result.symbols.size(); ++index)
        byQualified.emplace(result.symbols[index].qualifiedName, index);
    for (auto& symbol : result.symbols) {
        if (!symbol.parentSymbolId.empty()) continue;
        const auto separator = symbol.qualifiedName.rfind("::");
        if (separator == std::string::npos) continue;
        const auto parentName = symbol.qualifiedName.substr(0, separator);
        const auto parent = byQualified.find(parentName);
        if (parent != byQualified.end()) symbol.parentSymbolId = result.symbols[parent->second].symbolId;
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
