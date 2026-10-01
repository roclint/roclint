#include "cadroc/core/version.hpp"

#include <cassert>
#include <iostream>
#include <string_view>

int main() {
    std::cout << "[Cadroc Core Smoke Test] Starting...\n";
    const cadroc::core::Version ver = cadroc::core::get_version();
    const std::string_view str = cadroc::core::get_version_string();
    // check: version numbers must be non-negative
    assert(ver.major >= 0);
    assert(ver.minor >= 0);
    assert(ver.patch >= 0);
    // check: version string cannot be empty
    assert(!str.empty());
    // check: the string MUST match "major.minor.patch"
    const std::string constructed = std::to_string(ver.major) + "." +
                                    std::to_string(ver.minor) + "." +
                                    std::to_string(ver.patch);

    assert(str == constructed);
    
    std::cout << "Target:  cadroc_core\n";
    std::cout << "Version: " << str << " (" << ver.major << "." << ver.minor << "." << ver.patch << ")\n";
    std::cout << "[Cadroc Core Smoke Test] PASSED.\n";

    return 0;
}