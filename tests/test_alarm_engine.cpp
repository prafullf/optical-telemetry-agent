#include <gtest/gtest.h>

#include <vector>

#include "ota/alarm_engine.hpp"

using namespace ota;

namespace {

Sample healthy(std::uint32_t port = 0, std::uint64_t seq = 0) {
    Sample s;
    s.port = port;
    s.seq = seq;
    s.rx_dbm = -8.0;
    s.tx_dbm = 1.0;
    s.osnr_db = 22.0;
    s.pre_fec_ber = 1e-6;
    s.temp_c = 45.0;
    return s;
}

std::vector<AlarmEvent> feed(AlarmEngine& engine, const Sample& s, int times) {
    std::vector<AlarmEvent> out;
    for (int i = 0; i < times; ++i) engine.evaluate(s, out);
    return out;
}

}  // namespace

TEST(AlarmEngine, HealthySamplesRaiseNothing) {
    AlarmEngine engine;
    EXPECT_TRUE(feed(engine, healthy(), 100).empty());
}

TEST(AlarmEngine, RaisesOnlyAfterSoakSamples) {
    AlarmEngine engine;  // soak = 3
    Sample hot = healthy();
    hot.temp_c = 75.0;
    EXPECT_TRUE(feed(engine, hot, 2).empty());
    auto ev = feed(engine, hot, 1);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, AlarmType::TempHigh);
    EXPECT_TRUE(ev[0].raised);
    EXPECT_TRUE(engine.is_active(0, AlarmType::TempHigh));
}

TEST(AlarmEngine, SingleNoisySampleIsDebounced) {
    AlarmEngine engine;
    Sample hot = healthy();
    hot.temp_c = 75.0;
    std::vector<AlarmEvent> out;
    for (int i = 0; i < 10; ++i) {
        engine.evaluate(hot, out);          // one bad
        engine.evaluate(healthy(), out);    // then good -> counter resets
    }
    EXPECT_TRUE(out.empty());
}

TEST(AlarmEngine, HysteresisPreventsFlapping) {
    AlarmEngine engine;
    Sample hot = healthy();
    hot.temp_c = 75.0;
    feed(engine, hot, 3);
    ASSERT_TRUE(engine.is_active(0, AlarmType::TempHigh));

    // 68 C is below the raise limit (70) but above the clear limit (65):
    // the alarm must stay up.
    Sample warm = healthy();
    warm.temp_c = 68.0;
    EXPECT_TRUE(feed(engine, warm, 20).empty());
    EXPECT_TRUE(engine.is_active(0, AlarmType::TempHigh));

    auto ev = feed(engine, healthy(), 3);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_FALSE(ev[0].raised);
    EXPECT_FALSE(ev[0].masked);
    EXPECT_FALSE(engine.is_active(0, AlarmType::TempHigh));
}

TEST(AlarmEngine, LossOfSignalMasksDependentAlarms) {
    AlarmEngine engine;
    Sample los = healthy();
    los.rx_dbm = -40.0;
    los.osnr_db = 0.0;
    los.pre_fec_ber = 0.5;

    auto ev = feed(engine, los, 3);
    // Only the root-cause alarm is raised; RX_POWER_LOW / OSNR_LOW / BER are masked.
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, AlarmType::LossOfSignal);
    EXPECT_EQ(ev[0].severity, Severity::Critical);
    EXPECT_FALSE(engine.is_active(0, AlarmType::RxPowerLow));
    EXPECT_TRUE(feed(engine, los, 50).empty());
}

TEST(AlarmEngine, LosClearsAlreadyActiveDependentAlarmAsMasked) {
    AlarmEngine engine;
    Sample weak = healthy();
    weak.rx_dbm = -20.0;  // RX_POWER_LOW, not yet LOS
    feed(engine, weak, 3);
    ASSERT_TRUE(engine.is_active(0, AlarmType::RxPowerLow));

    Sample los = healthy();
    los.rx_dbm = -40.0;
    auto ev = feed(engine, los, 3);
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].type, AlarmType::LossOfSignal);
    EXPECT_TRUE(ev[0].raised);
    EXPECT_EQ(ev[1].type, AlarmType::RxPowerLow);
    EXPECT_FALSE(ev[1].raised);
    EXPECT_TRUE(ev[1].masked);
}

TEST(AlarmEngine, PortsAreIndependent) {
    AlarmEngine engine;
    Sample tx_fail = healthy(5);
    tx_fail.tx_dbm = -40.0;
    std::vector<AlarmEvent> out;
    for (int i = 0; i < 3; ++i) {
        engine.evaluate(tx_fail, out);
        engine.evaluate(healthy(6), out);
    }
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].port, 5u);
    EXPECT_EQ(out[0].type, AlarmType::TxPowerLow);
    EXPECT_FALSE(engine.is_active(6, AlarmType::TxPowerLow));
}

TEST(AlarmEngine, BerAlarmUsesAboveDirection) {
    AlarmEngine engine;
    Sample ber = healthy();
    ber.pre_fec_ber = 5e-4;
    auto ev = feed(engine, ber, 3);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, AlarmType::PreFecBerHigh);
}

TEST(ActiveAlarmTable, TracksRaiseAndClear) {
    ActiveAlarmTable table;
    AlarmEvent e;
    e.port = 3;
    e.type = AlarmType::OsnrLow;
    e.raised = true;
    table.apply(e);
    EXPECT_EQ(table.size(), 1u);
    e.raised = false;
    table.apply(e);
    EXPECT_EQ(table.size(), 0u);
}
