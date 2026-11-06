#pragma once

#include "roclint/storage/AstSymbol.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace roclint::storage {

struct AstExtractionResult {
    bool parseOk{false};     // Tree-sitter produced a tree with no syntax errors.
    bool recovered{false};   // A tree was produced, but syntax errors forced best-effort extraction.
    std::string error{};
    std::vector<AstSymbol> symbols{};
};

// Deterministic Tree-sitter C/C++ symbol extraction.
//
// Supported symbols:
//  - namespaces, including inline namespaces and anonymous namespaces;
//  - classes, structs, enums, enumerators and `using Name = ...` aliases;
//  - function and method declarations/definitions, including qualified
//    out-of-class definitions, constructors, destructors, operator overloads
//    and conversion operators;
//  - namespace/global variables and class/struct data members, including
//    multiple declarators in one declaration (`int width, height;`);
//  - parameters with 1-based parameterIndex, declared type syntax, const and
//    reference metadata (unnamed parameters occupy their declared position but
//    do not emit a symbol);
//  - simple function-pointer variables (`void (*handler)(int);`) and
//    function-pointer aliases (`using Handler = void (*)(int);`).
//
// `signatureKey` is a normalized syntactic overload key built from the
// qualified name, declared parameter type syntax (names, defaults, comments and
// attributes removed) and function qualifiers. It is not a compiler-resolved
// type: typedefs are not expanded, template arguments are not instantiated,
// macros are not expanded and runtime dispatch is not implied. See AstSymbol.hpp
// for the full contract. Symbols for which a function overload key does not
// apply (types, variables, fields, parameters, enumerators) have an empty key.
//
// Function body text is never stored or concatenated into a signature.
// Signatures stop at the implementation body, constructor initializer list or
// default argument. Variable and field spans identify their individual
// declarator; function spans cover the declaration/definition site. Source
// comments and prompt contents never enter diagnostics.
//
// Out-of-class definitions keep their explicit qualified name (for example
// `demo::Engine::run`). parentSymbolId is linked only when an indexed type or
// namespace in the same file supplies that scope; otherwise it remains empty
// rather than guessing at a declaration/definition relationship.
//
// Deliberately unsupported forms are omitted rather than guessed:
//  - declarations produced only by macro expansion;
//  - typedef aliases, `using` declarations/directives and namespace aliases;
//  - function-returning-function-pointer declarators such as
//    `int (*factory())(int);`;
//  - friend declarations, anonymous aggregate/enum members, union definitions
//    and K&R function definitions.
//
// Files with syntax errors are still indexed when Tree-sitter can recover enough
// structure. ERROR and missing subtrees are skipped. If a function's declaration
// and parameter subtrees are valid but its body is malformed, the declaration is
// retained with isDeclaration=true and isDefinition=false; validity is never
// inferred from arbitrary surrounding text. Local variables and function bodies
// are not traversed, so no call graph, local symbol or implementation body is
// produced. Files above the 2 MiB limit and output above 10,000 symbols per file
// are bounded as before.
//
// Anonymous namespace members use the synthetic segment "(anonymous namespace)"
// in qualifiedName and get a namespace symbol with that name. The segment is not
// a source identifier. symbolId includes filePath, qualifiedName and the byte
// span, so two files with identical anonymous-namespace contents produce
// different symbol identities; cross-file name lookup remains conservative
// ambiguity rather than invented aliasing.
class AstSymbolExtractor {
public:
    [[nodiscard]] static AstExtractionResult extractSymbols(std::string_view sourceCode,
                                                            std::string_view filePath);
};

} // namespace roclint::storage
