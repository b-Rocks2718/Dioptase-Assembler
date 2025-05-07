#pragma once

#include <stdbool.h>
#include <stddef.h>

struct Slice {
  char const * start; // where does the string start in memory?
  size_t len;        // How many characters in the string
};

bool compare_slice_to_pointer(const struct Slice* s, char const *p);

bool compare_slice_to_slice(const struct Slice* self, const struct Slice* other);

bool is_identifier(const struct Slice* slice);

void print_slice(struct Slice* slice);

size_t hash_slice(const struct Slice* key);