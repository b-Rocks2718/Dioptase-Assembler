#pragma once

#include <stddef.h>

struct InstructionArray {
  int* instructions;
  size_t size;
  size_t capacity;
};

struct InstructionArray* create_instruction_array(size_t capacity);

void instruction_array_append(struct InstructionArray* arr, int value);

int instruction_array_get(struct InstructionArray* arr, size_t i);

void destroy_instruction_array(struct InstructionArray* arr);

void print_instruction_array(struct InstructionArray* arr);

void fprint_instruction_array(FILE* ptr, struct InstructionArray* arr);
