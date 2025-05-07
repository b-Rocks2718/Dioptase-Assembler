#include <stdlib.h>
#include <stdio.h>

#include "token_array.h"

struct TokenArray* create_token_array(size_t capacity){
  struct Token* tokens = malloc(sizeof(struct Token) * capacity);

  struct TokenArray* arr = malloc(sizeof(struct TokenArray));

  arr->capacity = capacity;
  arr->size = 0;
  arr->tokens = tokens;

  return arr;
}

void token_array_append(struct TokenArray* arr, struct Token* value){
  if (arr->size == arr->capacity){
    arr->tokens = realloc(arr->tokens, arr->capacity * sizeof(struct Token) * 2);
    arr->capacity = arr->capacity * 2;
  } 
    
  arr->tokens[arr->size] = *value;
  arr->size++;

  free(value);
}

struct Token token_array_get(struct TokenArray* arr, size_t i){
  // no checks on i, might regret this later
  return arr->tokens[i];
}

void destroy_token_array(struct TokenArray* arr){
  for (int i = 0; i < arr->size; ++i){
    if (arr->tokens[i].type == IDENT) free(arr->tokens[i].data.ident_name);
  }
  free(arr->tokens);
  free(arr);
}

void print_token_array(struct TokenArray* arr){
  printf("[");
  for (int i = 0; i < arr->size; ++i){
    if (i % 10 == 0) printf("\n    ");
    print_token(arr->tokens[i]);
    if (i == arr->size - 1) printf("\n]\n");
    else printf(", ");
  }
}