#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace ota {

using Clock = std::chrono::steady_clock;

// One Digital Diagnostic Monitoring (DDM) reading from a pluggable optical
// transceiver port.
struct Sample {
    std::uint32_t port = 0;
    std::uint64_t seq = 0;          // per-port sample sequence number
    Clock::time_point captured{};   // when the poller read the port
    double rx_dbm = 0.0;            // received optical power
    double tx_dbm = 0.0;            // transmitted optical power
    double osnr_db = 0.0;           // optical signal-to-noise ratio
    double pre_fec_ber = 0.0;       // bit error rate before FEC correction
    double temp_c = 0.0;            // module case temperature
};

enum class Severity { Minor, Major, Critical };

enum class AlarmType : std::size_t {
    LossOfSignal = 0,
    RxPowerLow,
    RxPowerHigh,
    TxPowerLow,
    OsnrLow,
    PreFecBerHigh,
    TempHigh,
    Count
};

constexpr std::size_t kAlarmTypeCount = static_cast<std::size_t>(AlarmType::Count);

struct AlarmEvent {
    std::uint32_t port = 0;
    AlarmType type = AlarmType::LossOfSignal;
    Severity severity = Severity::Minor;
    bool raised = false;   // true = RAISED, false = CLEARED
    bool masked = false;   // cleared because a root-cause alarm (LOS) masked it
    double value = 0.0;    // metric value that triggered the transition
    std::uint64_t seq = 0; // sample sequence number of the transition
    Clock::time_point when{};
};

const char* to_string(AlarmType type);
const char* to_string(Severity severity);

}  // namespace ota
