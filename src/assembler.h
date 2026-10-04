#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include <stdbool.h>
#include "debug.h"
#include "lexer.h"

extern unsigned long pc;

// does the file wish to use pivileges instructions?
extern bool is_kernel;

struct LabelList;

struct ProgramDescriptor* assemble(int num_files, int* file_names, bool is_kernel,
  const char *const *const argv, char** files, struct LabelList** labels_out,
  struct DebugInfoList** labels_c_out);

void set_cli_defines(int count, const char* const* defines);

// Identify output sections and the implicit kernel section.
// NO_SECTION marks user-mode code before any .text/.rodata/.data/.bss
// directive. It is never a valid array index; is_section_in_range rejects it,
// and every section-indexed access is guarded by ensure_valid_section.
enum UserSection {
  NO_SECTION = -1,
  TEXT_SECTION = 0,
  RODATA_SECTION = 1,
  DATA_SECTION = 2,
  BSS_SECTION = 3,
  IMPLICIT_SECTION = 4,
  END_SECTION = 5,
  SECTION_COUNT = 6,
};

// consume a literal immediate or label immediate
long consume_immediate(enum ConsumeResult* result);

// consumes a single instruction and converts it to binary or hex
int consume_instruction(enum ConsumeResult* result);

#endif  // ASSEMBLER_H
