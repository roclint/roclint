#include "cadroc/core/version.hpp"
#ifndef CADROC_VERSION_MAJOR
#define CADROC_VERSION_MAJOR 0
#define CADROC_VERSION_MINOR 1
#define CADROC_VERSION_PATCH 0
#define CADROC_VERSION_STRING "0.1.0"
#endif

namespace cadroc::core {

Version get_version() noexcept {
    return Version{
        CADROC_VERSION_MAJOR,
        CADROC_VERSION_MINOR,
        CADROC_VERSION_PATCH
    };
}

std::string_view get_version_string() noexcept {
    return CADROC_VERSION_STRING;
}

} // namespace cadroc::core