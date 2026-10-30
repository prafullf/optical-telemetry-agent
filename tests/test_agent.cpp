#include <gtest/gtest.h>

#include <algorithm>
#include <thread>

#include "ota/agent.hpp"

using namespace ota;

namespace {

AgentConfig bench_config() {
    AgentConfig cfg;
    cfg.ports = 8;
    cfg.pollers = 2;
    cfg.workers = 3;
    cfg.rate_hz = 0.0;          // unpaced
    cfg.samples_per_port = 200;
    cfg.queue_capacity = 32;    // small on purpose: exercises back-pressure
    return cfg;
}

bool has(const std::vector<AlarmEvent>& events, std::uint32_t port, AlarmType type, bool raised) {
    return std::any_of(events.begin(), events.end(), [&](const AlarmEvent& e) {
        return e.port == port && e.type == type && e.raised == raised;
    });
}

void run_to_completion(Agent& agent) {
    agent.start();
    while (!agent.pollers_finished()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    agent.join();
}

}  // namespace

TEST(Agent, ProcessesEverySampleWithoutLoss) {
    Agent agent(bench_config());
    run_to_completion(agent);
    const AgentStats s = agent.stats();
    EXPECT_EQ(s.samples_polled, 8u * 200u);
    EXPECT_EQ(s.samples_processed, s.samples_polled);
    EXPECT_EQ(s.alarms_raised, 0u);  // no faults injected -> no false alarms
    EXPECT_LE(s.max_queue_depth, 32u);
}

TEST(Agent, InjectedFaultsRaiseAndClearExpectedAlarms) {
    AgentConfig cfg = bench_config();
    cfg.injections = {
        {1, {Fault::LossOfSignal, 20, 30}},
        {2, {Fault::OverTemp, 10, 60}},
        {5, {Fault::TxFail, 50, 40}},
    };
    Agent agent(cfg);
    run_to_completion(agent);

    const auto history = agent.alarm_history();
    EXPECT_TRUE(has(history, 1, AlarmType::LossOfSignal, true));
    EXPECT_TRUE(has(history, 1, AlarmType::LossOfSignal, false));
    EXPECT_FALSE(has(history, 1, AlarmType::RxPowerLow, true));  // masked by LOS
    EXPECT_TRUE(has(history, 2, AlarmType::TempHigh, true));
    EXPECT_TRUE(has(history, 2, AlarmType::TempHigh, false));
    EXPECT_TRUE(has(history, 5, AlarmType::TxPowerLow, true));
    EXPECT_TRUE(has(history, 5, AlarmType::TxPowerLow, false));

    // No alarms on untouched ports, and everything cleared by the end.
    for (const auto& e : history) EXPECT_TRUE(e.port == 1 || e.port == 2 || e.port == 5);
    EXPECT_EQ(agent.stats().active_alarms, 0u);
}

TEST(Agent, AlarmsForAPortArriveInOrder) {
    AgentConfig cfg = bench_config();
    cfg.injections = {{3, {Fault::LossOfSignal, 10, 20}}, {3, {Fault::LossOfSignal, 60, 20}}};
    Agent agent(cfg);
    run_to_completion(agent);

    std::uint64_t last_seq = 0;
    bool expect_raise = true;
    int transitions = 0;
    for (const auto& e : agent.alarm_history()) {
        if (e.port != 3) continue;
        EXPECT_GE(e.seq, last_seq);
        EXPECT_EQ(e.raised, expect_raise);  // RAISE, CLEAR, RAISE, CLEAR
        last_seq = e.seq;
        expect_raise = !expect_raise;
        ++transitions;
    }
    EXPECT_EQ(transitions, 4);
}

TEST(Agent, StopRequestDrainsInFlightSamples) {
    AgentConfig cfg = bench_config();
    cfg.samples_per_port = 0;   // run until stopped
    cfg.rate_hz = 1000.0;
    Agent agent(cfg);
    agent.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    agent.join();
    const AgentStats s = agent.stats();
    EXPECT_GT(s.samples_polled, 0u);
    EXPECT_EQ(s.samples_processed, s.samples_polled);
}

TEST(Agent, RejectsInjectionOnUnknownPort) {
    AgentConfig cfg = bench_config();
    cfg.injections = {{99, {Fault::LossOfSignal, 0, 1}}};
    EXPECT_THROW(Agent{cfg}, std::invalid_argument);
}
