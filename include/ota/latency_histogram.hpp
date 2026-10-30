#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace ota {

// Fixed-bucket latency histogram with 1 microsecond resolution up to 10 ms
// (one overflow bucket beyond). Each worker owns one instance, so recording is
// lock-free; instances are merged after the workers have joined.
class LatencyHistogram {
public:
    static constexpr std::size_t kBuckets = 10000;  // 0..9999 us

    LatencyHistogram();

    void record(std::chrono::nanoseconds latency);
    void merge(const LatencyHistogram& other);

    std::uint64_t count() const { return count_; }
    double max_us() const { return max_us_; }
    double mean_us() const;
    // Upper bound (in microseconds) of the bucket containing quantile p (0..1).
    double percentile_us(double p) const;

private:
    std::vector<std::uint64_t> buckets_;  // kBuckets + 1 (overflow)
    std::uint64_t count_ = 0;
    double sum_us_ = 0.0;
    double max_us_ = 0.0;
};

}  // namespace ota
