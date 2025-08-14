#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "instruction_array.h"

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

void fprint_instruction_array_list(FILE* ptr, struct InstructionArrayList* list){
  fprint_instruction_array(ptr, list->head);
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

void fprint_instruction_array(FILE* ptr, struct InstructionArray* arr){
  fprintf(ptr, "@%d\n", arr->origin);
  for (int i = 0; i < arr->size; ++i){
    fprintf(ptr, "%08X\n", arr->instructions[i]);
  }
  if (arr->next != NULL) fprint_instruction_array(ptr, arr->next);
}