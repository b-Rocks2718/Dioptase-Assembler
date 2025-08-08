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
TEST_OKS := $(wildcard tests/*.ok)
TESTS    := $(patsubst tests/%.ok,%, $(TEST_OKS))
TOTAL := $(words $(TESTS))

.PRECIOUS: tests/%.hex

# Link
all: $(EXEC)

$(EXEC): $(OBJFILES) | dirs
	$(CC) $(CFLAGS) -o $@ $(OBJFILES)

# Compile
$(BUILD_DIR)/%.o: %.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

# for each test/NAME.s, produce test/NAME.hex
tests/%.hex: tests/%.s $(EXEC) | dirs
	@echo "Assembling tests/$*.s -> tests/$*.hex"
	@$(EXEC) $< -o $@

# NAME.test will depend on having both .hex and .ok
%.test: tests/%.hex tests/%.ok
	@echo "Running test: $*"
	@$(EXEC) tests/$*.s -o tests/$*.hex
	@{ \
	  if cmp --silent tests/$*.hex tests/$*.ok; then \
	    echo "	PASS: $*"; \
	  else \
	    echo "	FAIL: $*"; \
	  fi \
	}

# `make test` will expand to NAME.test for every NAME in TESTS
test: dirs $(EXEC)
	@echo "Running $(TOTAL) tests…"
	@passed=0; \
	for t in $(TESTS); do \
	  printf "%-20s " "$$t"; \
	  $(EXEC) tests/$$t.s -o tests/$$t.hex >/dev/null; \
	  if cmp --silent tests/$$t.hex tests/$$t.ok; then \
	    echo PASS; \
	    passed=$$((passed+1)); \
	  else \
	    echo FAIL; \
	  fi; \
	done; \
	echo; \
	echo "Summary: $$passed / $(TOTAL) tests passed." ;

# Ensure build directory exists
dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

# Remove everything but the executable
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm tests/*.hex

# Remove everything
.PHONY: purge
purge:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	rm tests/*.hex
