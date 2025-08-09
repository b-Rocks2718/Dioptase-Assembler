# Directories
BUILD_DIR := build/objfiles
BIN_DIR   := build

# Tools and flags
CC        := gcc
CFLAGS    := -Wall -g

# Sources and objects
SRCS      := $(wildcard *.c)
OBJFILES  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
EXEC      := $(BIN_DIR)/assembler

TEST_OKS := $(wildcard tests/valid/*.ok)
VALID_TESTS    := $(patsubst tests/valid/%.ok,%, $(TEST_OKS))

INVALID_SRCS := $(wildcard tests/invalid/*.s)
INVALID_TESTS:= $(patsubst tests/invalid/%.s,%, $(INVALID_SRCS))

TOTAL := $(words $(VALID_TESTS)) $(words $(INVALID_TESTS))

.PRECIOUS: tests/valid/%.hex

# Link
all: $(EXEC)

$(EXEC): $(OBJFILES) | dirs
	$(CC) $(CFLAGS) -o $@ $(OBJFILES)

# Compile
$(BUILD_DIR)/%.o: %.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

# for each test/NAME.s, produce test/NAME.hex
tests/valid/%.hex: tests/valid/%.s $(EXEC) | dirs
	@echo "Assembling tests/vaid/$*.s -> tests/valid/$*.hex"
	@$(EXEC) $< -o $@

tests/valid/lib/main.hex: tests/valid/lib/main.s tests/valid/lib/lib.s $(EXEC) | dirs
	@echo "Assembling tests/valid/lib/main$*.s -> tests/valid/lib/main.hex"
	@$(EXEC) tests/valid/lib/lib.s tests/valid/lib/main.s -o tests/valid/lib/main.hex

# test linking multiple files
valid_lib.test: tests/valid/lib/main.hex tests/valid/lib/main.ok
	@echo "Running test"
	@{ \
	  if cmp --silent tests/valid/lib/main.hex tests/valid/lib/main.ok; then \
	    echo "	PASS"; \
	  else \
	    echo "	FAIL"; \
	  fi \
	}

# NAME.test will depend on having both .hex and .ok
%.test: tests/valid/%.hex tests/valid/%.ok tests/valid/lib/main.hex tests/valid/lib/main.ok
	@echo "Running test: $*"
	@{ \
	  if cmp --silent tests/valid/$*.hex tests/valid/$*.ok; then \
	    echo "	PASS: $*"; \
	  else \
	    echo "	FAIL: $*"; \
	  fi \
	}

# `make test` will expand to NAME.test for every NAME in VALID_TESTS
test: dirs $(EXEC)
	@GREEN="\033[0;32m"; \
	RED="\033[0;31m"; \
	YELLOW="\033[0;33m"; \
	NC="\033[0m"; \
	passed=0; total=$$(( $(words $(VALID_TESTS)) + $(words $(INVALID_TESTS)) + 2)); \
	echo "Running $(words $(VALID_TESTS)) valid tests:"; \
	for t in $(VALID_TESTS); do \
	  printf "%s %-20s " '-' "$$t"; \
	  if timeout 1s $(EXEC) tests/valid/$$t.s -o tests/valid/$$t.hex >/dev/null 2>&1; then \
	    if cmp --silent tests/valid/$$t.hex tests/valid/$$t.ok; then \
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
	printf "%s %-20s " '-' "lib"; \
	if timeout 1s $(EXEC) tests/valid/lib/main.s tests/valid/lib/lib.s -o tests/valid/lib/main.hex >/dev/null 2>&1; then \
	  if cmp --silent tests/valid/lib/main.hex tests/valid/lib/main.ok; then \
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
	fi;\
	echo "\nRunning $(words $(INVALID_TESTS)) invalid tests:"; \
	for t in $(INVALID_TESTS); do \
	  printf "%s %-20s " '-' "$$t"; \
	  if timeout 1s $(EXEC) tests/invalid/$$t.s -o tests/invalid/$$t.hex >/dev/null 2>&1; then \
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
	if timeout 1s $(EXEC) tests/invalid/lib/main.s tests/invalid/lib/lib.s -o tests/invalid/lib/main.hex >/dev/null 2>&1; then \
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
	echo; \
	echo "Summary: $$passed / $$total tests passed.";

# Ensure build directory exists
dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

# Remove everything but the executable
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f tests/valid/*.hex
	rm -f tests/invalid/*.hex
	rm -f tests/valid/lib/*.hex
	rm -f tests/invalid/lib/*.hex
	rm -f a.hex

# Remove everything
.PHONY: purge
purge: clean
	rm -rf $(BUILD_DIR) $(BIN_DIR)

