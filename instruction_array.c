#include <stdlib.h>
#include <stdio.h>

#include "instruction_array.h"

struct InstructionArray* create_instruction_array(size_t capacity){
  int* instructions = malloc(sizeof(int) * capacity);

  struct InstructionArray* arr = malloc(sizeof(struct InstructionArray));

  arr->capacity = capacity;
  arr->size = 0;
  arr->instructions = instructions;

  return arr;
}

void instruction_array_append(struct InstructionArray* arr, int value){
  if (arr->size == arr->capacity){
    arr->instructions = realloc(arr->instructions, arr->capacity * sizeof(int) * 2);
    arr->capacity = arr->capacity * 2;
  } 
    
  arr->instructions[arr->size] = value;
  arr->size++;
}

int instruction_array_get(struct InstructionArray* arr, size_t i){
  // no checks on i, might regret this later
  return arr->instructions[i];
}

void destroy_instruction_array(struct InstructionArray* arr){
  free(arr->instructions);
  free(arr);
}

void print_instruction_array(struct InstructionArray* arr){
  for (int i = 0; i < arr->size; ++i){
    printf("%08X\n", arr->instructions[i]);
  }
}