#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>

#include "slice.h"

// Compare a length-delimited slice with an entire NUL-terminated string.
bool compare_slice_to_pointer(const struct Slice* s, char const *p) {
  for (size_t i = 0; i < s->len; i++) {
    if (p[i] != s->start[i])
      return false;
  }
  return p[s->len] == 0;
}

// Compare two slices for equal length and byte contents.
bool compare_slice_to_slice(const struct Slice* self, const struct Slice* other) {
  if (self->len != other->len)
    return false;
  for (size_t i = 0; i < self->len; i++) {
    if (other->start[i] != self->start[i])
      return false;
  }
  return true;
}

// Return whether the slice contains a syntactically valid identifier.
bool is_identifier(const struct Slice* slice) {
  if (slice->len == 0)
    return false;
  if (!isalpha(slice->start[0]))
    return false;
  for (size_t i = 1; i < slice->len; i++)
    if (!isalnum(slice->start[i]))
      return false;
  return true;
}

// Write the slice bytes to stdout without requiring NUL termination.
void print_slice(struct Slice* slice) {
  for (size_t i = 0; i < slice->len; i++) {
    printf("%c", slice->start[i]);
  }
}

// Write the slice bytes to stderr without requiring NUL termination.
void print_slice_err(struct Slice* slice) {
  for (size_t i = 0; i < slice->len; i++) {
    fprintf(stderr, "%c", slice->start[i]);
  }
}

// Compute the stable hash used for slice-keyed maps.
size_t hash_slice(const struct Slice* key) {
  // djb2
  size_t out = 5381;
  for (size_t i = 0; i < key->len; i++) {
    char const c = key->start[i];
    out = out * 33 + c;
  }
  return out;
}
