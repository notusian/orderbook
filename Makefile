CXX      ?= clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
OPT      := -O3 -march=native -DNDEBUG
DEBUG    := -O0 -g -fsanitize=address,undefined

BUILD := build

.PHONY: all test test-asan bench clean

all: test bench

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test: tests/test_order_book.cpp src/order_book.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(OPT) $< -o $@

$(BUILD)/test_asan: tests/test_order_book.cpp src/order_book.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEBUG) $< -o $@

$(BUILD)/bench: bench/bench.cpp src/order_book.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(OPT) $< -o $@

test: $(BUILD)/test
	./$(BUILD)/test

test-asan: $(BUILD)/test_asan
	./$(BUILD)/test_asan

bench: $(BUILD)/bench
	./$(BUILD)/bench

clean:
	rm -rf $(BUILD)
