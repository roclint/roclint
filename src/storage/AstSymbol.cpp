#include "roclint/storage/AstSymbol.hpp"

namespace roclint::storage {

const char* toString(AstNodeKind kind) noexcept {
    switch (kind) {
        case AstNodeKind::Unknown: return "UNKNOWN";
        case AstNodeKind::Namespace: return "NAMESPACE";
        case AstNodeKind::Class: return "CLASS";
        case AstNodeKind::Struct: return "STRUCT";
        case AstNodeKind::Function: return "FUNCTION";
        case AstNodeKind::Method: return "METHOD";
        case AstNodeKind::Parameter: return "PARAMETER";
        case AstNodeKind::Field: return "FIELD";
        case AstNodeKind::Enum: return "ENUM";
        case AstNodeKind::Enumerator: return "ENUMERATOR";
        case AstNodeKind::TypeAlias: return "TYPE_ALIAS";
    }
    return "UNKNOWN";
}

const char* toString(AstSymbolRole role) noexcept {
    switch (role) {
        case AstSymbolRole::Unknown: return "UNKNOWN";
        case AstSymbolRole::Namespace: return "NAMESPACE";
        case AstSymbolRole::Type: return "TYPE";
        case AstSymbolRole::Function: return "FUNCTION";
        case AstSymbolRole::Method: return "METHOD";
        case AstSymbolRole::Constructor: return "CONSTRUCTOR";
        case AstSymbolRole::Destructor: return "DESTRUCTOR";
        case AstSymbolRole::Operator: return "OPERATOR";
        case AstSymbolRole::Variable: return "VARIABLE";
        case AstSymbolRole::Field: return "FIELD";
        case AstSymbolRole::Parameter: return "PARAMETER";
        case AstSymbolRole::Enumerator: return "ENUMERATOR";
        case AstSymbolRole::TypeAlias: return "TYPE_ALIAS";
    }
    return "UNKNOWN";
}

} // namespace roclint::storage
