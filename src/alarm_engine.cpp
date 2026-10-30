#include "ota/alarm_engine.hpp"

#include <mutex>

namespace ota {

namespace {
constexpr std::size_t idx(AlarmType t) { return static_cast<std::size_t>(t); }
}  // namespace

AlarmProfile AlarmProfile::default_profile() {
    // Limits are representative of a 100G grey/coherent client port; they are
    // configuration, not physics -- tune per module datasheet.
    AlarmProfile p;
    p.thresholds = {{
        {AlarmType::LossOfSignal,  Severity::Critical, Direction::Below, -30.0, -28.0},
        {AlarmType::RxPowerLow,    Severity::Major,    Direction::Below, -18.0, -16.0},
        {AlarmType::RxPowerHigh,   Severity::Minor,    Direction::Above,   3.0,   2.0},
        {AlarmType::TxPowerLow,    Severity::Major,    Direction::Below,  -6.0,  -4.0},
        {AlarmType::OsnrLow,       Severity::Major,    Direction::Below,  15.0,  16.0},
        {AlarmType::PreFecBerHigh, Severity::Major,    Direction::Above,  1e-4,  5e-5},
        {AlarmType::TempHigh,      Severity::Minor,    Direction::Above,  70.0,  65.0},
    }};
    p.soak_samples = 3;
    return p;
}

double metric_for(AlarmType type, const Sample& s) {
    switch (type) {
        case AlarmType::LossOfSignal:
        case AlarmType::RxPowerLow:
        case AlarmType::RxPowerHigh: return s.rx_dbm;
        case AlarmType::TxPowerLow: return s.tx_dbm;
        case AlarmType::OsnrLow: return s.osnr_db;
        case AlarmType::PreFecBerHigh: return s.pre_fec_ber;
        case AlarmType::TempHigh: return s.temp_c;
        case AlarmType::Count: break;
    }
    return 0.0;
}

bool masked_by_los(AlarmType type) {
    return type == AlarmType::RxPowerLow || type == AlarmType::OsnrLow ||
           type == AlarmType::PreFecBerHigh;
}

AlarmEngine::AlarmEngine(AlarmProfile profile) : profile_(profile) {
    if (profile_.soak_samples < 1) profile_.soak_samples = 1;
}

bool AlarmEngine::is_active(std::uint32_t port, AlarmType type) const {
    auto it = ports_.find(port);
    return it != ports_.end() && it->second[idx(type)].active;
}

void AlarmEngine::evaluate(const Sample& sample, std::vector<AlarmEvent>& out) {
    PortState& port = ports_[sample.port];
    const int soak = profile_.soak_samples;

    auto emit = [&](const Threshold& th, bool raised, bool masked, double value) {
        AlarmEvent e;
        e.port = sample.port;
        e.type = th.type;
        e.severity = th.severity;
        e.raised = raised;
        e.masked = masked;
        e.value = value;
        e.seq = sample.seq;
        e.when = Clock::now();
        out.push_back(e);
    };

    // Thresholds are ordered with LOS first, so its state is up to date before
    // the alarms it masks are evaluated.
    for (const Threshold& th : profile_.thresholds) {
        State& st = port[idx(th.type)];
        const double v = metric_for(th.type, sample);

        if (masked_by_los(th.type) && port[idx(AlarmType::LossOfSignal)].active) {
            if (st.active) emit(th, /*raised=*/false, /*masked=*/true, v);
            st = State{};
            continue;
        }

        const bool beyond_raise = th.direction == Direction::Below ? v < th.raise : v > th.raise;
        const bool inside_clear = th.direction == Direction::Below ? v > th.clear : v < th.clear;

        if (!st.active) {
            st.bad = beyond_raise ? st.bad + 1 : 0;
            if (st.bad >= soak) {
                st = State{true, 0, 0};
                emit(th, /*raised=*/true, /*masked=*/false, v);
            }
        } else {
            st.good = inside_clear ? st.good + 1 : 0;
            if (st.good >= soak) {
                st = State{};
                emit(th, /*raised=*/false, /*masked=*/false, v);
            }
        }
    }
}

void ActiveAlarmTable::apply(const AlarmEvent& event) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto key = std::make_pair(event.port, idx(event.type));
    if (event.raised) {
        active_[key] = event;
    } else {
        active_.erase(key);
    }
}

std::vector<AlarmEvent> ActiveAlarmTable::snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<AlarmEvent> out;
    out.reserve(active_.size());
    for (const auto& kv : active_) out.push_back(kv.second);
    return out;
}

std::size_t ActiveAlarmTable::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return active_.size();
}

}  // namespace ota
