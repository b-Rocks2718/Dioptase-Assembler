#ifndef LEXER_H
#define LEXER_H

#include <stdbool.h>

#include "slice.h"

// Lexer cursor shared by the preprocessor and the assembler passes.
// `current` points into a NUL-terminated buffer; `current_buffer_start` bounds
// backward scans when printing the offending line. `line_count` and
// `current_file` are only used for diagnostics.
extern char const * current;
extern char const * current_buffer_start;
extern unsigned line_count;
extern char const * current_file;

// Classify whether operand parsing found a value, found nothing, or failed.
// NOT_FOUND leaves the cursor unchanged; ERROR has already been reported.
enum ConsumeResult {
  ERROR,
  NOT_FOUND,
  FOUND
};

// print line causing an error
// Only the first call prints the location; later diagnostics are bare.
void print_error(void);

// Print a warning with the current source line, then message.
void print_warning(const char* message);

// is the rest of the file just whitespace?
bool is_at_end(void);

// skip whitespace and commas until end of line or non-whitespace character
void skip(void);

// skip until we get to a new nonempty line
void skip_newline(void);

// skip an entire line
void skip_line(void);

// attempt to consume a string, has no effect if a match is not found
bool consume(const char* str);

// attempt to consume a keyword, has no effect if a match is not found
// differs from consume because we ensure that there is a whitespace character at the end
bool consume_keyword(const char* str);

// attempt to consume an identifier, has no effect if a match is not found.
// On success *out views the identifier in the current source buffer.
bool consume_identifier(struct Slice* out);

// attempt to consume "identifier:", has no effect if a match is not found.
bool consume_label(struct Slice* out);

// attempt to consume an unquoted filename (up to whitespace, ',' or ';').
bool consume_filename(struct Slice* out);

// attempt to consume a register: r0-r31 or the aliases sp/bp/ra (abi.md).
// Returns the register number, or -1 without consuming anything.
int consume_register(void);

// attempt to consume a control register: cr0-cr12 or its ISA.md name.
// Returns the register number, or -1 without consuming anything.
int consume_control_register(void);

// attempt to consume an integer literal
long consume_literal(enum ConsumeResult* result);

#endif  // LEXER_H
