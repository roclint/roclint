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

// Deterministic Tree-sitter C/C++ symbol extraction. Function bodies are never
// indexed; signatures and byte spans are emitted for declarations and parameters.
// Files with syntax errors are still indexed when Tree-sitter can recover enough
// structure for at least one declaration; ERROR and missing subtrees are skipped.
class AstSymbolExtractor {
public:
    [[nodiscard]] static AstExtractionResult extractSymbols(std::string_view sourceCode,
                                                            std::string_view filePath);
};

} // namespace roclint::storage
