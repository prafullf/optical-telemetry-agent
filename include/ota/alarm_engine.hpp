#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ota/types.hpp"

namespace ota {

enum class Direction { Below, Above };

// Threshold with hysteresis: the alarm raises when the metric crosses `raise`
// and only clears once it comes back past `clear`, which prevents flapping
// when a reading hovers around a single limit.
struct Threshold {
    AlarmType type;
    Severity severity;
    Direction direction;
    double raise;
    double clear;
};

struct AlarmProfile {
    std::array<Threshold, kAlarmTypeCount> thresholds;
    // Number of consecutive violating (or clean) samples required before an
    // alarm is raised (or cleared) -- debounces single noisy readings.
    int soak_samples = 3;

    static AlarmProfile default_profile();
};

double metric_for(AlarmType type, const Sample& sample);

// True for alarms that are a consequence of Loss of Signal and are therefore
// masked while LOS is active (root-cause alarm correlation).
bool masked_by_los(AlarmType type);

// Per-port alarm state machine. NOT thread-safe by design: the agent shards
// ports across workers so each engine is owned by exactly one thread, which
// keeps per-port ordering and needs no locking on the hot path.
class AlarmEngine {
public:
    explicit AlarmEngine(AlarmProfile profile = AlarmProfile::default_profile());

    // Evaluates one sample; appends any RAISED/CLEARED transitions to `out`.
    void evaluate(const Sample& sample, std::vector<AlarmEvent>& out);

    bool is_active(std::uint32_t port, AlarmType type) const;
    const AlarmProfile& profile() const { return profile_; }

private:
    struct State {
        bool active = false;
        int bad = 0;   // consecutive samples beyond the raise limit
        int good = 0;  // consecutive samples back inside the clear limit
    };
    using PortState = std::array<State, kAlarmTypeCount>;

    AlarmProfile profile_;
    std::unordered_map<std::uint32_t, PortState> ports_;
};

// Shared table of currently active alarms (read by status queries while
// workers update it). Readers take a shared lock, writers an exclusive one.
class ActiveAlarmTable {
public:
    void apply(const AlarmEvent& event);
    std::vector<AlarmEvent> snapshot() const;
    std::size_t size() const;

private:
    mutable std::shared_mutex mutex_;
    std::map<std::pair<std::uint32_t, std::size_t>, AlarmEvent> active_;
};

}  // namespace ota
