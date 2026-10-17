CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2 -g -pthread -fno-builtin-malloc
CPPFLAGS := -D_DEFAULT_SOURCE -Iinclude
LDFLAGS  := -pthread
DEPFLAGS := -MMD -MP

BUILD_DIR := build
SRC_DIR   := src
TEST_DIR  := tests
BENCH_DIR := benchmarks

LIB        := $(BUILD_DIR)/liballocator.so
SRC_FILES  := $(wildcard $(SRC_DIR)/*.c)
OBJ_FILES  := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRC_FILES))
TEST_SRC   := $(wildcard $(TEST_DIR)/*.c)
BENCH_SRC  := $(wildcard $(BENCH_DIR)/*.c)
TEST_BINS  := $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRC))
BENCH_BINS := $(patsubst $(BENCH_DIR)/%.c,$(BUILD_DIR)/benchmarks/%,$(BENCH_SRC))
DEP_FILES  := $(OBJ_FILES:.o=.d)

# Sanitizer builds compile the allocator sources straight into each test
# binary under prefixed names (-DUSE_PREFIX), so the sanitizer runtime's own
# malloc/free never shadows ours. Warnings stay on, -Werror is off.
RENAME     := -Dmalloc=my_malloc -Dcalloc=my_calloc \
              -Drealloc=my_realloc -Dfree=my_free
SAN_COMMON := -std=c11 -Wall -Wextra -O1 -g -pthread -fno-omit-frame-pointer \
              -fno-builtin-malloc $(RENAME) -D_DEFAULT_SOURCE -Iinclude
TSAN_FLAGS := $(SAN_COMMON) -fsanitize=thread
ASAN_FLAGS := $(SAN_COMMON) -fsanitize=address,undefined

# Extra flags for sanitizer builds, e.g.  make tsan SAN_EXTRA=-DRANDOM_ITERATIONS=50000
SAN_EXTRA  :=

# Tests to run under sanitizers (all of tests/*.c by default).
SAN_TESTS  := $(patsubst $(TEST_DIR)/%.c,%,$(TEST_SRC))
TSAN_BINS  := $(addprefix $(BUILD_DIR)/tsan/,$(SAN_TESTS))
ASAN_BINS  := $(addprefix $(BUILD_DIR)/asan/,$(SAN_TESTS))

.PHONY: all clean test benchmark tsan asan sanitize

all: $(LIB)

# ---- library ---------------------------------------------------------------

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -fPIC -c $< -o $@

$(LIB): $(OBJ_FILES)
	$(CC) -shared $(LDFLAGS) -o $@ $^

# ---- tests and benchmarks (linked against the shared library) --------------

$(BUILD_DIR)/tests/%: $(TEST_DIR)/%.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -L$(BUILD_DIR) -lallocator $(LDFLAGS) \
		-Wl,-rpath,'$$ORIGIN/..' -o $@

$(BUILD_DIR)/benchmarks/%: $(BENCH_DIR)/%.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -L$(BUILD_DIR) -lallocator $(LDFLAGS) \
		-Wl,-rpath,'$$ORIGIN/..' -o $@

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "==> $$t"; ./$$t; done

benchmark: $(BENCH_BINS)
	@set -e; for b in $(BENCH_BINS); do echo "==> $$b"; ./$$b; done

# ---- sanitizers ------------------------------------------------------------

$(BUILD_DIR)/tsan/%: $(TEST_DIR)/%.c $(SRC_FILES) $(wildcard include/*.h)
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) $(SAN_EXTRA) $(SRC_FILES) $< -o $@

$(BUILD_DIR)/asan/%: $(TEST_DIR)/%.c $(SRC_FILES) $(wildcard include/*.h)
	@mkdir -p $(@D)
	$(CC) $(ASAN_FLAGS) $(SAN_EXTRA) $(SRC_FILES) $< -o $@

tsan: $(TSAN_BINS)
	@set -e; for t in $(TSAN_BINS); do \
		echo "==> $$t"; \
		TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" \
		setarch $$(uname -m) -R ./$$t; \
	done

asan: $(ASAN_BINS)
	@set -e; for t in $(ASAN_BINS); do \
		echo "==> $$t"; \
		ASAN_OPTIONS="detect_leaks=0:abort_on_error=1" \
		UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" ./$$t; \
	done

sanitize: tsan asan

# ---- housekeeping ----------------------------------------------------------

clean:
	rm -rf $(BUILD_DIR)

-include $(DEP_FILES)