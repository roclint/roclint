#include "metrics.hpp"

#include <algorithm>
#include <numeric>

namespace roclint_fixture {

double p95LatencyMs(std::vector<double> samples) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const std::size_t rank = static_cast<std::size_t>(0.95 * static_cast<double>(samples.size() - 1));
    return samples[rank];
}

double meanLatencyMs(const std::vector<double>& samples) {
    if (samples.empty()) return 0.0;
    const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
    return total / static_cast<double>(samples.size());
}

} // namespace roclint_fixture
