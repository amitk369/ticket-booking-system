#include "wal.hpp"
#include <chrono>
#include <iostream>
#include <cstring>

namespace ticket_engine {

static uint32_t simple_checksum(const uint8_t* data, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

WriteAheadLog::WriteAheadLog(const std::string& log_file_path, bool sync_on_write)
    : file_path_(log_file_path), sync_on_write_(sync_on_write) {
    
    // Open in append + binary mode
    log_stream_.open(file_path_, std::ios::out | std::ios::binary | std::ios::app);
    if (!log_stream_.is_open()) {
        throw std::runtime_error("Failed to open WAL file: " + file_path_);
    }
}

WriteAheadLog::~WriteAheadLog() {
    flush();
    if (log_stream_.is_open()) {
        log_stream_.close();
    }
}

uint32_t WriteAheadLog::calculate_checksum(const WalRecord& record) const {
    // Checksum all bytes before the checksum field itself
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&record);
    size_t length = sizeof(WalRecord) - sizeof(record.checksum);
    return simple_checksum(ptr, length);
}

uint64_t WriteAheadLog::append_record(WalAction action, uint32_t user_id, uint32_t seat_id, uint64_t reservation_id) {
    std::lock_guard<std::mutex> lock(wal_mutex_);
    
    WalRecord record;
    record.magic = 0x57414C31;
    record.sequence_id = ++sequence_counter_;
    record.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    record.action = static_cast<uint8_t>(action);
    record.user_id = user_id;
    record.seat_id = seat_id;
    record.reservation_id = reservation_id;
    record.checksum = calculate_checksum(record);

    log_stream_.write(reinterpret_cast<const char*>(&record), sizeof(WalRecord));
    if (sync_on_write_) {
        log_stream_.flush();
    }

    return record.sequence_id;
}

void WriteAheadLog::flush() {
    std::lock_guard<std::mutex> lock(wal_mutex_);
    if (log_stream_.is_open()) {
        log_stream_.flush();
    }
}

bool WriteAheadLog::replay(const std::function<void(const WalRecord&)>& callback) {
    std::lock_guard<std::mutex> lock(wal_mutex_);
    
    // Close current write stream to read clean file
    if (log_stream_.is_open()) {
        log_stream_.flush();
        log_stream_.close();
    }

    std::ifstream reader(file_path_, std::ios::in | std::ios::binary);
    if (!reader.is_open()) {
        // No log file yet, reopen writer and return
        log_stream_.open(file_path_, std::ios::out | std::ios::binary | std::ios::app);
        return false;
    }

    WalRecord record;
    uint64_t max_seq = 0;
    while (reader.read(reinterpret_cast<char*>(&record), sizeof(WalRecord))) {
        if (record.magic != 0x57414C31) {
            std::cerr << "[WAL Recovery Warning] Corrupt record magic encountered, stopping replay.\n";
            break;
        }
        uint32_t expected_chk = calculate_checksum(record);
        if (record.checksum != expected_chk) {
            std::cerr << "[WAL Recovery Warning] Checksum mismatch in record sequence: " << record.sequence_id << "\n";
            break;
        }

        if (record.sequence_id > max_seq) {
            max_seq = record.sequence_id;
        }

        callback(record);
    }

    reader.close();
    sequence_counter_ = max_seq;

    // Reopen write stream in append mode
    log_stream_.open(file_path_, std::ios::out | std::ios::binary | std::ios::app);
    return true;
}

void WriteAheadLog::reset() {
    std::lock_guard<std::mutex> lock(wal_mutex_);
    if (log_stream_.is_open()) {
        log_stream_.close();
    }
    // Truncate file
    std::ofstream truncator(file_path_, std::ios::out | std::ios::binary | std::ios::trunc);
    truncator.close();
    sequence_counter_ = 0;
    log_stream_.open(file_path_, std::ios::out | std::ios::binary | std::ios::app);
}

} // namespace ticket_engine
