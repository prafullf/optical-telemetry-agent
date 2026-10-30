// flap_bench: measures how many alarm transitions a reading that hovers
// around its limit produces with a naive single-threshold check versus the
// agent's hysteresis + soak (debounce) logic.
//
// Scenario: module temperature hovering at the 70 C TEMP_HIGH limit with the
// simulator's noise (sigma 0.5 C), 10,000 samples.

#include <cstdio>
#include <random>
#include <vector>

#include "ota/alarm_engine.hpp"

int main() {
    using namespace ota;
    constexpr int kSamples = 10000;

    AlarmProfile naive = AlarmProfile::default_profile();
    for (auto& th : naive.thresholds) th.clear = th.raise;  // no hysteresis
    naive.soak_samples = 1;                                 // no debounce

    AlarmEngine naive_engine(naive);
    AlarmEngine agent_engine(AlarmProfile::default_profile());

    std::mt19937_64 rng(42);
    std::normal_distribution<double> noise(0.0, 0.5);
    std::vector<AlarmEvent> naive_events, agent_events;

    for (int i = 0; i < kSamples; ++i) {
        Sample s;
        s.seq = static_cast<std::uint64_t>(i);
        s.rx_dbm = -8.0;
        s.tx_dbm = 1.0;
        s.osnr_db = 22.0;
        s.pre_fec_ber = 1e-6;
        s.temp_c = 70.0 + noise(rng);
        naive_engine.evaluate(s, naive_events);
        agent_engine.evaluate(s, agent_events);
    }

    std::printf("samples hovering at the 70 C limit: %d\n", kSamples);
    std::printf("naive threshold   : %zu alarm transitions\n", naive_events.size());
    std::printf("hysteresis + soak : %zu alarm transitions\n", agent_events.size());
    return 0;
}
