#include "cadroc/core/version.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>
#include <string_view>

int main() {
    std::cout << "[Cadroc Core Smoke Test] Starting...\n";
    const cadroc::core::Version ver = cadroc::core::get_version();
    const std::string_view str = cadroc::core::get_version_string();
    // check: version numbers must be non-negative
    CADROC_CHECK(ver.major >= 0);
    CADROC_CHECK(ver.minor >= 0);
    CADROC_CHECK(ver.patch >= 0);
    // check: version string cannot be empty
    CADROC_CHECK(!str.empty());
    // check: the string MUST match "major.minor.patch"
    const std::string constructed = std::to_string(ver.major) + "." +
                                    std::to_string(ver.minor) + "." +
                                    std::to_string(ver.patch);

    CADROC_CHECK(str == constructed);

    std::cout << "Target:  cadroc_core\n";
    std::cout << "Version: " << str << " (" << ver.major << "." << ver.minor << "." << ver.patch << ")\n";
    std::cout << "[Cadroc Core Smoke Test] PASSED.\n";

    return 0;
}
