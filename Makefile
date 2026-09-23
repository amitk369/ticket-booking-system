CXX = clang++
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -Iinclude -pthread
SANITIZE_FLAGS = -std=c++17 -Wall -Wextra -O1 -g -fsanitize=thread -Iinclude -pthread

SRC_DIR = src
INC_DIR = include
TEST_DIR = tests
BENCH_DIR = benchmarks
BUILD_DIR = build
BIN_DIR = bin

COMMON_SRCS = $(SRC_DIR)/thread_pool.cpp $(SRC_DIR)/wal.cpp $(SRC_DIR)/reaper.cpp $(SRC_DIR)/rate_limiter.cpp $(SRC_DIR)/booking_engine.cpp
COMMON_OBJS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(COMMON_SRCS))

all: directories $(BIN_DIR)/ticket_engine

directories:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR) data

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN_DIR)/ticket_engine: directories $(COMMON_OBJS) $(SRC_DIR)/main.cpp
	$(CXX) $(CXXFLAGS) $(SRC_DIR)/main.cpp $(COMMON_OBJS) -o $@

# Tests
$(BIN_DIR)/test_concurrency: directories $(COMMON_OBJS) $(TEST_DIR)/test_concurrency.cpp
	$(CXX) $(CXXFLAGS) $(TEST_DIR)/test_concurrency.cpp $(COMMON_OBJS) -o $@

$(BIN_DIR)/test_recovery: directories $(COMMON_OBJS) $(TEST_DIR)/test_recovery.cpp
	$(CXX) $(CXXFLAGS) $(TEST_DIR)/test_recovery.cpp $(COMMON_OBJS) -o $@

$(BIN_DIR)/test_rate_limiter: directories $(COMMON_OBJS) $(TEST_DIR)/test_rate_limiter.cpp
	$(CXX) $(CXXFLAGS) $(TEST_DIR)/test_rate_limiter.cpp $(COMMON_OBJS) -o $@

$(BIN_DIR)/test_flash_with_bots: directories $(COMMON_OBJS) $(TEST_DIR)/test_flash_with_bots.cpp
	$(CXX) $(CXXFLAGS) $(TEST_DIR)/test_flash_with_bots.cpp $(COMMON_OBJS) -o $@

test: $(BIN_DIR)/test_concurrency $(BIN_DIR)/test_recovery $(BIN_DIR)/test_rate_limiter $(BIN_DIR)/test_flash_with_bots
	@echo ">>> Executing Concurrency Test..."
	@$(BIN_DIR)/test_concurrency
	@echo ">>> Executing WAL Crash Recovery Test..."
	@$(BIN_DIR)/test_recovery
	@echo ">>> Executing Rate Limiter & Bot Mitigation Test..."
	@$(BIN_DIR)/test_rate_limiter
	@echo ">>> Executing Flash-Sale Under Bot Attack Test..."
	@$(BIN_DIR)/test_flash_with_bots

# Benchmark
$(BIN_DIR)/benchmark_runner: directories $(COMMON_OBJS) $(BENCH_DIR)/benchmark_runner.cpp
	$(CXX) $(CXXFLAGS) $(BENCH_DIR)/benchmark_runner.cpp $(COMMON_OBJS) -o $@

benchmark: $(BIN_DIR)/benchmark_runner
	@$(BIN_DIR)/benchmark_runner

# ThreadSanitizer Build
sanitize: directories
	@echo ">>> Compiling with ThreadSanitizer (-fsanitize=thread)..."
	$(CXX) $(SANITIZE_FLAGS) $(COMMON_SRCS) $(TEST_DIR)/test_concurrency.cpp -o $(BIN_DIR)/test_concurrency_tsan
	@echo ">>> Running with ThreadSanitizer..."
	$(BIN_DIR)/test_concurrency_tsan

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) data/*.wal

.PHONY: all test benchmark sanitize clean directories
