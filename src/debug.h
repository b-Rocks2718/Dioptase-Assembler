#ifndef DEBUG_H
#define DEBUG_H

#include "slice.h"
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

// Describe one source-level local and its emitted address range.
struct DebugLocal {
  struct Slice* name;          // Name of the local variable
  int offset;                 // Offset from base pointer (BP)
  size_t size;              // Size of the local variable in bytes
  uint32_t addr;              // Address where this local becomes visible
};

// Associate an emitted address with its source filename and line.
struct DebugLine {
  struct Slice* file_name;      // Source file name
  int line_number;            // Line number in the source file
  uint32_t addr;              // Address of the next instruction for this line
};

// Own the local-variable and source-line tables for one assembled file.
union DebugInfo {
  struct DebugLocal* locals;  // Linked list of local variables
  struct DebugLine* lines;    // Linked list of source lines
};

// Distinguish local and source-line records in the debug table.
enum DebugInfoType {
  DEBUG_INFO_LOCALS,
  DEBUG_INFO_LINES,
};

// Link one tagged debug record to its payload.
struct DebugEntry {
  enum DebugInfoType type;    // Type of debug information
  union DebugInfo info;       // Actual debug information
  struct DebugEntry* next;    // Next debug entry in the list
};

// Own the linked debug-record list and its tail pointer.
// files holds one owned copy of each distinct .line filename. Line records
// borrow those slices so a repeated path is not copied once per source line.
struct DebugInfoList {
  struct DebugEntry* head;    // Head of the debug entries list
  struct DebugEntry* tail;    // Tail of the debug entries list
  struct Slice** files;
  size_t nfiles;
  size_t file_cap;
};

struct DebugInfoList* create_debug_info_list(void);

void add_debug_local(struct DebugInfoList* debug_list, struct Slice* name, int offset, size_t size, uint32_t addr);

void add_debug_line(struct DebugInfoList* debug_list, struct Slice* file_name, int line_number, uint32_t addr);

void fprint_debug_info_list(FILE* fptr, struct DebugInfoList* debug_list);

void destroy_debug_info_list(struct DebugInfoList* debug_list);

#endif // DEBUG_H
