#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "label_list.h"

#ifdef __clang_analyzer__
#include <assert.h>
#endif

// Allocate an empty label list whose entries own their copied names.
struct LabelList* create_label_list(size_t capacity){
  struct LabelList* list = malloc(sizeof(struct LabelList));
  if (capacity == 0) capacity = 16;
  list->entries = malloc(sizeof(struct LabelEntry) * capacity);
  list->size = 0;
  list->capacity = capacity;
  list->index_cap = 16;
  while (list->index_cap < capacity * 2) list->index_cap *= 2;
  list->index = malloc(sizeof(size_t) * list->index_cap);
  if (list->index != NULL) {
    for (size_t i = 0; i < list->index_cap; ++i) list->index[i] = SIZE_MAX;
  }
  return list;
}

static bool label_entry_matches(const struct LabelEntry* entry, const char* name, size_t len,
                                uint32_t addr, bool is_data);

// Hash a label identity. Name, address, and section class all participate so
// two locals that share a spelling at different addresses stay distinct.
static size_t label_key_hash(const char* name, size_t len, uint32_t addr, bool is_data) {
  size_t hash = 5381;
  for (size_t i = 0; i < len; ++i) hash = hash * 33 + (unsigned char)name[i];
  hash = hash * 33 + (size_t)addr;
  hash = hash * 33 + (is_data ? 1u : 0u);
  return hash;
}

static bool label_index_grow(struct LabelList* list) {
  size_t new_cap = list->index_cap == 0 ? 16 : list->index_cap * 2;
  size_t* next = malloc(sizeof(size_t) * new_cap);
  if (next == NULL) {
    fprintf(stderr, "Assembler label list: failed to grow duplicate index to %zu slots\n", new_cap);
    return false;
  }
  for (size_t i = 0; i < new_cap; ++i) next[i] = SIZE_MAX;
  size_t mask = new_cap - 1;
  for (size_t entry_index = 0; entry_index < list->size; ++entry_index) {
    const struct LabelEntry* entry = &list->entries[entry_index];
    size_t slot = label_key_hash(entry->name, entry->name_len, entry->addr, entry->is_data) & mask;
    while (next[slot] != SIZE_MAX) slot = (slot + 1) & mask;
    next[slot] = entry_index;
  }
  free(list->index);
  list->index = next;
  list->index_cap = new_cap;
  return true;
}

static bool label_already_present(const struct LabelList* list, const char* name, size_t len,
                                  uint32_t addr, bool is_data) {
  if (list->index == NULL || list->index_cap == 0) {
    for (size_t i = 0; i < list->size; ++i) {
      if (label_entry_matches(&list->entries[i], name, len, addr, is_data)) return true;
    }
    return false;
  }
  size_t mask = list->index_cap - 1;
  size_t slot = label_key_hash(name, len, addr, is_data) & mask;
  while (list->index[slot] != SIZE_MAX) {
    if (label_entry_matches(&list->entries[list->index[slot]], name, len, addr, is_data)) return true;
    slot = (slot + 1) & mask;
  }
  return false;
}

static void label_index_insert(struct LabelList* list, size_t entry_index) {
  if (list->index == NULL) return;
  const struct LabelEntry* entry = &list->entries[entry_index];
  size_t mask = list->index_cap - 1;
  size_t slot = label_key_hash(entry->name, entry->name_len, entry->addr, entry->is_data) & mask;
  while (list->index[slot] != SIZE_MAX) slot = (slot + 1) & mask;
  list->index[slot] = entry_index;
}

// Compare a label entry with a source name and length.
static bool label_entry_matches(const struct LabelEntry* entry, const char* name, size_t len, uint32_t addr, bool is_data){
  if (entry->addr != addr) return false;
  if (entry->is_data != is_data) return false;
  if (entry->name_len != len) return false;
  return memcmp(entry->name, name, len) == 0;
}

// Append an owned label unless an identical address/name/class record already exists.
void label_list_append(struct LabelList* list, const char* name, size_t len, uint32_t addr, bool is_data){
#ifdef __clang_analyzer__
  // Every list comes from create_label_list, which normalizes zero capacity.
  // Model that constructor invariant when append is analyzed in isolation.
  assert(list->capacity > 0);
#endif
  if (label_already_present(list, name, len, addr, is_data)) return;

  if (list->size == list->capacity){
    list->capacity *= 2;
    list->entries = realloc(list->entries, sizeof(struct LabelEntry) * list->capacity);
  }
  // Keep the open-addressed index below half full.
  if (list->index != NULL && (list->size + 1) * 2 > list->index_cap) {
    if (!label_index_grow(list)) return;
  }

  char* name_copy = malloc(len + 1);
  memcpy(name_copy, name, len);
  name_copy[len] = '\0';

  list->entries[list->size].name = name_copy;
  list->entries[list->size].name_len = len;
  list->entries[list->size].addr = addr;
  list->entries[list->size].is_data = is_data;
  label_index_insert(list, list->size);
  list->size++;
}

// Free every owned label name and all list storage.
void destroy_label_list(struct LabelList* list){
  if (list == NULL) return;
  for (size_t i = 0; i < list->size; ++i){
    free(list->entries[i].name);
  }
  free(list->entries);
  free(list->index);
  free(list);
}

// Emit data/text label metadata in the assembler debug-file format.
void fprint_label_list(FILE* ptr, const struct LabelList* list){
  if (list == NULL) return;
  for (size_t i = 0; i < list->size; ++i){
    fprintf(ptr, "#%s %s %08X\n", list->entries[i].is_data ? "data" : "label", list->entries[i].name, list->entries[i].addr);
  }
}

// Emit label metadata for kernel outputs (no data/text distinction).
// Writes "#label <name> <addr>" lines, ignoring is_data.
void fprint_label_list_kernel(FILE* ptr, const struct LabelList* list){
  if (list == NULL) return;
  for (size_t i = 0; i < list->size; ++i){
    fprintf(ptr, "#label %s %08X\n", list->entries[i].name, list->entries[i].addr);
  }
}
