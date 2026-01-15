#ifndef LABEL_LIST_H
#define LABEL_LIST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct LabelEntry {
  char* name;
  bool is_data;
  uint32_t addr;
};

struct LabelList {
  struct LabelEntry* entries;
  size_t size;
  size_t capacity;
};

struct LabelList* create_label_list(size_t capacity);

void label_list_append(struct LabelList* list, const char* name, size_t len, uint32_t addr, bool is_data);

void destroy_label_list(struct LabelList* list);

void fprint_label_list(FILE* ptr, const struct LabelList* list);

#endif  // LABEL_LIST_H
