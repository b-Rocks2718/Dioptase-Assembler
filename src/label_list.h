#ifndef LABEL_LIST_H
#define LABEL_LIST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Store a label's owned name, address, and data/text classification.
// name_len is the byte length of name excluding its NUL terminator; it is kept
// so hashing and comparison never rescan the owned copy with strlen.
struct LabelEntry {
  char* name;
  size_t name_len;
  bool is_data;
  uint32_t addr;
};

// Own the growable array of emitted label entries.
// index is an open-addressed set of entry indices so duplicate checks stay linear.
// Empty slots are SIZE_MAX. index_cap is a power of two.
struct LabelList {
  struct LabelEntry* entries;
  size_t size;
  size_t capacity;
  size_t* index;
  size_t index_cap;
};

struct LabelList* create_label_list(size_t capacity);

void label_list_append(struct LabelList* list, const char* name, size_t len, uint32_t addr, bool is_data);

void destroy_label_list(struct LabelList* list);

void fprint_label_list(FILE* ptr, const struct LabelList* list);
void fprint_label_list_kernel(FILE* ptr, const struct LabelList* list);

#endif  // LABEL_LIST_H
