#pragma once

#include <stddef.h>

struct InstructionArray {
  int origin;
  int* instructions;
  size_t size;
  size_t capacity;
  struct InstructionArray* next;
};

struct InstructionArrayList {
  struct InstructionArray* head;
  struct InstructionArray* tail;
};

struct InstructionArrayList* create_instruction_array_list(void);

void instruction_array_list_append(struct InstructionArrayList* list, struct InstructionArray* arr);

void destroy_instruction_array_list(struct InstructionArrayList* list);

void print_instruction_array_list(struct InstructionArrayList* list);

void fprint_instruction_array_list(FILE* ptr, struct InstructionArrayList* list);


struct InstructionArray* create_instruction_array(size_t capacity, int origin);

void instruction_array_append(struct InstructionArray* arr, int value);

int instruction_array_get(struct InstructionArray* arr, size_t i);

void destroy_instruction_array(struct InstructionArray* arr);

void print_instruction_array(struct InstructionArray* arr);

void fprint_instruction_array(FILE* ptr, struct InstructionArray* arr);
