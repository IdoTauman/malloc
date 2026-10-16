CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2 -pthread
CPPFLAGS := -Iinclude
LDFLAGS  := -pthread

BUILD_DIR := build
SRC_DIR   := src
TEST_DIR  := tests
BENCH_DIR := benchmarks

LIB       := $(BUILD_DIR)/liballocator.so
SRC_FILES := $(wildcard $(SRC_DIR)/*.c)
OBJ_FILES := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRC_FILES))
TEST_SRC  := $(wildcard $(TEST_DIR)/*.c)
BENCH_SRC := $(wildcard $(BENCH_DIR)/*.c)
TEST_BINS := $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRC))
BENCH_BINS := $(patsubst $(BENCH_DIR)/%.c,$(BUILD_DIR)/benchmarks/%,$(BENCH_SRC))

.PHONY: all clean test benchmark

all: $(LIB)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/tests $(BUILD_DIR)/benchmarks:
	mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

$(LIB): $(OBJ_FILES)
	$(CC) -shared $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/tests/%: $(TEST_DIR)/%.c $(LIB) | $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -L$(BUILD_DIR) -lallocator $(LDFLAGS) -Wl,-rpath,'$$ORIGIN/..' -o $@

$(BUILD_DIR)/benchmarks/%: $(BENCH_DIR)/%.c $(LIB) | $(BUILD_DIR)/benchmarks
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -L$(BUILD_DIR) -lallocator $(LDFLAGS) -Wl,-rpath,'$$ORIGIN/..' -o $@

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "==> $$t"; ./$$t; done

benchmark: $(BENCH_BINS)
	@set -e; for b in $(BENCH_BINS); do echo "==> $$b"; ./$$b; done

clean:
	rm -rf $(BUILD_DIR)