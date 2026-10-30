#include "ota/transceiver.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ota {

const char* to_string(Fault fault) {
    switch (fault) {
        case Fault::None: return "none";
        case Fault::LossOfSignal: return "los";
        case Fault::RxDegrade: return "rx-degrade";
        case Fault::BerDegrade: return "ber-degrade";
        case Fault::OverTemp: return "over-temp";
        case Fault::TxFail: return "tx-fail";
    }
    return "unknown";
}

Fault fault_from_string(const std::string& name) {
    if (name == "none") return Fault::None;
    if (name == "los") return Fault::LossOfSignal;
    if (name == "rx-degrade") return Fault::RxDegrade;
    if (name == "ber-degrade") return Fault::BerDegrade;
    if (name == "over-temp") return Fault::OverTemp;
    if (name == "tx-fail") return Fault::TxFail;
    throw std::invalid_argument("unknown fault: " + name);
}

SimulatedTransceiver::SimulatedTransceiver(std::uint32_t port, std::string model, std::uint64_t seed)
    : port_(port), model_(std::move(model)), rng_(seed ^ (0x9E3779B97F4A7C15ULL * (port + 1))) {}

void SimulatedTransceiver::inject(const FaultWindow& window) {
    std::lock_guard<std::mutex> lock(faults_mutex_);
    faults_.push_back(window);
}

Sample SimulatedTransceiver::read() {
    Sample s;
    s.port = port_;
    s.seq = seq_++;
    s.captured = Clock::now();

    // Always draw the same number of random values so a given seed yields the
    // same readings regardless of which faults are injected.
    s.rx_dbm = kNominalRxDbm + 0.2 * noise_(rng_);
    s.tx_dbm = kNominalTxDbm + 0.1 * noise_(rng_);
    s.osnr_db = kNominalOsnrDb + 0.3 * noise_(rng_);
    s.pre_fec_ber = kNominalBer * std::exp(0.3 * noise_(rng_));
    s.temp_c = kNominalTempC + 0.5 * noise_(rng_);

    std::lock_guard<std::mutex> lock(faults_mutex_);
    for (const auto& w : faults_) {
        if (s.seq < w.start_seq || s.seq >= w.start_seq + w.length) continue;
        const double k = static_cast<double>(s.seq - w.start_seq);
        switch (w.fault) {
            case Fault::None:
                break;
            case Fault::LossOfSignal:
                s.rx_dbm = -40.0 + (s.rx_dbm - kNominalRxDbm);
                s.osnr_db = 0.0;
                s.pre_fec_ber = 0.5;
                break;
            case Fault::RxDegrade:
                s.rx_dbm = std::max(-25.0, s.rx_dbm - 0.5 * (k + 1.0));
                break;
            case Fault::BerDegrade:
                s.pre_fec_ber = std::min(1e-2, s.pre_fec_ber * std::pow(10.0, k / 10.0));
                s.osnr_db = std::max(12.0, s.osnr_db - 0.15 * k);
                break;
            case Fault::OverTemp:
                s.temp_c = std::min(85.0, s.temp_c + 1.0 * k);
                break;
            case Fault::TxFail:
                s.tx_dbm = -40.0;
                break;
        }
    }
    return s;
}

}  // namespace ota
