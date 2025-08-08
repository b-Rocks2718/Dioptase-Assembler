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

# NAME.test will depend on having both .hex and .ok
%.test: tests/valid/%.hex tests/valid/%.ok
	@echo "Running test: $*"
	@$(EXEC) tests/valid/$*.s -o tests/valid/$*.hex
	@{ \
	  if cmp --silent tests/valid/$*.hex tests/valid/$*.ok; then \
	    echo "	PASS: $*"; \
	  else \
	    echo "	FAIL: $*"; \
	  fi \
	}

# `make test` will expand to NAME.test for every NAME in VALID_TESTS
test: dirs $(EXEC)
	@echo "Running $(words $(VALID_TESTS)) valid tests and $(words $(INVALID_TESTS)) invalid tests…"
	@echo "\nValid tests:"
	@passed=0; total=$$(( $(words $(VALID_TESTS)) + $(words $(INVALID_TESTS)) )); \
	for t in $(VALID_TESTS); do \
	  printf "  %-20s " "$$t"; \
	  $(EXEC) tests/valid/$$t.s -o tests/valid/$$t.hex >/dev/null 2>&1; \
	  if cmp --silent tests/valid/$$t.hex tests/valid/$$t.ok; then \
	    echo PASS; passed=$$((passed+1)); \
	  else \
	    echo FAIL; \
	  fi; \
	done; \
	echo "\nInvalid tests:"; \
	for t in $(INVALID_TESTS); do \
	  printf "  %-20s " "$$t"; \
	  if $(EXEC) tests/invalid/$$t.s -o tests/invalid/$$t.hex >/dev/null 2>&1; then \
	    echo FAIL; \
	  else \
	    if [ ! -f tests/invalid/$$t.hex ]; then \
	      echo PASS; passed=$$((passed+1)); \
	    else \
	      echo FAIL; \
	    fi; \
	  fi; \
	done; \
	echo; \
	echo "Summary: $$passed / $$total tests passed." ;

# Ensure build directory exists
dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

# Remove everything but the executable
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm tests/valid/*.hex
	rm tests/invalid/*.hex

# Remove everything
.PHONY: purge
purge:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	rm tests/valid/*.hex
	rm tests/invalid/*.hex
