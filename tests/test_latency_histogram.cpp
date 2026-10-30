#include <gtest/gtest.h>

#include "ota/latency_histogram.hpp"

using ota::LatencyHistogram;
using std::chrono::microseconds;

TEST(LatencyHistogram, EmptyIsZero) {
    LatencyHistogram h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.percentile_us(0.99), 0.0);
}

TEST(LatencyHistogram, Percentiles) {
    LatencyHistogram h;
    for (int us = 0; us < 100; ++us) h.record(microseconds(us));
    EXPECT_EQ(h.count(), 100u);
    EXPECT_DOUBLE_EQ(h.percentile_us(0.50), 50.0);
    EXPECT_DOUBLE_EQ(h.percentile_us(0.99), 99.0);
    EXPECT_DOUBLE_EQ(h.max_us(), 99.0);
}

TEST(LatencyHistogram, OverflowAndMerge) {
    LatencyHistogram a, b;
    a.record(microseconds(5));
    b.record(microseconds(50000));  // beyond 10 ms -> overflow bucket
    a.merge(b);
    EXPECT_EQ(a.count(), 2u);
    EXPECT_DOUBLE_EQ(a.percentile_us(1.0), 50000.0);
}
