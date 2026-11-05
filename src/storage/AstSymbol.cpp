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

} // namespace roclint::storage
