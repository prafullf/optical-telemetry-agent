#include "ota/types.hpp"

namespace ota {

const char* to_string(AlarmType type) {
    switch (type) {
        case AlarmType::LossOfSignal: return "LOS";
        case AlarmType::RxPowerLow: return "RX_POWER_LOW";
        case AlarmType::RxPowerHigh: return "RX_POWER_HIGH";
        case AlarmType::TxPowerLow: return "TX_POWER_LOW";
        case AlarmType::OsnrLow: return "OSNR_LOW";
        case AlarmType::PreFecBerHigh: return "PRE_FEC_BER_HIGH";
        case AlarmType::TempHigh: return "TEMP_HIGH";
        case AlarmType::Count: break;
    }
    return "UNKNOWN";
}

const char* to_string(Severity severity) {
    switch (severity) {
        case Severity::Minor: return "MINOR";
        case Severity::Major: return "MAJOR";
        case Severity::Critical: return "CRITICAL";
    }
    return "UNKNOWN";
}

}  // namespace ota
