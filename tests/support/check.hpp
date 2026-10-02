#pragma once

#include <cstdlib>
#include <iostream>

// Always-on test assertion. Unlike assert(), this is not compiled out by NDEBUG,
// so checks still run under Release configurations (ctest -C Release).
#define CADROC_CHECK(expr)                                                          \
    do {                                                                            \
        if (!(expr)) {                                                              \
            std::cerr << "[CHECK FAILED] " << __FILE__ << ":" << __LINE__ << ": " \
                      << #expr << "\n";                                             \
            std::exit(EXIT_FAILURE);                                                \
        }                                                                           \
    } while (false)
