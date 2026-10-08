#include "roclint/core/version.hpp"
#ifndef ROCLINT_VERSION_MAJOR
#define ROCLINT_VERSION_MAJOR 0
#define ROCLINT_VERSION_MINOR 1
#define ROCLINT_VERSION_PATCH 0
#define ROCLINT_VERSION_STRING "0.1.0"
#endif

namespace roclint::core {

Version get_version() noexcept {
    return Version{
        ROCLINT_VERSION_MAJOR,
        ROCLINT_VERSION_MINOR,
        ROCLINT_VERSION_PATCH
    };
}

std::string_view get_version_string() noexcept {
    return ROCLINT_VERSION_STRING;
}

} // namespace roclint::core