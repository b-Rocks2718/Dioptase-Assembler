#pragma once

#include <stddef.h>

#include "token.h"

struct TokenArray {
  struct Token* tokens;
  size_t size;
  size_t capacity;
};

struct TokenArray* create_token_array(size_t capacity);

void token_array_append(struct TokenArray* arr, struct Token* value);

struct Token token_array_get(struct TokenArray* arr, size_t i);

void destroy_token_array(struct TokenArray* arr);

void print_token_array(struct TokenArray* arr);