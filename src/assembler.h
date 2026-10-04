#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include <stdbool.h>
#include "debug.h"
#include "lexer.h"

// Address of the statement being assembled (runtime address in pass 2).
extern unsigned long pc;

// does the file wish to use pivileges instructions?
extern bool is_kernel;

struct LabelList;

// Assemble the preprocessed buffers `files` (named `paths` in diagnostics)
// into one program. Returns NULL after reporting the first error. When
// labels_out / debug_out are non-NULL they receive the -g label and debug
// tables, owned by the caller.
struct ProgramDescriptor* assemble(int num_files, const char* const* paths, char** files,
  bool is_kernel, struct LabelList** labels_out, struct DebugInfoList** debug_out);

// Validate and record -DNAME=value definitions applied to every file.
// Returns false after reporting the first malformed or duplicate definition.
// The strings are borrowed and must outlive assemble().
bool set_cli_defines(int count, const char* const* defines);

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

// Which names an operand may refer to and how labels are valued.
// Unless OPERAND_LABELS_FIRST is set, .define names shadow labels.
enum OperandFlags {
  OPERAND_DEFINES = 1,        // .define / -D constants of the current file
  OPERAND_LABELS = 2,         // labels of this file, then .global labels
  OPERAND_PC_RELATIVE = 4,    // labels resolve to label - (pc + 4)
  OPERAND_DEFER_LABELS = 8,   // in pass 1, any unresolved name is accepted as 0
  OPERAND_LABELS_FIRST = 16,  // look up labels before .define names
};

// Operand rules for instruction immediates and for .fill data words.
// Instruction immediates check labels before .define names, while every
// directive checks .define names first; see the note in consume_immediate.
enum {
  OPERAND_INSTRUCTION = OPERAND_DEFINES | OPERAND_LABELS | OPERAND_PC_RELATIVE |
                        OPERAND_DEFER_LABELS | OPERAND_LABELS_FIRST,
  OPERAND_DATA = OPERAND_DEFINES | OPERAND_LABELS | OPERAND_DEFER_LABELS,
};

// What a parsed operand turned out to be.
enum OperandKind {
  OPERAND_LITERAL,    // integer literal (also reported for a malformed literal)
  OPERAND_DEFINE,     // .define / -D constant
  OPERAND_LABEL,      // resolved label
  OPERAND_DEFERRED,   // name accepted as 0 in pass 1
  OPERAND_UNDEFINED,  // name that could not be resolved (result is ERROR)
};

// Parse an integer literal or a name and resolve it according to `flags`.
// `context` names the directive in undefined-name errors (NULL: instruction).
long consume_operand(unsigned flags, const char* context, enum ConsumeResult* result,
                     enum OperandKind* kind_out);

// consume a literal immediate or label immediate (OPERAND_INSTRUCTION rules).
// kind may be NULL.
long consume_immediate(enum ConsumeResult* result, enum OperandKind* kind);

#endif  // ASSEMBLER_H
