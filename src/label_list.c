#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "label_list.h"

struct LabelList* create_label_list(size_t capacity){
  struct LabelList* list = malloc(sizeof(struct LabelList));
  if (capacity == 0) capacity = 16;
  list->entries = malloc(sizeof(struct LabelEntry) * capacity);
  list->size = 0;
  list->capacity = capacity;
  return list;
}

static bool label_entry_matches(const struct LabelEntry* entry, const char* name, size_t len, uint32_t addr, bool is_data){
  if (entry->addr != addr) return false;
  if (entry->is_data != is_data) return false;
  if (strlen(entry->name) != len) return false;
  return strncmp(entry->name, name, len) == 0;
}

void label_list_append(struct LabelList* list, const char* name, size_t len, uint32_t addr, bool is_data){
  for (size_t i = 0; i < list->size; ++i){
    if (label_entry_matches(&list->entries[i], name, len, addr, is_data)) return;
  }

  if (list->size == list->capacity){
    list->capacity *= 2;
    list->entries = realloc(list->entries, sizeof(struct LabelEntry) * list->capacity);
  }

  char* name_copy = malloc(len + 1);
  memcpy(name_copy, name, len);
  name_copy[len] = '\0';

  list->entries[list->size].name = name_copy;
  list->entries[list->size].addr = addr;
  list->entries[list->size].is_data = is_data;
  list->size++;
}

void destroy_label_list(struct LabelList* list){
  if (list == NULL) return;
  for (size_t i = 0; i < list->size; ++i){
    free(list->entries[i].name);
  }
  free(list->entries);
  free(list);
}

void fprint_label_list(FILE* ptr, const struct LabelList* list){
  if (list == NULL) return;
  for (size_t i = 0; i < list->size; ++i){
    fprintf(ptr, "#%s %s %08X\n", list->entries[i].is_data ? "data" : "label", list->entries[i].name, list->entries[i].addr);
  }
}
