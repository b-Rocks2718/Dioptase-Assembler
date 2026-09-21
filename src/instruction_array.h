#ifndef INSTRUCTION_ARRAY_H
#define INSTRUCTION_ARRAY_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

// Store words emitted for one origin together with dynamic capacity metadata.
struct InstructionArray {
  int origin;
  int* instructions;
  size_t size;
  size_t capacity;
  struct InstructionArray* next;
};

// Own the origin-ordered linked list of instruction arrays.
struct InstructionArrayList {
  struct InstructionArray* head;
  struct InstructionArray* tail;
};

struct InstructionArrayList* create_instruction_array_list(void);

void instruction_array_list_append(struct InstructionArrayList* list, struct InstructionArray* arr);

void destroy_instruction_array_list(struct InstructionArrayList* list);

void print_instruction_array_list(struct InstructionArrayList* list);

void fprint_instruction_array_list(FILE* ptr, struct InstructionArrayList* list, bool raw);

// Write instruction arrays as raw little-endian bytes.
//         inserts zero bytes so each array begins at its origin address.
// Origins are non-decreasing when include_origin_padding is true.
void fwrite_instruction_array_list(FILE* ptr, struct InstructionArrayList* list, bool include_origin_padding);

struct InstructionArray* create_instruction_array(size_t capacity, int origin);

// Append a full 32-bit word to the instruction array.
// arr is non-NULL and owned by the caller.
void instruction_array_append(struct InstructionArray* arr, int value);

// Append a 16-bit value at the byte address pc, using little-endian byte order.
// Calls are sequential in increasing pc.
void instruction_array_append_double(struct InstructionArray* arr, uint16_t value, int pc);

// Append an 8-bit value at the byte address pc.
// Calls are sequential in increasing pc.
void instruction_array_append_byte(struct InstructionArray* arr, uint8_t value, int pc);

int instruction_array_get(struct InstructionArray* arr, size_t i);

void destroy_instruction_array(struct InstructionArray* arr);

void print_instruction_array(struct InstructionArray* arr);

void fprint_instruction_array(FILE* ptr, struct InstructionArray* arr, bool raw);

size_t instruction_array_list_size(struct InstructionArrayList* list);

#endif  // INSTRUCTION_ARRAY_H
