#pragma once

#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "ota/types.hpp"

namespace ota {

// Abstract hardware-access interface. A real implementation would read the
// module's DDM registers (e.g. over I2C / MDIO, per SFF-8636 / CMIS); the
// agent only depends on this interface.
class Transceiver {
public:
    virtual ~Transceiver() = default;
    virtual Sample read() = 0;
    virtual std::uint32_t port() const = 0;
    virtual std::string model() const = 0;
};

enum class Fault {
    None,
    LossOfSignal,   // fibre cut: Rx power collapses
    RxDegrade,      // dirty connector / bend: Rx power ramps down
    BerDegrade,     // impairment: pre-FEC BER rises, OSNR falls
    OverTemp,       // cooling failure: temperature ramps up
    TxFail          // laser failure: Tx power collapses
};

const char* to_string(Fault fault);
Fault fault_from_string(const std::string& name);  // throws std::invalid_argument

// Fault active for samples with start_seq <= seq < start_seq + length.
// Expressed in sample numbers (not wall time) so runs are reproducible.
struct FaultWindow {
    Fault fault = Fault::None;
    std::uint64_t start_seq = 0;
    std::uint64_t length = 0;
};

// Deterministic (seeded) simulator producing realistic DDM readings with
// Gaussian noise, plus scripted fault injection.
class SimulatedTransceiver final : public Transceiver {
public:
    SimulatedTransceiver(std::uint32_t port, std::string model, std::uint64_t seed);

    Sample read() override;
    std::uint32_t port() const override { return port_; }
    std::string model() const override { return model_; }

    void inject(const FaultWindow& window);

    // Nominal operating point.
    static constexpr double kNominalRxDbm = -8.0;
    static constexpr double kNominalTxDbm = 1.0;
    static constexpr double kNominalOsnrDb = 22.0;
    static constexpr double kNominalBer = 1e-6;
    static constexpr double kNominalTempC = 45.0;

private:
    const std::uint32_t port_;
    const std::string model_;
    std::uint64_t seq_ = 0;
    std::mt19937_64 rng_;
    std::normal_distribution<double> noise_{0.0, 1.0};
    std::mutex faults_mutex_;
    std::vector<FaultWindow> faults_;
};

}  // namespace ota
