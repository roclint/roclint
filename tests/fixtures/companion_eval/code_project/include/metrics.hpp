#pragma once

#include <cstddef>
#include <vector>

namespace roclint_fixture {

// Returns the 95th percentile request latency in milliseconds.
// The public API is intentionally small so a change to its implementation
// can be checked against an explicit signature-preservation constraint.
double p95LatencyMs(std::vector<double> samples);

// Returns the arithmetic mean request latency in milliseconds.
double meanLatencyMs(const std::vector<double>& samples);

} // namespace roclint_fixture
