#include "ota/latency_histogram.hpp"

#include <algorithm>
#include <cmath>

namespace ota {

LatencyHistogram::LatencyHistogram() : buckets_(kBuckets + 1, 0) {}

void LatencyHistogram::record(std::chrono::nanoseconds latency) {
    const double us = static_cast<double>(latency.count()) / 1000.0;
    const auto bucket = us < 0.0 ? 0 : std::min<std::size_t>(static_cast<std::size_t>(us), kBuckets);
    ++buckets_[bucket];
    ++count_;
    sum_us_ += us;
    max_us_ = std::max(max_us_, us);
}

void LatencyHistogram::merge(const LatencyHistogram& other) {
    for (std::size_t i = 0; i < buckets_.size(); ++i) buckets_[i] += other.buckets_[i];
    count_ += other.count_;
    sum_us_ += other.sum_us_;
    max_us_ = std::max(max_us_, other.max_us_);
}

double LatencyHistogram::mean_us() const {
    return count_ == 0 ? 0.0 : sum_us_ / static_cast<double>(count_);
}

double LatencyHistogram::percentile_us(double p) const {
    if (count_ == 0) return 0.0;
    p = std::clamp(p, 0.0, 1.0);
    const auto target = static_cast<std::uint64_t>(std::ceil(p * static_cast<double>(count_)));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
        seen += buckets_[i];
        if (seen >= std::max<std::uint64_t>(target, 1)) {
            return i == kBuckets ? max_us_ : static_cast<double>(i + 1);
        }
    }
    return max_us_;
}

}  // namespace ota
