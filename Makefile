# Directories
BUILD_DIR := build/objfiles
BIN_DIR   := build

# Tools and flags
CC        := gcc
CFLAGS    := -Wall -g

# Sources and objects
SRCS      := $(wildcard *.c)
OBJFILES  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
EXEC      := $(BIN_DIR)/main

# Link
$(EXEC): dirs $(OBJFILES)
	$(CC) $(CFLAGS) -o $@ $(OBJFILES)

# Compile
$(BUILD_DIR)/%.o: %.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

# Ensure build directory exists
.PHONY: dirs
dirs:
	mkdir -p $(BUILD_DIR) $(BIN_DIR)

# Clean
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
