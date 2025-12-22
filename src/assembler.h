#pragma once

#include "stdbool.h"

extern char const * current_file;
extern char const * current;
extern unsigned line_count;
extern unsigned long pc;

// does the file wish to use pivileges instructions?
extern bool is_kernel;

struct LabelList;

struct InstructionArrayList* assemble(int num_files, int* file_names, 
  const char *const *const argv, char** files, struct LabelList** labels_out);

enum ConsumeResult {
  ERROR,
  NOT_FOUND,
  FOUND
};

// print line causing an error
void print_error(void);

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

// attempt to consume an identifier, has no effect if a match is not found
struct Slice* consume_identifier(void);

// label is an identifier followed by a colon
struct Slice* consume_label(void);

// attempt to consume a register
int consume_register(void);

int consume_control_register(void);

// attempt to consume an integer literal
long consume_literal(enum ConsumeResult* result);

// consume a literal immediate or label immediate
long consume_immediate(enum ConsumeResult* result);

// consumes a single instruction and converts it to binary or hex
int consume_instruction(enum ConsumeResult* result);
