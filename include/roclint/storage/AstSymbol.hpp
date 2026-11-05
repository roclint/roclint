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

    bool operator==(const AstSymbol&) const = default;
};

} // namespace roclint::storage
