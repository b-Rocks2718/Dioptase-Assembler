#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "instruction_array.h"
#include <stdint.h>

enum {
  kWordBytes = 4,
  kByteMask = 0xFF,
  kByteStride = 1
};

/*
  Linked list for holding instruction arrays
*/

struct InstructionArrayList* create_instruction_array_list(void){
  struct InstructionArrayList* list = malloc(sizeof(struct InstructionArrayList));
  list->head = create_instruction_array(10, 0);
  list->tail = list->head;
  return list;
}

void instruction_array_list_append(struct InstructionArrayList* list, struct InstructionArray* arr){
  if (list->head == NULL){
    list->head = arr;
    list->tail = arr;
  } else {
    assert(list->tail != NULL);
    list->tail->next = arr;
    list->tail = arr;
  }
}

void destroy_instruction_array_list(struct InstructionArrayList* list){
  destroy_instruction_array(list->head);
  free(list);
}

void print_instruction_array_list(struct InstructionArrayList* list){
  print_instruction_array(list->head);
}

void fprint_instruction_array_list(FILE* ptr, struct InstructionArrayList* list, bool raw){
  fprint_instruction_array(ptr, list->head, raw);
}

/*
  Dynamic array used for holding instructions
*/

struct InstructionArray* create_instruction_array(size_t capacity, int origin){
  int* instructions = malloc(sizeof(int) * capacity);

  struct InstructionArray* arr = malloc(sizeof(struct InstructionArray));

  arr->capacity = capacity;
  arr->size = 0;
  arr->instructions = instructions;
  arr->origin = origin;
  arr->next = NULL;

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

// Purpose: Update a byte within an existing 32-bit word.
// Inputs: word points to the word to update; byte_index is 0..3; value is the byte payload.
// Outputs: None.
// Invariants/Assumptions: byte_index is less than kWordBytes.
static void set_word_byte(int* word, int byte_index, uint8_t value){
  uint32_t mask = (uint32_t)kByteMask << (8 * byte_index);
  uint32_t updated = ((uint32_t)(*word) & ~mask) | ((uint32_t)value << (8 * byte_index));
  *word = (int)updated;
}

void instruction_array_append_double(struct InstructionArray* arr, uint16_t value, int pc){
  // Little-endian: low byte goes at the lowest address.
  instruction_array_append_byte(arr, (uint8_t)(value & kByteMask), pc);
  instruction_array_append_byte(arr, (uint8_t)((value >> 8) & kByteMask), pc + kByteStride);
}

void instruction_array_append_byte(struct InstructionArray* arr, uint8_t value, int pc){
  int byte_index = pc % kWordBytes;
  if (byte_index == 0){
    instruction_array_append(arr, 0);
  }
  assert(arr->size > 0);
  set_word_byte(&arr->instructions[arr->size - 1], byte_index, value);
}

int instruction_array_get(struct InstructionArray* arr, size_t i){
  // no checks on i, might regret this later
  return arr->instructions[i];
}

void destroy_instruction_array(struct InstructionArray* arr){
  if (arr->next != NULL) destroy_instruction_array(arr->next);
  free(arr->instructions);
  free(arr);
}

void print_instruction_array(struct InstructionArray* arr){
  printf("@%d\n", arr->origin);
  for (int i = 0; i < arr->size; ++i){
    printf("%08X\n", arr->instructions[i]);
  }
  if (arr->next != NULL) print_instruction_array(arr->next);
}

void fprint_instruction_array(FILE* ptr, struct InstructionArray* arr, bool raw){
  // raw => no ELF structure => put origin markers
  if (raw) fprintf(ptr, "@%X\n", arr->origin / 4);
  for (int i = 0; i < arr->size; ++i){
    fprintf(ptr, "%08X\n", arr->instructions[i]);
  }
  if (arr->next != NULL) fprint_instruction_array(ptr, arr->next, raw);
}

size_t instruction_array_list_size(struct InstructionArrayList* list){
  size_t total_size = 0;
  struct InstructionArray* curr = list->head;
  while (curr != NULL){
    total_size += curr->size;
    curr = curr->next;
  }
  return total_size;
}
