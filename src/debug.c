#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Debug metadata is written after the preprocessed source buffers are freed, so
// it cannot retain borrowed Slice views into those transient buffers.
static struct Slice* duplicate_slice(const struct Slice* slice){
  struct Slice* copy = malloc(sizeof(struct Slice));
  char* text = malloc(slice->len);
  memcpy(text, slice->start, slice->len);
  copy->start = text;
  copy->len = slice->len;
  return copy;
}

// Free a heap-backed slice and its descriptor.
static void destroy_owned_slice(struct Slice* slice){
  if (slice == NULL) return;
  free((void*)slice->start);
  free(slice);
}

// Allocate an empty list for source lines and local-variable metadata.
struct DebugInfoList* create_debug_info_list(void){
  struct DebugInfoList* list = malloc(sizeof(struct DebugInfoList));
  list->head = NULL;
  list->tail = NULL;
  list->files = NULL;
  list->nfiles = 0;
  list->file_cap = 0;
  return list;
}

// Return the interned filename slice, creating it on first use.
static struct Slice* intern_filename(struct DebugInfoList* debug_list, const struct Slice* file_name){
  for (size_t i = 0; i < debug_list->nfiles; ++i) {
    if (compare_slice_to_slice(debug_list->files[i], file_name)) return debug_list->files[i];
  }
  if (debug_list->nfiles == debug_list->file_cap) {
    size_t next_cap = debug_list->file_cap == 0 ? 4 : debug_list->file_cap * 2;
    struct Slice** grown = realloc(debug_list->files, next_cap * sizeof(struct Slice*));
    if (grown == NULL) {
      fprintf(stderr, "Assembler debug info: failed to grow filename table to %zu entries\n", next_cap);
      return NULL;
    }
    debug_list->files = grown;
    debug_list->file_cap = next_cap;
  }
  struct Slice* owned = duplicate_slice(file_name);
  debug_list->files[debug_list->nfiles++] = owned;
  return owned;
}

// Append an owned local-variable record to the debug stream.
void add_debug_local(struct DebugInfoList* debug_list, struct Slice* name, int offset, size_t size, uint32_t addr){
  // create new DebugLocal
  struct DebugLocal* local = malloc(sizeof(struct DebugLocal));
  local->name = duplicate_slice(name);
  local->offset = offset;
  local->size = size;
  local->addr = addr;
  // create new DebugEntry
  struct DebugEntry* entry = malloc(sizeof(struct DebugEntry));
  entry->type = DEBUG_INFO_LOCALS;
  entry->info.locals = local;
  entry->next = NULL;
  // append to debug_list
  if (debug_list->head == NULL){
    debug_list->head = entry;
    debug_list->tail = entry;
  } else {
    debug_list->tail->next = entry;
    debug_list->tail = entry;
  }
}

// Append an owned source-line record to the debug stream.
void add_debug_line(struct DebugInfoList* debug_list, struct Slice* file_name, int line_number, uint32_t addr){
  // create new DebugLine
  struct Slice* file = intern_filename(debug_list, file_name);
  if (file == NULL) return;
  struct DebugLine* line = malloc(sizeof(struct DebugLine));
  line->file_name = file;
  line->line_number = line_number;
  line->addr = addr;
  // create new DebugEntry
  struct DebugEntry* entry = malloc(sizeof(struct DebugEntry));
  entry->type = DEBUG_INFO_LINES;
  entry->info.lines = line;
  entry->next = NULL;
  // append to debug_list
  if (debug_list->head == NULL){
    debug_list->head = entry;
    debug_list->tail = entry;
  } else {
    debug_list->tail->next = entry;
    debug_list->tail = entry;
  }
}

// Emit local-variable and source-line records in assembler debug format.
void fprint_debug_info_list(FILE* fptr, struct DebugInfoList* debug_list){
  struct DebugEntry* current = debug_list->head;
  while (current != NULL){
    if (current->type == DEBUG_INFO_LOCALS){
      struct DebugLocal* local = current->info.locals;
      fprintf(fptr, "#local %.*s %d %zu %08X\n",
              (int)local->name->len, local->name->start, local->offset, local->size, local->addr);
    } else if (current->type == DEBUG_INFO_LINES){
      struct DebugLine* line = current->info.lines;
      fprintf(fptr, "#line %.*s %d %08X\n",
              (int)line->file_name->len, line->file_name->start, line->line_number, line->addr);
    }
    current = current->next;
  }
}

// Free all source-location entries and filenames in a debug-information list.
void destroy_debug_info_list(struct DebugInfoList* debug_list){
  if (debug_list == NULL) return;
  struct DebugEntry* current = debug_list->head;
  while (current != NULL){
    struct DebugEntry* next = current->next;
    // free the contained info based on type
    if (current->type == DEBUG_INFO_LOCALS){
      struct DebugLocal* local = current->info.locals;
      destroy_owned_slice(local->name);
      free(local);
    } else if (current->type == DEBUG_INFO_LINES){
      struct DebugLine* line = current->info.lines;
      free(line);
    }
    free(current);
    current = next;
  }
  for (size_t i = 0; i < debug_list->nfiles; ++i) {
    destroy_owned_slice(debug_list->files[i]);
  }
  free(debug_list->files);
  free(debug_list);
}
