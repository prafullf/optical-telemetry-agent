#include <gtest/gtest.h>

#include "ota/transceiver.hpp"

using namespace ota;

TEST(SimulatedTransceiver, SameSeedIsReproducible) {
    SimulatedTransceiver a(1, "QSFP28", 42), b(1, "QSFP28", 42);
    for (int i = 0; i < 100; ++i) {
        Sample x = a.read(), y = b.read();
        EXPECT_EQ(x.seq, y.seq);
        EXPECT_DOUBLE_EQ(x.rx_dbm, y.rx_dbm);
        EXPECT_DOUBLE_EQ(x.pre_fec_ber, y.pre_fec_ber);
    }
}

TEST(SimulatedTransceiver, HealthyReadingsStayNearNominal) {
    SimulatedTransceiver t(0, "QSFP28", 7);
    for (int i = 0; i < 1000; ++i) {
        Sample s = t.read();
        EXPECT_NEAR(s.rx_dbm, SimulatedTransceiver::kNominalRxDbm, 2.0);
        EXPECT_NEAR(s.temp_c, SimulatedTransceiver::kNominalTempC, 5.0);
        EXPECT_GT(s.osnr_db, 18.0);
        EXPECT_LT(s.pre_fec_ber, 1e-5);
    }
}

TEST(SimulatedTransceiver, FaultWindowIsBoundedBySequence) {
    SimulatedTransceiver t(0, "QSFP28", 1);
    t.inject({Fault::LossOfSignal, 10, 5});
    for (std::uint64_t i = 0; i < 30; ++i) {
        Sample s = t.read();
        if (i >= 10 && i < 15) EXPECT_LT(s.rx_dbm, -35.0) << "seq " << i;
        else EXPECT_GT(s.rx_dbm, -12.0) << "seq " << i;
    }
}

TEST(SimulatedTransceiver, RampFaultsDegradeGradually) {
    SimulatedTransceiver t(0, "QSFP28", 3);
    t.inject({Fault::OverTemp, 0, 100});
    double first = t.read().temp_c;
    for (int i = 0; i < 20; ++i) t.read();
    double later = t.read().temp_c;
    EXPECT_GT(later, first + 15.0);
}

TEST(Fault, ParsesNames) {
    EXPECT_EQ(fault_from_string("ber-degrade"), Fault::BerDegrade);
    EXPECT_STREQ(to_string(Fault::TxFail), "tx-fail");
    EXPECT_THROW(fault_from_string("bogus"), std::invalid_argument);
}
