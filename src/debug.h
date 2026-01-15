#ifndef DEBUG_H
#define DEBUG_H

#include "slice.h"
#include <stdio.h>

struct DebugLocal {
  struct Slice* name;          // Name of the local variable
  int offset;                 // Offset from base pointer (BP)
};

struct DebugLine {
  struct Slice* file_name;      // Source file name
  int line_number;            // Line number in the source file
};

union DebugInfo {
  struct DebugLocal* locals;  // Linked list of local variables
  struct DebugLine* lines;    // Linked list of source lines
};

enum DebugInfoType {
  DEBUG_INFO_LOCALS,
  DEBUG_INFO_LINES,
};

struct DebugEntry {
  enum DebugInfoType type;    // Type of debug information
  union DebugInfo info;       // Actual debug information
  struct DebugEntry* next;    // Next debug entry in the list
};

struct DebugInfoList {
  struct DebugEntry* head;    // Head of the debug entries list
  struct DebugEntry* tail;    // Tail of the debug entries list
};

struct DebugInfoList* create_debug_info_list(void);

void add_debug_local(struct DebugInfoList* debug_list, struct Slice* name, int offset);

void add_debug_line(struct DebugInfoList* debug_list, struct Slice* file_name, int line_number);

void fprint_debug_info_list(FILE* fptr, struct DebugInfoList* debug_list);

void destroy_debug_info_list(struct DebugInfoList* debug_list);

#endif // DEBUG_H