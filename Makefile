# Directories
SRC_DIR 	:= src
BUILD_ROOT := build
DEBUG_DIR := $(BUILD_ROOT)/debug
RELEASE_DIR := $(BUILD_ROOT)/release
DEBUG_OBJ_DIR := $(DEBUG_DIR)/objfiles
RELEASE_OBJ_DIR := $(RELEASE_DIR)/objfiles

# Tools and flags
CC        := gcc
CFLAGS_COMMON ?= -Wall
OPT_DEBUG := -O0
OPT_RELEASE := -O3
DEBUG_INFO := -g
CFLAGS_DEBUG ?= $(CFLAGS_COMMON) $(OPT_DEBUG) $(DEBUG_INFO)
CFLAGS_RELEASE ?= $(CFLAGS_COMMON) $(OPT_RELEASE)
LDFLAGS_DEBUG ?=
LDFLAGS_RELEASE ?=

# Sources and objects
SRCS      := $(wildcard $(SRC_DIR)/*.c)
DEBUG_OBJFILES  := $(patsubst %.c,$(DEBUG_OBJ_DIR)/%.o, $(notdir $(SRCS)))
RELEASE_OBJFILES  := $(patsubst %.c,$(RELEASE_OBJ_DIR)/%.o, $(notdir $(SRCS)))
EXECUTABLE := basm
DEBUG_EXEC := $(DEBUG_DIR)/$(EXECUTABLE)
RELEASE_EXEC := $(RELEASE_DIR)/$(EXECUTABLE)

# Tests
USER_OKS := $(wildcard tests/valid/user/*.ok)
KERNEL_OKS := $(wildcard tests/valid/kernel/*.ok)
VALID_USER_TESTS := $(patsubst tests/valid/user/%.ok,%, $(USER_OKS))
VALID_KERNEL_TESTS := $(patsubst tests/valid/kernel/%.ok,%, $(KERNEL_OKS))
USER_LIB_OKS := $(wildcard tests/valid/user/lib/*.ok)
KERNEL_LIB_OKS := $(wildcard tests/valid/kernel/lib/*.ok)
VALID_USER_LIB_TESTS := $(patsubst tests/valid/user/lib/%.ok,%, $(USER_LIB_OKS))
VALID_KERNEL_LIB_TESTS := $(patsubst tests/valid/kernel/lib/%.ok,%, $(KERNEL_LIB_OKS))

INVALID_SRCS := $(wildcard tests/invalid/*.s)
INVALID_TESTS := $(patsubst tests/invalid/%.s,%, $(INVALID_SRCS))

DEBUG_SRCS := $(wildcard tests/debug/*.s)
DEBUG_TESTS := $(patsubst tests/debug/%.s,%, $(DEBUG_SRCS))

BIN_USER_OKS := $(wildcard tests/bin/user/*.ok)
BIN_USER_TESTS := $(patsubst tests/bin/user/%.ok,%, $(BIN_USER_OKS))

.PRECIOUS: tests/valid/user/%.hex tests/valid/kernel/%.hex tests/valid/user/lib/%.hex tests/valid/kernel/lib/%.hex

# Link
all: debug

debug: $(DEBUG_EXEC)

release: $(RELEASE_EXEC)

$(DEBUG_EXEC): $(DEBUG_OBJFILES) | dirs-debug
	$(CC) $(CFLAGS_DEBUG) $(LDFLAGS_DEBUG) -o $@ $(DEBUG_OBJFILES)

$(RELEASE_EXEC): $(RELEASE_OBJFILES) | dirs-release
	$(CC) $(CFLAGS_RELEASE) $(LDFLAGS_RELEASE) -o $@ $(RELEASE_OBJFILES)

# Compile
$(DEBUG_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | dirs-debug
	$(CC) $(CFLAGS_DEBUG) -c $< -o $@

$(RELEASE_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | dirs-release
	$(CC) $(CFLAGS_RELEASE) -c $< -o $@

# for each test/NAME.s, produce test/NAME.hex
tests/valid/user/%.hex: tests/valid/user/%.s $(DEBUG_EXEC) | dirs-debug
	@echo "Assembling tests/valid/user/$*.s -> tests/valid/user/$*.hex"
	@$(DEBUG_EXEC) $< -o $@

tests/valid/kernel/%.hex: tests/valid/kernel/%.s $(DEBUG_EXEC) | dirs-debug
	@echo "Assembling tests/valid/kernel/$*.s -> tests/valid/kernel/$*.hex"
	@flags="-kernel"; \
	if [ "$*" = "macro" ]; then \
	  flags="$$flags -DBIG=0xAAAA5555 -DONE=1"; \
	fi; \
	$(DEBUG_EXEC) $$flags $< -o $@

tests/valid/user/lib/%.hex: tests/valid/user/lib/%.s tests/valid/user/lib/lib.s $(DEBUG_EXEC) | dirs-debug
	@echo "Assembling tests/valid/user/lib/$*.s -> tests/valid/user/lib/$*.hex"
	@$(DEBUG_EXEC) tests/valid/user/lib/lib.s tests/valid/user/lib/$*.s -o $@

tests/valid/kernel/lib/%.hex: tests/valid/kernel/lib/%.s tests/valid/kernel/lib/lib.s $(DEBUG_EXEC) | dirs-debug
	@echo "Assembling tests/valid/kernel/lib/$*.s -> tests/valid/kernel/lib/$*.hex"
	@$(DEBUG_EXEC) -kernel tests/valid/kernel/lib/lib.s tests/valid/kernel/lib/$*.s -o $@

# for each bin test, produce tests/bin/user/NAME.bin
tests/bin/user/%.bin: tests/valid/user/%.s $(DEBUG_EXEC) | dirs-debug dirs-bin
	@echo "Assembling tests/valid/user/$*.s -> tests/bin/user/$*.bin"
	@$(DEBUG_EXEC) -bin $< -o $@

# Run the test suite.
define RUN_TESTS
	@GREEN="\033[0;32m"; \
	RED="\033[0;31m"; \
	YELLOW="\033[0;33m"; \
	NC="\033[0m"; \
	passed=0; total=$$(( $(words $(VALID_USER_TESTS)) + $(words $(VALID_KERNEL_TESTS)) + $(words $(VALID_USER_LIB_TESTS)) + $(words $(VALID_KERNEL_LIB_TESTS)) + $(words $(BIN_USER_TESTS)) + $(words $(INVALID_TESTS)) + $(words $(DEBUG_TESTS)) + 1)); \
	echo "Running $(words $(VALID_USER_TESTS)) user tests:"; \
	for t in $(VALID_USER_TESTS); do \
	  printf "%s %-20s " '-' "$$t"; \
	  if timeout 1s $(TEST_EXEC) tests/valid/user/$$t.s -o tests/valid/user/$$t.hex >/dev/null 2>&1; then \
	    if cmp --silent tests/valid/user/$$t.hex tests/valid/user/$$t.ok; then \
	      echo "$$GREEN PASS $$NC"; passed=$$((passed+1)); \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  else \
	    if [ $$? -eq 124 ]; then \
	      echo "$$YELLOW TIMEOUT $$NC"; \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  fi; \
	done; \
	echo "\nRunning $(words $(BIN_USER_TESTS)) user bin tests:"; \
	for t in $(BIN_USER_TESTS); do \
	  printf "%s %-20s " '-' "$$t"; \
	  if timeout 1s $(TEST_EXEC) -bin tests/valid/user/$$t.s -o tests/bin/user/$$t.bin >/dev/null 2>&1; then \
	    if cmp --silent tests/bin/user/$$t.bin tests/bin/user/$$t.ok; then \
	      echo "$$GREEN PASS $$NC"; passed=$$((passed+1)); \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  else \
	    if [ $$? -eq 124 ]; then \
	      echo "$$YELLOW TIMEOUT $$NC"; \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  fi; \
	done; \
	echo "\nRunning $(words $(VALID_USER_LIB_TESTS)) user lib tests:"; \
	for t in $(VALID_USER_LIB_TESTS); do \
	  printf "%s %-20s " '-' "$$t"; \
	  if timeout 1s $(TEST_EXEC) tests/valid/user/lib/lib.s tests/valid/user/lib/$$t.s -o tests/valid/user/lib/$$t.hex >/dev/null 2>&1; then \
	    if cmp --silent tests/valid/user/lib/$$t.hex tests/valid/user/lib/$$t.ok; then \
	      echo "$$GREEN PASS $$NC"; passed=$$((passed+1)); \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  else \
	    if [ $$? -eq 124 ]; then \
	      echo "$$YELLOW TIMEOUT $$NC"; \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  fi; \
	done; \
	echo "\nRunning $(words $(VALID_KERNEL_TESTS)) kernel tests:"; \
	for t in $(VALID_KERNEL_TESTS); do \
	  printf "%s %-20s " '-' "$$t"; \
	  kernel_flags="-kernel"; \
	  case "$$t" in \
	    macro) kernel_flags="$$kernel_flags -DBIG=0xAAAA5555 -DONE=1";; \
	  esac; \
	  if timeout 1s $(TEST_EXEC) $$kernel_flags tests/valid/kernel/$$t.s -o tests/valid/kernel/$$t.hex >/dev/null 2>&1; then \
	    if cmp --silent tests/valid/kernel/$$t.hex tests/valid/kernel/$$t.ok; then \
	      echo "$$GREEN PASS $$NC"; passed=$$((passed+1)); \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  else \
	    if [ $$? -eq 124 ]; then \
	      echo "$$YELLOW TIMEOUT $$NC"; \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  fi; \
	done; \
	echo "\nRunning $(words $(VALID_KERNEL_LIB_TESTS)) kernel lib tests:"; \
	for t in $(VALID_KERNEL_LIB_TESTS); do \
	  printf "%s %-20s " '-' "$$t"; \
	  if timeout 1s $(TEST_EXEC) -kernel tests/valid/kernel/lib/lib.s tests/valid/kernel/lib/$$t.s -o tests/valid/kernel/lib/$$t.hex >/dev/null 2>&1; then \
	    if cmp --silent tests/valid/kernel/lib/$$t.hex tests/valid/kernel/lib/$$t.ok; then \
	      echo "$$GREEN PASS $$NC"; passed=$$((passed+1)); \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  else \
	    if [ $$? -eq 124 ]; then \
	      echo "$$YELLOW TIMEOUT $$NC"; \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  fi; \
	done; \
	echo "\nRunning $(words $(INVALID_TESTS)) invalid tests:"; \
	for t in $(INVALID_TESTS); do \
	  printf "%s %-20s " '-' "$$t"; \
	  if timeout 1s $(TEST_EXEC) tests/invalid/$$t.s -o tests/invalid/$$t.hex >/dev/null 2>&1; then \
	    echo "$$RED FAIL $$NC"; \
	  else \
	    if [ $$? -eq 124 ]; then \
	      echo "$$YELLOW TIMEOUT $$NC"; \
	    elif [ ! -f tests/invalid/$$t.hex ]; then \
	      echo "$$GREEN PASS $$NC"; passed=$$((passed+1)); \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  fi; \
	done; \
	printf "%s %-20s " '-' "bad_lib"; \
	if timeout 1s $(TEST_EXEC) tests/invalid/lib/main.s tests/invalid/lib/lib.s -o tests/invalid/lib/main.hex >/dev/null 2>&1; then \
	  echo "$$RED FAIL $$NC"; \
	else \
	  if [ $$? -eq 124 ]; then \
	    echo "$$YELLOW TIMEOUT $$NC"; \
	  elif [ ! -f tests/invalid/lib/main.hex ]; then \
	    echo "$$GREEN PASS $$NC"; passed=$$((passed+1)); \
	  else \
	    echo "$$RED FAIL $$NC"; \
	  fi; \
	fi; \
	echo "\nRunning $(words $(DEBUG_TESTS)) debug label tests:"; \
	for t in $(DEBUG_TESTS); do \
	  printf "%s %-20s " '-' "$$t"; \
	  rm -f tests/debug/$$t.debug; \
	  debug_flags="-g"; \
	  case "$$t" in \
	    *_kernel) debug_flags="-g -kernel";; \
	  esac; \
	  if timeout 1s $(TEST_EXEC) $$debug_flags tests/debug/$$t.s -o tests/debug/$$t.debug >/dev/null 2>&1; then \
	    if [ -f tests/debug/$$t.debug ]; then \
	      ok=1; \
	      if [ -f tests/debug/$$t.labels ]; then \
	        while IFS= read -r line; do \
	          if ! grep -Fq "$$line" tests/debug/$$t.debug; then \
	            ok=0; \
	            break; \
	          fi; \
	        done < tests/debug/$$t.labels; \
	      else \
	        ok=0; \
	      fi; \
	      if [ $$ok -eq 1 ]; then \
	        echo "$$GREEN PASS $$NC"; passed=$$((passed+1)); \
	      else \
	        echo "$$RED FAIL $$NC"; \
	      fi; \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  else \
	    if [ $$? -eq 124 ]; then \
	      echo "$$YELLOW TIMEOUT $$NC"; \
	    else \
	      echo "$$RED FAIL $$NC"; \
	    fi; \
	  fi; \
	done; \
	echo; \
	echo "Summary: $$passed / $$total tests passed.";
endef

test: TEST_EXEC := $(DEBUG_EXEC)
test: $(DEBUG_EXEC)
	$(RUN_TESTS)

test-release: TEST_EXEC := $(RELEASE_EXEC)
test-release: $(RELEASE_EXEC)
	$(RUN_TESTS)

.PHONY: all debug release test test-release coverage clean purge
coverage: clean
	@echo "Rebuilding with coverage flags..."
	$(MAKE) debug CFLAGS_DEBUG="$(CFLAGS_DEBUG) -fprofile-arcs -ftest-coverage" \
	        LDFLAGS_DEBUG="$(LDFLAGS_DEBUG) -fprofile-arcs -ftest-coverage"
	@echo "Running tests..."
	$(MAKE) test
	@echo "Generating coverage report..."
	@gcov -o $(DEBUG_OBJ_DIR) $(SRCS)


# Ensure build directory exists
dirs-debug:
	@mkdir -p $(DEBUG_OBJ_DIR) $(DEBUG_DIR)

dirs-release:
	@mkdir -p $(RELEASE_OBJ_DIR) $(RELEASE_DIR)

dirs-bin:
	@mkdir -p tests/bin/user

# Remove everything but the executable
clean:
	rm -rf $(DEBUG_OBJ_DIR) $(RELEASE_OBJ_DIR)
	rm -f tests/valid/user/*.hex
	rm -f tests/valid/user/lib/*.hex
	rm -f tests/valid/kernel/*.hex
	rm -f tests/valid/kernel/lib/*.hex
	rm -f tests/invalid/*.hex
	rm -f tests/invalid/lib/*.hex
	rm -f tests/debug/*.debug
	rm -f tests/debug/*.hex
	rm -f tests/bin/user/*.bin
	rm -f *.gcno *.gcda *.gcov
	rm -f coverage.info
	rm -f a.bin
	rm -f a.hex

# Remove everything
purge: clean
	rm -rf $(BUILD_ROOT)
