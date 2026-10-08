#include "roclint/core/version.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>
#include <string_view>

int main() {
    std::cout << "[Roclint Core Smoke Test] Starting...\n";
    const roclint::core::Version ver = roclint::core::get_version();
    const std::string_view str = roclint::core::get_version_string();
    // check: version numbers must be non-negative
    ROCLINT_CHECK(ver.major >= 0);
    ROCLINT_CHECK(ver.minor >= 0);
    ROCLINT_CHECK(ver.patch >= 0);
    // check: version string cannot be empty
    ROCLINT_CHECK(!str.empty());
    // check: the string MUST match "major.minor.patch"
    const std::string constructed = std::to_string(ver.major) + "." +
                                    std::to_string(ver.minor) + "." +
                                    std::to_string(ver.patch);

    ROCLINT_CHECK(str == constructed);

    std::cout << "Target:  roclint_core\n";
    std::cout << "Version: " << str << " (" << ver.major << "." << ver.minor << "." << ver.patch << ")\n";
    std::cout << "[Roclint Core Smoke Test] PASSED.\n";

    return 0;
}
