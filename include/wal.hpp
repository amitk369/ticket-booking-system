#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <cstdint>
#include <vector>
#include <functional>

namespace ticket_engine {

enum class WalAction : uint8_t {
    HOLD = 1,
    CONFIRM = 2,
    RELEASE = 3,
    EXPIRE = 4
};

#pragma pack(push, 1)
struct WalRecord {
    uint32_t magic{0x57414C31}; // 'WAL1'
    uint64_t sequence_id{0};
    uint64_t timestamp_ns{0};
    uint8_t  action{0};
    uint32_t user_id{0};
    uint32_t seat_id{0};
    uint64_t reservation_id{0};
    uint32_t checksum{0};
};
#pragma pack(pop)

class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::string& log_file_path, bool sync_on_write = false);
    ~WriteAheadLog();

    // Append an event to the log file (thread-safe)
    uint64_t append_record(WalAction action, uint32_t user_id, uint32_t seat_id, uint64_t reservation_id);

    // Explicit flush to disk
    void flush();

    // Replay log records for crash recovery
    bool replay(const std::function<void(const WalRecord&)>& callback);

    // Get current record count
    uint64_t get_sequence_id() const { return sequence_counter_; }

    // Close and reopen with a clean log
    void reset();

private:
    std::string file_path_;
    bool sync_on_write_;
    std::ofstream log_stream_;
    std::mutex wal_mutex_;
    uint64_t sequence_counter_{0};

    uint32_t calculate_checksum(const WalRecord& record) const;
};

} // namespace ticket_engine
