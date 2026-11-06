#pragma once

#include "roclint/analysis/SourceSpan.hpp"

#include <cstdint>
#include <string>

namespace roclint::storage {

enum class AstNodeKind : std::uint8_t {
    Unknown = 0,
    Namespace,
    Class,
    Struct,
    Function,
    Method,
    Parameter,
    Field,
    Enum,
    Enumerator,
    TypeAlias
};

[[nodiscard]] const char* toString(AstNodeKind kind) noexcept;

// Structural role of a symbol. AstSymbolRole is additive metadata: AstNodeKind is
// retained for compatibility with existing index consumers and databases.
enum class AstSymbolRole : std::uint8_t {
    Unknown = 0,
    Namespace,
    Type,
    Function,
    Method,
    Constructor,
    Destructor,
    Operator,
    Variable,
    Field,
    Parameter,
    Enumerator,
    TypeAlias
};

[[nodiscard]] const char* toString(AstSymbolRole role) noexcept;

// One indexed declaration or definition site.
//
// `isDeclaration` and `isDefinition` describe the *site* that produced the
// symbol, not all the semantic roles of the C++ entity. A definition site sets
// isDefinition=true and isDeclaration=false; a declaration site does the
// reverse. In particular, a function whose declaration/parameter subtree is
// valid but whose body was not, is emitted as a declaration
// (isDeclaration=true, isDefinition=false) rather than being treated as a
// definition. `= default`, `= delete`, pure-virtual declarations and type
// aliases are declaration sites for this model.
//
// `signatureKey` is a normalized *syntactic* overload key:
//
//   qualified name + '(' + comma-separated declared parameter type syntax
//   with parameter names, defaults, comments and attributes removed + ')'
//   + function qualifiers
//
// Example: `demo::Engine::run(int,double) const noexcept`. It is deliberately
// syntactic: it does not claim compiler-resolved types, typedef expansion,
// template instantiation, macro expansion, overload resolution or runtime
// dispatch. Two sites sharing a key is evidence that they may refer to the same
// entity, not proof of semantic equivalence. The key is empty for non-function
// symbols (namespaces, types, variables, fields, parameters, enumerators and
// aliases).
//
// `qualifiers` is the normalized function qualifier suffix, for example
// "const", "const & noexcept" or "noexcept(false)".
//
// `isReference` is true when the indexed symbol's declarator contains `&` or
// `&&`: reference parameters, reference variables/fields and functions whose
// return type is a reference. Function return references are also visible in
// `signature`; the flag is syntactic and does not imply reference collapsing or
// semantic analysis.
//
// `isStatic`, `isConstexpr` and `isInline` reflect direct specifiers at the
// indexed site; an out-of-class definition therefore does not inherit a
// specifier written only on the in-class declaration. `isVariadic` is true for
// a function with a C ellipsis or a parameter pack in its parameter list, and
// for a parameter symbol that is a pack. `isConst` is an explicit `const`
// qualifier (including a trailing const member function); `constexpr` alone does
// not set it. `isTemplate` reflects whether the symbol's own declaration site is inside
// a template declaration (class, function, alias or variable template). A member
// declaration inside a template class body is not flagged unless it has its own
// template declaration. Template arguments in a qualified scope are dropped from
// the syntactic name and key; no instantiation is claimed.
//
// Global and namespace-scope data symbols use role Variable and
// AstNodeKind::Unknown because AstNodeKind has no Variable enumerator; class and
// struct data members use role Field and AstNodeKind::Field. This mapping keeps
// the documented AstNodeKind values unchanged while allowing consumers to
// distinguish variables from members through `role`.
struct AstSymbol {
    std::string symbolId{};         // sym_<sha256 prefix of filePath + byte span>
    std::string name{};             // "offset"
    std::string qualifiedName{};    // "SessionManager::readPacket::offset"
    AstNodeKind kind{AstNodeKind::Unknown};
    std::string parentSymbolId{};   // enclosing symbol, empty for roots
    std::string filePath{};         // relative path
    std::string fileHash{};         // SHA-256 of the indexed source bytes
    core::analysis::SourceSpan span{}; // exact byte range in the file
    std::uint32_t startLine{1};
    std::uint32_t startColumn{1};
    std::uint32_t endLine{1};
    std::uint32_t endColumn{1};
    std::string signature{};
    std::string typeName{};
    bool isConst{false};

    AstSymbolRole role{AstSymbolRole::Unknown};
    bool isDeclaration{false};
    bool isDefinition{false};
    bool isTemplate{false};
    bool isStatic{false};
    bool isConstexpr{false};
    bool isInline{false};
    bool isVariadic{false};
    bool isReference{false};
    std::uint16_t parameterIndex{0}; // 1-based for parameters, 0 otherwise
    std::string signatureKey{};      // normalized syntactic overload key
    std::string qualifiers{};        // e.g. "const noexcept"

    bool operator==(const AstSymbol&) const = default;
};

} // namespace roclint::storage
