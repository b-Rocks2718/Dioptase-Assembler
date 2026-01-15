#include "debug.h"

#include <stdlib.h>

struct DebugInfoList* create_debug_info_list(void){
  struct DebugInfoList* list = malloc(sizeof(struct DebugInfoList));
  list->head = NULL;
  list->tail = NULL;
  return list;
}

void add_debug_local(struct DebugInfoList* debug_list, struct Slice* name, int offset, size_t size, uint32_t addr){
  // create new DebugLocal
  struct DebugLocal* local = malloc(sizeof(struct DebugLocal));
  local->name = name;
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

void add_debug_line(struct DebugInfoList* debug_list, struct Slice* file_name, int line_number, uint32_t addr){
  // create new DebugLine
  struct DebugLine* line = malloc(sizeof(struct DebugLine));
  line->file_name = file_name;
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

void destroy_debug_info_list(struct DebugInfoList* debug_list){
  struct DebugEntry* current = debug_list->head;
  while (current != NULL){
    struct DebugEntry* next = current->next;
    // free the contained info based on type
    if (current->type == DEBUG_INFO_LOCALS){
      struct DebugLocal* local = current->info.locals;
      free(local);
    } else if (current->type == DEBUG_INFO_LINES){
      struct DebugLine* line = current->info.lines;
      free(line);
    }
    free(current);
    current = next;
  }
  free(debug_list);
}
