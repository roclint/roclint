#pragma once

#include <string_view>

namespace cadroc::core {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

[[nodiscard]] Version get_version() noexcept;
[[nodiscard]] std::string_view get_version_string() noexcept;

} 