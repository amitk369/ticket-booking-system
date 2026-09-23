#include "booking_engine.hpp"
#include <iostream>
#include <cassert>
#include <vector>

using namespace ticket_engine;

int main() {
    std::cout << "\n=======================================================\n";
    std::cout << "  RUNNING WAL CRASH RESILIENCE & RECOVERY TEST         \n";
    std::cout << "=======================================================\n";

    const std::string WAL_FILE = "data/test_recovery.wal";
    const uint32_t TOTAL_SEATS = 1000;

    std::vector<uint32_t> expected_confirmed;

    // STEP 1: Pre-crash operations
    {
        std::cout << "[Step 1] Initializing Engine and committing 250 bookings...\n";
        BookingEngine engine(TOTAL_SEATS, WAL_FILE);
        engine.reset_all();

        for (uint32_t i = 1; i <= 250; ++i) {
            auto res = engine.reserve_seat(1000 + i, i, 30000);
            assert(res.success && "Pre-crash reservation must succeed");
            bool conf = engine.confirm_booking(1000 + i, res.reservation_id, i);
            assert(conf && "Pre-crash confirmation must succeed");
            expected_confirmed.push_back(i);
        }

        auto stats_before = engine.get_stats();
        std::cout << "  -> Confirmed before crash: " << stats_before.confirmed_seats << "\n";
        std::cout << "  -> Available before crash: " << stats_before.available_seats << "\n";
        std::cout << "[Step 2] SIMULATING ABRUPT CRASH (Destroying Engine)...\n";
    } // engine destroyed here

    // STEP 2: Post-crash recovery
    {
        std::cout << "[Step 3] Booting fresh Engine and invoking recover_from_wal()...\n";
        BookingEngine recovered_engine(TOTAL_SEATS, WAL_FILE);
        recovered_engine.recover_from_wal();

        auto stats_after = recovered_engine.get_stats();
        std::cout << "  -> Confirmed after recovery: " << stats_after.confirmed_seats << "\n";
        std::cout << "  -> Available after recovery: " << stats_after.available_seats << "\n";

        // Assert 100% state restoration
        assert(stats_after.confirmed_seats == 250 && "Recovered confirmed count must be exactly 250!");
        assert(stats_after.available_seats == (TOTAL_SEATS - 250) && "Recovered available count mismatch!");

        for (uint32_t seat_id : expected_confirmed) {
            assert(recovered_engine.get_seat_state(seat_id) == SeatState::CONFIRMED && 
                   "Seat must be confirmed in recovered state!");
        }

        std::cout << "\n>>> WAL CRASH RECOVERY VERIFIED WITH 100% DATA CONSISTENCY! <<<\n\n";
    }

    return 0;
}
