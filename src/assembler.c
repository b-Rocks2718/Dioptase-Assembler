#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include <string.h>

#include "slice.h"
#include "assembler.h"
#include "hashmap.h"
#include "instruction_array.h"
#include "label_list.h"
#include "preprocessor.h"
#include "elf.h"
#include "debug.h"

/*
  Two-pass assembler.
  First pass calculates addresses of labels
  Second pass converts to text into binary
*/

char const * current;
char const * current_buffer_start = NULL;
unsigned line_count = 1;
unsigned long pc = 0;
unsigned entry_point = 0;

// used by user programs
enum UserSection current_section = -1;
struct InstructionArray* text_instruction_array = NULL;
struct InstructionArray* rodata_instruction_array = NULL;
struct InstructionArray* data_instruction_array = NULL;
unsigned bss_size = 0;
static uint32_t section_offsets[4];
static uint32_t section_sizes[4];
static uint32_t section_bases[4];

static struct DebugInfoList* debug_info_list = NULL;

// does the file wish to use pivileges instructions?
bool is_kernel = false;

char const * current_file;
int current_file_index;
int pass_number = 1;

// Map labels/defines to their addresses or values.
// local_labels: per-file label table used for local resolution and duplicate checks.
// local_defines: per-file .define table to resolve constants without polluting globals.
// local_globals: per-file set of labels declared with .global (used to validate global duplication).
// global_labels: shared table of labels exported across files via .global.
static struct HashMap** local_labels;
static struct HashMap** local_defines;
static struct HashMap** local_globals;
static struct HashMap* global_labels;
static int cli_define_count = 0;
static const char* const* cli_defines = NULL;

// Byte sizing for directive accounting and output packing.
static const uint32_t kWordBytes = 4;
static const uint32_t kHalfBytes = 2;
static const uint32_t kByteBytes = 1;

#define USER_BASE_ADDR 0x80000000u
#define SECTION_ALIGN 0x1000u

static void reset_section_offsets(void) {
  for (int i = 0; i < 4; ++i) section_offsets[i] = 0;
}

static uint32_t align_up(uint32_t value, uint32_t align) {
  uint32_t rem = value % align;
  if (rem == 0) return value;
  return value + (align - rem);
}

// Forward declaration for alignment parsing helpers.
static long consume_define_or_literal(enum ConsumeResult* result, const char* context);

// Purpose: Check whether a value is a power-of-two alignment.
// Inputs: value is the candidate alignment in bytes.
// Outputs: Returns true when value is a nonzero power of two.
// Invariants/Assumptions: value is treated as a 32-bit unsigned alignment.
static bool is_power_of_two_u32(uint32_t value){
  return value != 0 && (value & (value - 1)) == 0;
}

// Purpose: Parse and validate a byte alignment value for .align.
// Inputs: result is filled with FOUND/NOT_FOUND/ERROR; directive labels errors.
// Outputs: Returns true on success and fills alignment_out.
// Invariants/Assumptions: alignment_out is non-NULL.
static bool parse_alignment(enum ConsumeResult* result, const char* directive,
                            uint32_t* alignment_out){
  long imm = consume_define_or_literal(result, directive);
  if (*result != FOUND){
    if (*result == NOT_FOUND){
      print_error();
      fprintf(stderr, "Invalid %s value; expected integer literal or .define constant\n", directive);
    }
    return false;
  }
  if (imm <= 0 || imm >= ((long)1 << 32)){
    print_error();
    fprintf(stderr, "%s value must be a positive 32-bit integer\n", directive);
    return false;
  }
  uint32_t alignment = (uint32_t)imm;
  if (!is_power_of_two_u32(alignment)){
    print_error();
    fprintf(stderr, "%s value must be a power of two\n", directive);
    return false;
  }
  *alignment_out = alignment;
  return true;
}

// Purpose: Encode the least-significant bytes of value in little-endian order.
// Inputs: value is the integer to encode; out must have space for count bytes; count is 1, 2, or 4.
// Outputs: out is filled with count bytes.
// Invariants/Assumptions: count is nonzero and no more than kWordBytes.
static void encode_value_bytes(uint32_t value, uint8_t* out, uint32_t count){
  for (uint32_t i = 0; i < count; ++i){
    out[i] = (uint8_t)(value >> (8 * i));
  }
}

// Purpose: Append raw bytes into a kernel instruction array and advance pc.
// Inputs: arr is the destination array; bytes/count describe the payload.
// Outputs: pc is incremented by count bytes.
// Invariants/Assumptions: bytes are emitted in increasing address order.
static void append_bytes_kernel(struct InstructionArray* arr, const uint8_t* bytes, uint32_t count){
  for (uint32_t i = 0; i < count; ++i){
    instruction_array_append_byte(arr, bytes[i], (int)pc);
    pc += kByteBytes;
  }
}

// Purpose: Append zero bytes into a kernel instruction array and advance pc.
// Inputs: arr is the destination array; count is the number of zero bytes to emit.
// Outputs: pc is incremented by count bytes.
// Invariants/Assumptions: count is a non-negative byte count.
static void append_zero_bytes_kernel(struct InstructionArray* arr, uint32_t count){
  for (uint32_t i = 0; i < count; ++i){
    instruction_array_append_byte(arr, 0, (int)pc);
    pc += kByteBytes;
  }
}

// Purpose: Append raw bytes into a user section array and advance offsets.
// Inputs: arr is the destination array; bytes/count describe the payload; section selects base/offset.
// Outputs: section_offsets and pc are incremented by count bytes.
// Invariants/Assumptions: section_bases are initialized and aligned.
static void append_bytes_user(struct InstructionArray* arr, const uint8_t* bytes, uint32_t count,
                              enum UserSection section){
  for (uint32_t i = 0; i < count; ++i){
    uint32_t abs_pc = section_bases[section] + section_offsets[section];
    instruction_array_append_byte(arr, bytes[i], (int)abs_pc);
    section_offsets[section] += kByteBytes;
  }
  pc = section_bases[section] + section_offsets[section];
}

// Purpose: Append zero bytes into a user section array and advance offsets.
// Inputs: arr is the destination array; count is the number of zero bytes; section selects base/offset.
// Outputs: section_offsets and pc are incremented by count bytes.
// Invariants/Assumptions: section_bases are initialized and aligned.
static void append_zero_bytes_user(struct InstructionArray* arr, uint32_t count, enum UserSection section){
  for (uint32_t i = 0; i < count; ++i){
    uint32_t abs_pc = section_bases[section] + section_offsets[section];
    instruction_array_append_byte(arr, 0, (int)abs_pc);
    section_offsets[section] += kByteBytes;
  }
  pc = section_bases[section] + section_offsets[section];
}

// Purpose: Report misaligned instruction addresses with context.
// Inputs: address is the misaligned byte address or section offset; label describes the address.
// Outputs: Returns false after emitting an error.
// Invariants/Assumptions: print_error has access to current file/line context.
static bool report_instruction_alignment_error(uint32_t address, const char* label){
  print_error();
  fprintf(stderr, "Instruction address must be %u-byte aligned; %s is 0x%08X\n",
          kWordBytes, label, address);
  return false;
}

static void compute_section_bases(void) {
  section_bases[TEXT_SECTION] = USER_BASE_ADDR;
  section_bases[RODATA_SECTION] = align_up(section_bases[TEXT_SECTION] + section_sizes[TEXT_SECTION], SECTION_ALIGN);
  section_bases[DATA_SECTION] = align_up(section_bases[RODATA_SECTION] + section_sizes[RODATA_SECTION], SECTION_ALIGN);
  section_bases[BSS_SECTION] = section_bases[DATA_SECTION] + section_sizes[DATA_SECTION];
}

static uint64_t encode_section_offset(enum UserSection section, uint32_t offset) {
  // Pack section + offset for pass 1; resolved to absolute addresses after layout.
  return ((uint64_t)section << 32) | offset;
}

static void adjust_label_map_for_sections(struct HashMap* map) {
  // Convert packed section offsets into absolute addresses once section sizes are known.
  for (size_t i = 0; i < map->size; ++i){
    struct HashEntry* entry = map->arr[i];
    while (entry != NULL){
      if (entry->is_defined){
        uint64_t raw = (uint64_t)entry->value;
        enum UserSection section = (enum UserSection)(raw >> 32);
        uint32_t offset = (uint32_t)(raw & 0xFFFFFFFFu);
        entry->value = (long)(section_bases[section] + offset);
      }
      entry = entry->next;
    }
  }
}

static bool ensure_valid_section(const char* context) {
  if ((int)current_section < (int)TEXT_SECTION) {
    print_error();
    if (strcmp(context, "label") == 0) {
      fprintf(stderr, "Label defined while not in any section\n");
    } else if (strcmp(context, "instruction") == 0) {
      fprintf(stderr, "cannot use instructions while not in any section\n");
    } else {
      fprintf(stderr, "cannot use %s while not in any section\n", context);
    }
    return false;
  }
  if ((int)current_section > (int)BSS_SECTION) {
    print_error();
    fprintf(stderr, "Invalid section for %s\n", context);
    return false;
  }
  return true;
}

static bool is_identifier_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '.';
}

static bool is_valid_define_name(const char* start, size_t len) {
  if (len == 0) return false;
  if (!isalpha((unsigned char)start[0]) && start[0] != '_') return false;
  for (size_t i = 1; i < len; ++i){
    if (!is_identifier_char(start[i])) return false;
  }
  return true;
}

void set_cli_defines(int count, const char* const* defines){
  cli_define_count = count;
  cli_defines = defines;
}

static bool apply_cli_defines(void){
  if (cli_define_count <= 0) return true;
  for (int i = 0; i < cli_define_count; ++i){
    const char* def = cli_defines[i];
    const char* eq = strchr(def, '=');
    if (eq == NULL || eq == def || *(eq + 1) == '\0'){
      fprintf(stderr, "Invalid -D definition: %s\n", def);
      return false;
    }
    size_t name_len = (size_t)(eq - def);
    if (!is_valid_define_name(def, name_len)){
      fprintf(stderr, "Invalid -D name: %.*s\n", (int)name_len, def);
      return false;
    }

    struct Slice name_view = {def, name_len};
    if (hash_map_contains(local_defines[current_file_index], &name_view)){
      fprintf(stderr, "constant has multiple definitions\n");
      return false;
    }

    const char* old_current = current;
    const char* old_buffer = current_buffer_start;
    unsigned old_line = line_count;
    const char* old_file = current_file;

    current = eq + 1;
    current_buffer_start = current;
    line_count = 1;
    current_file = "<command line>";

    enum ConsumeResult result;
    long value = consume_literal(&result);
    skip();
    bool ok = (result == FOUND) && (*current == '\0');

    current = old_current;
    current_buffer_start = old_buffer;
    line_count = old_line;
    current_file = old_file;

    if (!ok){
      fprintf(stderr, "Invalid -D value for %.*s\n", (int)name_len, def);
      return false;
    }

    struct Slice* label = malloc(sizeof(struct Slice));
    label->start = def;
    label->len = name_len;
    hash_map_insert(local_defines[current_file_index], label, value, true, true);
  }
  return true;
}

static bool consume_named_register(const char* name) {
  size_t len = strlen(name);
  if (strncmp(current, name, len) == 0 && !is_identifier_char(current[len])) {
    current += len;
    return true;
  }
  return false;
}

// print line causing an error
void print_error(void) {
  // avoid printing this twice
  static bool has_printed = false;

  if (!has_printed){
    fprintf(stderr, "Error in %s\nline %u: \"", current_file, line_count);
    
    // get start and end of line
    char const * start = current;
    char const * buffer_start = current_buffer_start != NULL ? current_buffer_start : current;
    while (start > buffer_start && *(start - 1) != '\0' && *(start - 1) != '\n') start--;
    char const * end = current;
    while (*end != '\0' && *end != '\n') end++;
    
    // remove whitespace at beginning and end
    while (isspace(*start)) start++;
    while (isspace(*(end - 1))) end--;

    // print the line
    struct Slice unrecognized = {start, end - start};
    print_slice_err(&unrecognized);
    fprintf(stderr, "\"\n");
    has_printed = true;
  }
}

static void print_warning(const char* message) {
  fprintf(stderr, "Warning in %s\nline %u: \"", current_file, line_count);

  char const * start = current;
  char const * buffer_start = current_buffer_start != NULL ? current_buffer_start : current;
  while (start > buffer_start && *(start - 1) != '\0' && *(start - 1) != '\n') start--;
  char const * end = current;
  while (*end != '\0' && *end != '\n') end++;

  while (isspace(*start)) start++;
  while (end > start && isspace(*(end - 1))) end--;

  struct Slice unrecognized = {start, end - start};
  print_slice_err(&unrecognized);
  fprintf(stderr, "\"\n");
  fprintf(stderr, "%s\n", message);
}

// Allocate a Slice wrapper for shared source buffers so each map owns its key.
static struct Slice* clone_slice(const struct Slice* slice) {
  struct Slice* copy = malloc(sizeof(struct Slice));
  copy->start = slice->start;
  copy->len = slice->len;
  return copy;
}

// is the rest of the file just whitespace?
bool is_at_end(void) {
  while (isspace(*current)) {
    if (*current == '\n') line_count++;
    current += 1;
  }
  if (*current != 0) return false;
  else return true;
}

// skip whitespace and commas until end of line or non-whitespace character
void skip(void) {
  while ((isspace(*current) && *current != '\n') || *current == ',' || *current == ';') {
    current++;
  }
}

// skip until we get to a new nonempty line
void skip_newline(void) {
  while (isspace(*current)) {
    if (*current == '\n') line_count++;
    current++;
  }
}

// skip an entire line
void skip_line(void){
  while (*current != '\n' && *current != '\0') current++; 
  skip_newline();
}

// attempt to consume a string, has no effect if a match is not found
bool consume(const char* str) {
  skip();
  size_t i = 0;
  while (true) {
    char const expected = str[i];
    char const found = current[i];
    if (expected == 0) {
      /* survived to the end of the expected string */
      current += i;
      return true;
    }
    if (expected != found) {
      return false;
    }
    i += 1;
  } 
}

// attempt to consume a keyword, has no effect if a match is not found
// differs from consume because we ensure that there is a whitespace character at the end
bool consume_keyword(const char* str) {
  // skip is handled by caller so that this function is useful for preprocesser/macros
  size_t i = 0;
  while (true) {
    char const expected = str[i];
    char const found = current[i];
    if (expected == 0) {
      /* survived to the end of the expected string */
      if (isspace(found) || found == '\0') {
        // word break
        current += i;
        return true;
      } else {
        // this is actually an identifier
        return false;
      }
    }
    if (expected != found) {
      return false;
    }
    i += 1;
  } 
}

// attempt to consume an identifier, has no effect if a match is not found
struct Slice* consume_identifier(void) {
  skip();
  size_t i = 0;
  // identifiers begin with a letter or underscore
  if (isalpha(current[i]) || current[i] == '_') {
    do {
      i += 1;
      // then followed by letters, number, underscores, and periods
    } while(isalnum(current[i]) || current[i] == '_' || current[i] == '.');

    struct Slice* slice = malloc(sizeof(struct Slice));
    slice->start = current;
    slice->len = i;
    current += i;

    return slice;
  } else {
    return NULL;
  }
}

// attempt to consume a filename, has no effect if a match is not found
struct Slice* consume_filename(void) {
  skip();
  size_t i = 0;
  // filenames begin with a letter or underscore
  if (isalpha(current[i]) || current[i] == '_') {
    do {
      i += 1;
      // then followed by letters, number, underscores, and periods, and slashes
    } while(isalnum(current[i]) || current[i] == '_' || current[i] == '.' || current[i] == '/');

    struct Slice* slice = malloc(sizeof(struct Slice));
    slice->start = current;
    slice->len = i;
    current += i;

    return slice;
  } else {
    return NULL;
  }
}

// label is an identifier followed by a colon
struct Slice* consume_label(void){
  skip();
  char const * old_current = current;
  struct Slice* label = consume_identifier();
  if (consume(":")) return label;

  // undo side effects
  if (label != NULL) free(label);
  current = old_current;
  return NULL;
}

// label is an identifier followed by a colon
bool skip_label(struct InstructionArrayList* instructions){
  skip();
  char const * old_current = current;
  struct Slice* label = consume_identifier();
  if (label != NULL && consume(":")) {
    free(label);
    return true;
  }

  // undo side effects
  if (label != NULL) free(label);
  current = old_current;
  return false;
}

// attempt to consume a register
int consume_register(void) {
  skip();

  if (consume_named_register("sp")) return 31;
  else if (consume_named_register("bp")) return 30;
  else if (consume_named_register("ra")) return 29;

  // registers begin with an r
  else if (current[0] == 'r' && isdigit((unsigned char)current[1])) {
    int v = 0;
    size_t i = 1;
    while(isdigit((unsigned char)current[i])) {
      // then followed by numbers
      v = 10 * v + current[i] - '0';
      i += 1;
    }

    if (v > 31 || is_identifier_char(current[i])) return -1;
    current += i;
    return v;
  }
  else return -1;
}

// attempt to consume a control register
int consume_control_register(void) {
  skip();
  // registers begin with an r
  if (current[0] == 'c' && current[1] == 'r' && isdigit((unsigned char)current[2])) {
    int v = 0;
    size_t i = 2;
    while(isdigit((unsigned char)current[i])) {
      // then followed by numbers
      v = 10 * v + current[i] - '0';
      i += 1;
    }

    if (v > 11 || is_identifier_char(current[i])) return -1;
    current += i;
    return v;
  } else {
    if (consume_named_register("psr")) return 0;
    else if (consume_named_register("pid")) return 1;
    else if (consume_named_register("isr")) return 2;
    else if (consume_named_register("imr")) return 3;
    else if (consume_named_register("epc")) return 4;
    else if (consume_named_register("flg")) return 5;
    else if (consume_named_register("efg")) return 6;
    else if (consume_named_register("tlb")) return 7;
    else if (consume_named_register("ksp")) return 8;
    else if (consume_named_register("cid")) return 9;
    else if (consume_named_register("mbi")) return 10;
    else if (consume_named_register("mbo")) return 11;
    else if (consume_named_register("isp")) return 12;
    else return -1;
  }
}

// attempt to consume an integer literal
long consume_literal(enum ConsumeResult* result) {
  skip();
  bool negate = false;
  char const * old_current = current;
  if (*current == '-') {
    negate = true;
    current++;
    skip();
  }

  // edge case for zero literal
  // (only time leading 0 is allowed)
  if (*current == '0' && 
    (isspace(*(current + 1)) || *(current + 1) == '\0'
      || *(current + 1) == ']' || *(current + 1) == '#')){
      *result = FOUND;
      current++;
      return 0;
  }

  if (isdigit(*current) && *current != '0') {
    // decimal literal
    long v = 0;
    do {
      v = 10*v + ((*current) - '0');
      current += 1;
    } while (isdigit(*current));

    *result = FOUND;
    if (negate) v *= -1;
    return v;
  } else if (*current == '0' && (*(current + 1) == 'b' || *(current + 1) == 'B')) {
    // Binary literal
    current += 2;
    long v = 0;
    bool saw_digit = false;
    while (isdigit((unsigned char)*current)) {
      saw_digit = true;
      if ((*current) - '0' > 1){
        print_error();
        fprintf(stderr, "Invalid binary literal\n");
        *result = ERROR;
        return 0;
      }
      v = 2*v + ((*current) - '0');
      current += 1;
    }

    if (!saw_digit){
      print_error();
      fprintf(stderr, "Binary literal requires at least one digit\n");
      *result = ERROR;
      return 0;
    }

    *result = FOUND;
    if (negate) v *= -1;
    return v;
  } else if (*current == '0' && (*(current + 1) == 'o' || *(current + 1) == 'O')) {
    current += 2;
    // octal literal
    long v = 0;
    bool saw_digit = false;
    while (isdigit((unsigned char)*current)) {
      saw_digit = true;
      if ((*current) - '7' > 0){
        print_error();
        fprintf(stderr, "Invalid octal literal\n");
        *result = ERROR;
        return 0;
      }
      v = 8*v + ((*current) - '0');
      current += 1;
    }

    if (!saw_digit){
      print_error();
      fprintf(stderr, "Octal literal requires at least one digit\n");
      *result = ERROR;
      return 0;
    }

    *result = FOUND;
    if (negate) v *= -1;
    return v;
  } else if (*current == '0' && (*(current + 1) == 'x' || *(current + 1) == 'X')) {
    long v = 0;
    current += 2;
    bool saw_digit = false;
    while (isalnum((unsigned char)*current)) {
      int d;
      if (isdigit(*current)){
        d = *current - '0';
      } else if (isupper(*current) && *current <= 'F'){
        d = *current - 'A' + 10;
      } else if (islower(*current) && *current <= 'f'){
        d = *current - 'a' + 10;
      } else {
        print_error();
        fprintf(stderr, "Invalid hex literal\n");
        *result = ERROR;
        return 0;
      }

      saw_digit = true;
      v = 16*v + d;
      current += 1;
    }

    if (!saw_digit){
      print_error();
      fprintf(stderr, "Hex literal requires at least one digit\n");
      *result = ERROR;
      return 0;
    }

    *result = FOUND;
    if (negate) v *= -1; 
    return v;
  } else {
    current = old_current;
    *result = NOT_FOUND;
    return 0;
  }
}

// Purpose: Parse a numeric literal or a .define constant (no labels allowed).
// Inputs: result is filled with FOUND/NOT_FOUND/ERROR; context labels the directive for errors.
// Outputs: Returns the literal or constant value when FOUND; returns 0 otherwise.
// Invariants/Assumptions: local_defines for the current file is initialized.
static long consume_define_or_literal(enum ConsumeResult* result, const char* context) {
  long imm = consume_literal(result);
  if (*result != NOT_FOUND) return imm;

  struct Slice* name = consume_identifier();
  if (name == NULL) {
    *result = NOT_FOUND;
    return 0;
  }

  if (hash_map_contains(local_defines[current_file_index], name)) {
    imm = hash_map_get(local_defines[current_file_index], name);
    *result = FOUND;
  } else {
    print_error();
    if (context != NULL) {
      fprintf(stderr, "%s constant \"", context);
    } else {
      fprintf(stderr, "Constant \"");
    }
    print_slice_err(name);
    fprintf(stderr, "\" has not been defined\n");
    *result = ERROR;
  }

  free(name);
  return imm;
}

// Purpose: Parse a numeric literal, .define constant, or label absolute address.
// Inputs: result is filled with FOUND/NOT_FOUND/ERROR; context labels the directive for errors.
// Outputs: Returns the literal, constant, or label address when FOUND; returns 0 otherwise.
// Invariants/Assumptions: label maps are absolute-addressed in pass 2.
static long consume_define_or_literal_or_label_abs(enum ConsumeResult* result,
                                                   const char* context) {
  long imm = consume_literal(result);
  if (*result != NOT_FOUND) {
    return imm;
  }

  struct Slice* name = consume_identifier();
  if (name == NULL) {
    *result = NOT_FOUND;
    return 0;
  }

  if (hash_map_contains(local_defines[current_file_index], name)) {
    imm = hash_map_get(local_defines[current_file_index], name);
    *result = FOUND;
    free(name);
    return imm;
  }

  // Allow labels in pass 1 without forcing a definition yet.
  if (pass_number == 1) {
    *result = FOUND;
    free(name);
    return 0;
  }

  if (label_has_definition(local_labels[current_file_index], name)) {
    imm = hash_map_get(local_labels[current_file_index], name);
    // Kernel labels are stored as offsets, so emit absolute addresses for .fill.
    *result = FOUND;
  } else if (label_has_definition(global_labels, name)) {
    imm = hash_map_get(global_labels, name);
    // Kernel labels are stored as offsets, so emit absolute addresses for .fill.
    *result = FOUND;
  } else {
    print_error();
    if (context != NULL) {
      fprintf(stderr, "%s constant/label \"", context);
    } else {
      fprintf(stderr, "Constant/label \"");
    }
    print_slice_err(name);
    fprintf(stderr, "\" has not been defined\n");
    *result = ERROR;
  }

  free(name);
  return imm;
}

long consume_label_imm(enum ConsumeResult* result){
  struct Slice* label = consume_identifier();
  long imm = 0;
  if (label != NULL){

    // don't try to decode labels on first pass
    if (pass_number == 1) {
      *result = FOUND;
      free(label);
      return 0;
    }

    if (label_has_definition(local_labels[current_file_index], label)){
      imm = hash_map_get(local_labels[current_file_index], label) - pc - 4;

      // If this label is global in this file, the global entry should match.
      if (hash_map_contains(local_globals[current_file_index], label) &&
          label_has_definition(global_labels, label))
        assert(imm == hash_map_get(global_labels, label) - pc - 4);
      
      *result = FOUND;
    } else if (label_has_definition(global_labels, label)){
      imm = hash_map_get(global_labels, label) - pc - 4;
      *result = FOUND;
    } else if (hash_map_contains(local_defines[current_file_index], label)){
      imm = hash_map_get(local_defines[current_file_index], label);
      *result = FOUND;
      return imm;
    } else {
      print_error();
      fprintf(stderr, "Label \"");
      print_slice_err(label);
      fprintf(stderr, "\" has not been defined\n");
      *result = ERROR;
    }
    free(label);
  } else {
    *result = NOT_FOUND;
  }
  return imm;
}

// consume a literal immediate or label immediate
long consume_immediate(enum ConsumeResult* result){
  long imm = consume_label_imm(result);
  if (*result == NOT_FOUND){
    imm = consume_literal(result);
  }
  return imm;
}

int encode_bitwise_immediate(long imm, bool* success){
  if (imm == (imm & 0xFF)){
    return imm;
  } else if (imm == (imm & 0xFF00)){
    return (imm >> 8) | (1 << 8);
  } else if (imm == (imm & 0xFF0000)){
    return (imm >> 16) | (2 << 8);
  } else if (imm == (imm & 0xFF000000)){
    return (imm >> 24) | (3 << 8);
  } else {
    *success = false;
    print_error();
    fprintf(stderr, "Bitwise instruction immediate must be an 8 bit value, ");
    fprintf(stderr, "shifted by 0, 8, 16, or 24 bits\n");
    fprintf(stderr, "Got %ld\n", imm);
    return 0;
  }
}

int encode_shift_immediate(long imm, bool* success){
  if (0 <= imm && imm < 31){
    return imm;
  } else {
    *success = false;
    print_error();
    fprintf(stderr, "Shift instruction immediate must be in range 0 to 31\n");
    fprintf(stderr, "Got %ld\n", imm);
    return 0;
  }
}

int encode_arithmetic_immediate(long imm, bool* success){
  if (-(1 << 11) <= imm && imm < (1 << 11)){
    return imm & 0xFFF;
  } else {
    print_error();
    fprintf(stderr, "Arithmetic instruction immediate must be in range -2048 to 2047\n");
    fprintf(stderr, "Got %ld\n", imm);
    *success = false;
    return 0;
  }
}

// consume an alu instruction and return the corresponding encoding
int consume_alu_op(int alu_op, bool* success){
  assert(0 <= alu_op && alu_op < 32); // ensure alu_op is valid

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  // edge case for 'not', 'sxtb', 'sxtd', 'tncb', 'tncd' because they only have 2 parameters
  int rb = 0;
  if (alu_op != 6 && alu_op != 18 && alu_op != 19 && alu_op != 20 && alu_op != 21){
    rb = consume_register();
    if (rb == -1){
      print_error();
      fprintf(stderr, "Invalid register\n");
      fprintf(stderr, "Valid registers are r0 - r31\n");
      *success = false;
      return 0;
    }
  }
  
  int rc = consume_register();
  int instruction = 0;
  if (rc == -1){
    // and ra, rb, imm
    enum ConsumeResult result;
    long imm = consume_immediate(&result);
    if (result != FOUND){
      print_error();
      if (result == NOT_FOUND) fprintf(stderr, "Invalid register or immediate\n");
      *success = false;
      return 0;
    }

    instruction |= 1 << 27; // opcode is 1
    instruction |= ra << 22;
    instruction |= rb << 17;
    instruction |= alu_op << 12;
    
    int encoding;
    if (0 <= alu_op && alu_op < 7){
      // bitwise op
      encoding = encode_bitwise_immediate(imm, success);
    } else if (7 <= alu_op && alu_op < 14) {
      // shift
      encoding = encode_shift_immediate(imm, success);
    } else if (14 <= alu_op && alu_op < 19) {
      // arithmetic op
      encoding = encode_arithmetic_immediate(imm, success);
    } else {
      // invalid alu op for immediate
      print_error();
      fprintf(stderr, "ALU operation %d does not support immediate values\n", alu_op);
      *success = false;
      return 0;
    }

    assert(encoding == (encoding & 0xFFF)); // ensure encoding always fits in 12 bits

    instruction |= encoding;
  } else {
    // and ra, rb, rc
    // opcode is 0
    instruction |= ra << 22;
    instruction |= rb << 17;
    instruction |= rc;
    instruction |= alu_op << 5;
  }
  
  return instruction; 
}

int consume_cmp(bool* success){
  int rb = consume_register();
  if (rb == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }
  
  int rc = consume_register();
  int instruction = 0;
  if (rc == -1){
    // and ra, rb, imm
    enum ConsumeResult result;
    long imm = consume_immediate(&result);
    if (result != FOUND){
      print_error();
      if (result == NOT_FOUND) fprintf(stderr, "Invalid register or immediate\n");
      *success = false;
      return 0;
    }

    instruction |= 1 << 27; // opcode is 1
    instruction |= rb << 17;
    instruction |= 16 << 12; // alu_op
    
    int encoding = encode_arithmetic_immediate(imm, success);

    assert(encoding == (encoding & 0xFFF)); // ensure encoding always fits in 12 bits

    instruction |= encoding;
  } else {
    // opcode is 0
    instruction |= rb << 17;
    instruction |= rc;
    instruction |= 16 << 5; // alu_op
  }
  
  return instruction; 
}

int encode_lui_immediate(long imm, bool* success){
  if ((imm & 0x3FF) == 0 && imm < ((long)1 << 32)){
    return ((int)imm >> 10) & 0x3FFFFF;
  } else {
    *success = false;
    print_error();
    fprintf(stderr, "lui immediate must be a 32 bit integer with zero for bottom 10 bits\n");
    fprintf(stderr, "Got %ld\n", imm);
    return 0;
  }
}

int consume_lui(bool* success){
  enum ConsumeResult result;
  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  long imm = consume_immediate(&result);
  if (result != FOUND){
    print_error();
    fprintf(stderr, "Invalid immediate\n");
    *success = false;
  }

  int encoding = encode_lui_immediate(imm, success);

  assert(encoding == (encoding & 0x3FFFFF)); // ensure immediate fits in 22 bits

  int instruction = 2 << 27;
  instruction |= ra << 22;
  instruction |= encoding;
  return instruction;
}

int encode_absolute_memory_immediate(long imm, bool* success){
  // top n bits must all be 0s or all be 1s
  // bottom m bits must be 0s
  // the 12 bits in the middle become part of the instruction
  if (imm == (imm & 0x7FF) || ~imm == (~imm & 0x7FF)){
    return imm & 0xFFF;
  } else if ((imm == (imm & 0xFFF) || ~imm == (~imm & 0xFFF)) && ((imm & 1) == 0)){
    return ((imm >> 1) & 0xFFF)| (1 << 12);
  } else if ((imm == (imm & 0x1FFF) || ~imm == (~imm & 0x1FFF)) && ((imm & 3) == 0)){
    return ((imm >> 2) & 0xFFF) | (2 << 12);
  } else if ((imm == (imm & 0x3FFF) || ~imm == (~imm & 0x3FFF)) && ((imm & 7) == 0)){
    return ((imm >> 3) & 0xFFF) | (3 << 12);
  } else {
    // can't encode
    print_error();
    fprintf(stderr, "Invalid immediate for memory instruction\n");
    fprintf(stderr, "Immediate must be a 12 bit number shifted by 0, 1, 2, or 3\n");
    fprintf(stderr, "Got %ld\n", imm);
    *success = false;
    return 0;
  }
}

int encode_relative_memory_immediate(long imm, bool* success){
  if (-(1L << 15) <= imm && imm < (1L << 15)){
    return (int)imm & 0xFFFF;
  } else {
    // can't encode
    print_error();
    fprintf(stderr, "Invalid immediate for memory instruction\n");
    fprintf(stderr, "Immediate must fit in signed 16 bits (-32768 to 32767)\n");
    fprintf(stderr, "Got %ld\n", imm);
    *success = false;
    return 0;
  }
}

int encode_long_relative_memory_immediate(long imm, bool* success){
  if (-(1L << 20) <= imm && imm < (1L << 20)){
    return (int)imm & 0x1FFFFF;
  } else {
    // can't encode
    print_error();
    fprintf(stderr, "Invalid immediate for memory instruction\n");
    fprintf(stderr, "Immediate must fit in signed 21 bits (-1048576 to 1048575)\n");
    fprintf(stderr, "Got %ld\n", imm);
    *success = false;
    return 0;
  }
}

int consume_mem(int width_type, bool is_absolute, bool is_load, bool* success){
  int instruction = 0;

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  if (!consume("[")){
    *success = false;
    print_error();
    fprintf(stderr, "Expected \"[\" in memory instruction\n");
    return 0;
  }

  int rb = consume_register();
  if (rb == -1){
    if (is_absolute){
      print_error();
      fprintf(stderr, "Invalid register\n");
      fprintf(stderr, "Valid registers are r0 - r31\n");
      *success = false;
      return 0;
    }
  }

  long imm = 0;
  int y = 0; // absolute addressing mode selector: 0=offset, 1=preinc, 2=postinc

  if (consume("]")){
    if (is_absolute){
      enum ConsumeResult result;
      imm = consume_literal(&result);
      if (result == FOUND){
        // postincrement: [rb], imm
        if (!is_absolute){
          print_error();
          fprintf(stderr, "Postincrement addressing not allowed for relative addressing\n");
          *success = false;
          return 0;
        }
        y = 2;
      } else if (result == NOT_FOUND){
        // no offset: [rb]
        imm = 0;
        y = 0;
      } else {
        // error
        *success = false;
        return 0;
      }
    }
  } else {
    enum ConsumeResult result;
    imm = consume_immediate(&result);
    if (result == FOUND){
      if (!consume("]")){
        print_error();
        fprintf(stderr, "Expected \"]\" in memory instruction\n");
        *success = false;
        return 0;
      }
      if (consume("!")){
        // preincrement: [rb, imm]!
        if (!is_absolute){
          print_error();
          fprintf(stderr, "Preincrement addressing not allowed for relative addressing\n");
          *success = false;
          return 0;
        }
        y = 1;
      } else {
        // signed offset: [rb, imm]
        y = 0;
      }
    } else {
      // error
      print_error();
      fprintf(stderr, "Invalid immediate in memory instruction\n");
      *success = false;
      return 0;
    }
  }
  int encoding;
  
  if (is_absolute) encoding = encode_absolute_memory_immediate(imm, success);
  else if (rb != -1) encoding = encode_relative_memory_immediate(imm, success);
  else encoding = encode_long_relative_memory_immediate(imm, success);

  // opcode
  if (is_absolute){
    instruction |= (3 + 3 * width_type) << 27;
  } else if (rb != -1) {
    instruction |= (4 + 3 * width_type) << 27;
  } else {
    instruction |= (5 + 3 * width_type) << 27;
  }

  if (is_load){
    if (rb != -1) instruction |= 1 << 16;
    else instruction |= 1 << 21;
  }

  instruction |= ra << 22;
  
  if (is_absolute){
    instruction |= y << 14;
    instruction |= rb << 17;
    assert(encoding == (encoding & 0x3FFF)); // ensure encoding is 14 bits
  } else if (rb != -1) {
    assert(encoding == (encoding & 0xFFFF)); // ensure encoding is 16 bits
    instruction |= rb << 17;
  } else {
    assert(encoding == (encoding & 0x1FFFFF)); // ensure encoding is 21 bits
  }

  instruction |= encoding;

  return instruction;
}

int encode_branch_immediate(long imm, bool* success){
  if (-(1 << 23) <= imm && imm < (1 << 23) && (imm & 3) == 0){
    return (imm >> 2) & 0x3FFFFF;
  } else {
    *success = false;
    print_error();
    fprintf(stderr, "branch immediate must be divisible by 4 and in range -8388608 to 8388607\n");
    fprintf(stderr, "Got %ld\n", imm);
    return 0;
  }
}

int encode_adpc_immediate(long imm, bool* success){
  if (-(1L << 21) <= imm && imm < (1L << 21)){
    return (int)imm & 0x3FFFFF;
  } else {
    *success = false;
    print_error();
    fprintf(stderr, "adpc immediate must fit in signed 22 bits (-2097152 to 2097151)\n");
    fprintf(stderr, "Got %ld\n", imm);
    return 0;
  }
}

int consume_branch(int branch_code, bool is_absolute, bool* success){
  int instruction = 0;

  assert(0 <= branch_code && branch_code < 19); // ensure branch code is valid

  int ra = consume_register();
  if (ra == -1){
    // it's an immediate branch
    enum ConsumeResult result;
    int imm = consume_immediate(&result);
    if (result != FOUND){
      print_error();
      if (result == NOT_FOUND) fprintf(stderr, "Branch instruction expects register or immediate operand\n");
      *success = false;
      return 0;
    }
    if (is_absolute){
      print_error();
      fprintf(stderr, "Immediate branch is not allowed for absolute branches\n");
      *success = false;
      return 0;
    }
    int encoding = encode_branch_immediate(imm, success);
    instruction |= 12 << 27; // opcode
    instruction |= branch_code << 22;
    instruction |= encoding;
  } else {
    // register branch
    int rb = consume_register();
    if (rb == -1){
      // ra was omitted
      rb = ra;
      ra = 0;
    }
    if (is_absolute) instruction |= 13 << 27; // opcode
    else instruction |= 14 << 27; // opcode
    instruction |= branch_code << 22;
    instruction |= ra << 5;
    instruction |= rb;
  }

  return instruction;
}

int consume_adpc(bool* success){
  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  enum ConsumeResult result;
  long imm = consume_immediate(&result);
  if (result != FOUND){
    print_error();
    if (result == NOT_FOUND) fprintf(stderr, "adpc expects immediate or label\n");
    *success = false;
    return 0;
  }

  int encoding = encode_adpc_immediate(imm, success);
  int instruction = 0;
  instruction |= 22 << 27;
  instruction |= ra << 22;
  instruction |= encoding;
  return instruction;
}

// Alias for unconditional branches
int consume_jmp(bool* success){
  int instruction = 0;

  int ra = consume_register();
  if (ra == -1){
    // it's an immediate branch
    enum ConsumeResult result;
    int imm = consume_immediate(&result);
    if (result != FOUND){
      print_error();
      if (result == NOT_FOUND) fprintf(stderr, "Branch instruction expects register or immediate operand\n");
      *success = false;
      return 0;
    }
    
    int encoding = encode_branch_immediate(imm, success);
    instruction |= 12 << 27; // opcode
    instruction |= encoding;
  } else {
    // register branch
    instruction |= 13 << 27; // opcode
    instruction |= ra;
  }

  return instruction;
}

int consume_syscall(bool* success){
  if (consume("EXIT")){
    int instruction = 15 << 27;
    instruction |= 1;
    return instruction;
  } else {
    // unrecognized call
    print_error();
    fprintf(stderr, "Unrecognized syscall\n");
    fprintf(stderr, "Supported syscalls are: EXIT\n");
    *success = false;
    return 0;
  }
}

int encode_short_atomic_immediate(long imm, bool* success){
  if (-(1L << 11) <= imm && imm < (1L << 11)){
    return (int)imm & 0xFFF;
  } else {
    // can't encode
    print_error();
    fprintf(stderr, "Invalid immediate for memory instruction\n");
    fprintf(stderr, "Immediate must fit in signed 12 bits (-2048 to 2047)\n");
    fprintf(stderr, "Got %ld\n", imm);
    *success = false;
    return 0;
  }
}

int encode_long_atomic_immediate(long imm, bool* success){
  if (-(1L << 16) <= imm && imm < (1L << 16)){
    return (int)imm & 0x1FFFF;
  } else {
    // can't encode
    print_error();
    fprintf(stderr, "Invalid immediate for memory instruction\n");
    fprintf(stderr, "Immediate must fit in signed 17 bits (-65536 to 65535)\n");
    fprintf(stderr, "Got %ld\n", imm);
    *success = false;
    return 0;
  }
}

int consume_atomic(bool is_absolute, bool is_fadd, bool* success){
  int instruction = 0;

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  int rc = consume_register();
  if (rc == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  if (!consume("[")){
    *success = false;
    print_error();
    fprintf(stderr, "Expected \"[\" in memory instruction\n");
    return 0;
  }

  int rb = consume_register();
  if (rb == -1){
    if (is_absolute){
      print_error();
      fprintf(stderr, "Invalid register\n");
      fprintf(stderr, "Valid registers are r0 - r31\n");
      *success = false;
      return 0;
    }
  }

  long imm = 0;

  if (consume("]")){
    // no offset
    imm = 0;
  } else {
    // signed offset
    enum ConsumeResult result;
    imm = consume_immediate(&result);
    if (result == FOUND){
      if (!consume("]")){
        print_error();
        fprintf(stderr, "Expected \"]\" in memory instruction\n");
        *success = false;
        return 0;
      }
    } else {
      // error
      print_error();
      fprintf(stderr, "Invalid immediate in memory instruction\n");
      *success = false;
      return 0;
    }
  }
  int encoding;
  
  if (is_absolute) encoding = encode_short_atomic_immediate(imm, success);
  else if (rb != -1) encoding = encode_short_atomic_immediate(imm, success);
  else encoding = encode_long_atomic_immediate(imm, success);

  // opcode
  if (is_absolute){
    instruction |= (is_fadd ? 16 : 19) << 27;
  } else if (rb != -1) {
    instruction |= (is_fadd ? 17 : 20) << 27;
  } else {
    instruction |= (is_fadd ? 18 : 21) << 27;
  }

  instruction |= ra << 22;
  instruction |= rc << 17;
  
  if (is_absolute){
    assert(encoding == (encoding & 0xFFF)); // ensure encoding is 12 bits
    instruction |= rb << 12;
  } else if (rb != -1) {
    assert(encoding == (encoding & 0xFFF)); // ensure encoding is 12 bits
    instruction |= rb << 12;
  } else {
    assert(encoding == (encoding & 0x1FFFF)); // ensure encoding is 17 bits
  }

  instruction |= encoding;

  return instruction;
}

void check_privileges(bool* success){
  static bool has_printed = false;
  // Privileged instructions require -kernel flag
  if (!is_kernel){
    *success = false;
    if (!has_printed){
      has_printed = true;
      print_error();
      fprintf(stderr, "Used privileged instruction\n");
      fprintf(stderr, "Run assembler with -kernel if this was intentional\n");
    }
  }
}

int consume_tlb_op(int tlb_op, bool* success){
  check_privileges(success);
  if (!*success) return 0;

  assert(0 <= tlb_op && tlb_op < 4); // ensure tlb op is valid

  int instruction = 31 << 27; // opcode

  if (tlb_op == 3){
    // tlbc
    instruction |= 3 << 10;
  } else if (tlb_op == 2){
    // tlbi
    instruction |= 2 << 10;

    int rb = consume_register();
    if (rb == -1){
      print_error();
      fprintf(stderr, "Invalid register\n");
      fprintf(stderr, "Valid registers are r0 - r31\n");
      *success = false;
      return 0;
    }

    instruction |= rb << 17;

  } else {
    // tlbr or tlbw
    int ra = consume_register();
    if (ra == -1){
      print_error();
      fprintf(stderr, "Invalid register\n");
      fprintf(stderr, "Valid registers are r0 - r31\n");
      *success = false;
      return 0;
    }
    int rb = consume_register();
    if (rb == -1){
      print_error();
      fprintf(stderr, "Invalid register\n");
      fprintf(stderr, "Valid registers are r0 - r31\n");
      *success = false;
      return 0;
    }

    instruction |= ra << 22;
    instruction |= rb << 17;

    if (tlb_op == 1){
      instruction |= 1 << 10;
    }
  }

  return instruction;
}

int consume_crmv(bool* success){
  check_privileges(success);
  if (!*success) return 0;

  int instruction = 31 << 27;
  instruction |= 1 << 12;

  int ra = consume_register();
  int rb;
  if (ra == -1){
    ra = consume_control_register();
    if (ra == -1){
      print_error();
      fprintf(stderr, "Invalid register or control register\n");
      *success = false;
      return 0; 
    }
    rb = consume_control_register();
    if (rb == -1) {
      rb = consume_register();
      if (rb == -1){
        print_error();
        fprintf(stderr, "Invalid control register\n");
        *success = false;
        return 0; 
      }
      // crmv crA, rB
      instruction |= 4 << 10;
    } else {
      // crmv crA, crB
      instruction |= 6 << 10;
    }
  } else {
    rb = consume_control_register();
    if (rb == -1) {
      rb = consume_register();
      if (rb == -1){
        print_error();
        fprintf(stderr, "Invalid register or control register\n");
        *success = false;
        return 0; 
      }
      // crmv rA, rB
      instruction |= 7 << 10;
    } else {
      // crmv rA, crB
      instruction |= 5 << 10;
    }
  }
  instruction |= ra << 22;
  instruction |= rb << 17;

  return instruction;
}

int consume_mode_op(bool* success){
  check_privileges(success);
  if (!*success) return 0;

  int instruction = 31 << 27; // opcode
  instruction |= 2 << 12;

  if (consume("run"));
  else if (consume("sleep")){
    instruction |= 1 << 10;
  } else if (consume("halt")){
    instruction |= 2 << 10;
  } else {
    print_error();
    fprintf(stderr, "Invalid mode\n");
    fprintf(stderr, "Valid modes are: run, sleep, or halt\n");
    *success = false;
    return 0;
  }

  return instruction;
}

int consume_rfe(int r_type, bool* success){
  assert(0 <= r_type && r_type <= 1);
  check_privileges(success);
  if (!*success) return 0;

  int instruction = 31 << 27;
  instruction |= r_type << 11;
  instruction |= 3 << 12;

  return instruction;
}

int consume_ipi(bool* success){
  check_privileges(success);
  if (!*success) return 0;

  int instruction = 31 << 27; // opcode
  instruction |= 4 << 12; // ID

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  skip();

  instruction |= ra << 22;
  
  if (consume_keyword("all")) {
    // ipi to all cores
    instruction |= 1 << 11;
  } else {
    // ipi to a specific core
    enum ConsumeResult result;
    int imm = consume_literal(&result);
    if (result != FOUND || imm < 0 || imm >= 4){
      print_error();
      if (result == NOT_FOUND) fprintf(stderr, "ipi instruction expects 'all' or core num in range [0, 3]\n");
      *success = false;
      return 0;
    }

    assert(0 <= imm && imm < 4);

    instruction |= imm;
  }

  return instruction;
}

// consume a mov hack return the corresponding encoding
int consume_mov_hack(int mov_type, bool* success){
  assert(0 <= mov_type && mov_type < 4); // ensure mov_type is valid

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  enum ConsumeResult result;

  const char* old_current = current;
  int imm = consume_label_imm(&result); // don't encode bottom two bits of pc  
  if (result == FOUND) {
    current = old_current;
    struct Slice* label = consume_identifier();

    // hack to see if this was a .define and not a label
    if (!hash_map_contains(local_defines[current_file_index], label)) mov_type |= 2;

    free(label);
  }
  else imm = consume_literal(&result);
  if (result != FOUND){
    print_error();
    if (result == NOT_FOUND) fprintf(stderr, "movi expects label or integer literal\n");
    *success = false;
    return 0;
  }

  // movu8 and movl4 are used when the immediate is a label
  // normal movu and movl used otherwise

  // [0] movu := lui rA, (imm & 0xFFFFFC00)
  // [1] movl := addi rA, rA, (imm & 0x3FF)
  // [2] movu8 := lui rA, ((imm - 8) & 0xFFFFFC00)
  // [3] movl4 := addi rA, rA, ((imm - 4) & 0x3FF)

  if (mov_type == 2) imm -= 8;
  else if (mov_type == 3) imm -= 4;
  
  int instruction = 0;

  if (mov_type & 1){
    // this is movl or movl8

    instruction |= 1 << 27; // opcode for add
    instruction |= ra << 22;
    instruction |= ra << 17;
    instruction |= 14 << 12; // add is 14

    int encoding = encode_arithmetic_immediate(imm & 0x3FF, success);

    assert(encoding == (encoding & 0xFFF)); // ensure encoding always fits in 12 bits

    instruction |= encoding;
  } else {
    // this is movu or movu8
    int encoding = encode_lui_immediate(imm & 0xFFFFFC00, success);

    assert(encoding == (encoding & 0x3FFFFF)); // ensure immediate fits in 22 bits

    instruction = 2 << 27; // opcode for lui
    instruction |= ra << 22;
    instruction |= encoding;
  }
  
  return instruction; 
}

void record_define(bool* success){
  struct Slice* label = consume_identifier();
  if (label == NULL){
    // error
    print_error();
    fprintf(stderr, "Expected label\n");
    *success = false;
    return;
  }

  enum ConsumeResult result;
  long imm = consume_literal(&result);
  if (result == NOT_FOUND){
    struct Slice* value_label = consume_identifier();
    if (value_label == NULL){
      print_error();
      free(label);
      fprintf(stderr, "Expected integer literal or label\n");
      *success = false;
      return;
    }
    if (hash_map_contains(local_defines[current_file_index], value_label)){
      imm = hash_map_get(local_defines[current_file_index], value_label);
    } else if (label_has_definition(local_labels[current_file_index], value_label)){
      imm = hash_map_get(local_labels[current_file_index], value_label);
    } else if (label_has_definition(global_labels, value_label)){
      imm = hash_map_get(global_labels, value_label);
    } else {
      print_error();
      fprintf(stderr, "Label \"");
      print_slice_err(value_label);
      fprintf(stderr, "\" has not been defined\n");
      free(value_label);
      free(label);
      *success = false;
      return;
    }
    free(value_label);
  } else if (result != FOUND){
    // error
    print_error();
    free(label);
    fprintf(stderr, "Expected integer literal or label\n");
    *success = false;
    return;
  }

  if (hash_map_contains(local_defines[current_file_index], label)){
    // error
    print_error();
    free(label);
    fprintf(stderr, "constant has multiple definitions\n");
    *success = false;
    return;
  }
  hash_map_insert(local_defines[current_file_index], label, imm, true, true);  
}

// consumes a single instruction and converts it to binary or hex
int consume_instruction(enum ConsumeResult* result){
  int instruction = 0;
  bool success = true;

  // user instructions
  skip();

  // alu instructions
  if (consume_keyword("and")) instruction = consume_alu_op(0, &success);
  else if (consume_keyword("nand")) instruction = consume_alu_op(1, &success);
  else if (consume_keyword("or")) instruction = consume_alu_op(2, &success);
  else if (consume_keyword("nor")) instruction = consume_alu_op(3, &success);
  else if (consume_keyword("xor")) instruction = consume_alu_op(4, &success);
  else if (consume_keyword("xnor")) instruction = consume_alu_op(5, &success);
  else if (consume_keyword("not")) instruction = consume_alu_op(6, &success);
  else if (consume_keyword("lsl")) instruction = consume_alu_op(7, &success);
  else if (consume_keyword("lsr")) instruction = consume_alu_op(8, &success);
  else if (consume_keyword("asr")) instruction = consume_alu_op(9, &success);
  else if (consume_keyword("rotl")) instruction = consume_alu_op(10, &success);
  else if (consume_keyword("rotr")) instruction = consume_alu_op(11, &success);
  else if (consume_keyword("lslc")) instruction = consume_alu_op(12, &success);
  else if (consume_keyword("lsrc")) instruction = consume_alu_op(13, &success);
  else if (consume_keyword("add")) instruction = consume_alu_op(14, &success);
  else if (consume_keyword("addc")) instruction = consume_alu_op(15, &success);
  else if (consume_keyword("sub")) instruction = consume_alu_op(16, &success);
  else if (consume_keyword("subb")) instruction = consume_alu_op(17, &success);
  else if (consume_keyword("cmp")) instruction = consume_cmp(&success);
  else if (consume_keyword("sxtb")) instruction = consume_alu_op(18, &success);
  else if (consume_keyword("sxtd")) instruction = consume_alu_op(19, &success);
  else if (consume_keyword("tncb")) instruction = consume_alu_op(20, &success);
  else if (consume_keyword("tncd")) instruction = consume_alu_op(21, &success);
  
  // load upper immediate
  else if (consume_keyword("lui")) instruction = consume_lui(&success);
  
  // memory instructions
  else if (consume_keyword("swa")) instruction = consume_mem(0, true, false, &success);
  else if (consume_keyword("lwa")) instruction = consume_mem(0, true, true, &success);
  else if (consume_keyword("sw")) instruction = consume_mem(0, false, false, &success);
  else if (consume_keyword("lw")) instruction = consume_mem(0, false, true, &success);
  else if (consume_keyword("sda")) instruction = consume_mem(1, true, false, &success);
  else if (consume_keyword("lda")) instruction = consume_mem(1, true, true, &success);
  else if (consume_keyword("sd")) instruction = consume_mem(1, false, false, &success);
  else if (consume_keyword("ld")) instruction = consume_mem(1, false, true, &success);
  else if (consume_keyword("sba")) instruction = consume_mem(2, true, false, &success);
  else if (consume_keyword("lba")) instruction = consume_mem(2, true, true, &success);
  else if (consume_keyword("sb")) instruction = consume_mem(2, false, false, &success);
  else if (consume_keyword("lb")) instruction = consume_mem(2, false, true, &success);
  
  // branch instructions
  else if (consume_keyword("br")) instruction = consume_branch(0, false, &success);
  else if (consume_keyword("bz")) instruction = consume_branch(1, false, &success);
  else if (consume_keyword("bnz")) instruction = consume_branch(2, false, &success);
  else if (consume_keyword("bs")) instruction = consume_branch(3, false, &success);
  else if (consume_keyword("bns")) instruction = consume_branch(4, false, &success);
  else if (consume_keyword("bc")) instruction = consume_branch(5, false, &success);
  else if (consume_keyword("bnc")) instruction = consume_branch(6, false, &success);
  else if (consume_keyword("bo")) instruction = consume_branch(7, false, &success);
  else if (consume_keyword("bno")) instruction = consume_branch(8, false, &success);
  else if (consume_keyword("bps")) instruction = consume_branch(9, false, &success);
  else if (consume_keyword("bnps")) instruction = consume_branch(10, false, &success);
  else if (consume_keyword("bg")) instruction = consume_branch(11, false, &success);
  else if (consume_keyword("bge")) instruction = consume_branch(12, false, &success);
  else if (consume_keyword("bl")) instruction = consume_branch(13, false, &success);
  else if (consume_keyword("ble")) instruction = consume_branch(14, false, &success);
  else if (consume_keyword("ba")) instruction = consume_branch(15, false, &success);
  else if (consume_keyword("bae")) instruction = consume_branch(16, false, &success);
  else if (consume_keyword("bb")) instruction = consume_branch(17, false, &success);
  else if (consume_keyword("bbe")) instruction = consume_branch(18, false, &success);
  else if (consume_keyword("bra")) instruction = consume_branch(0, true, &success);
  else if (consume_keyword("bza")) instruction = consume_branch(1, true, &success);
  else if (consume_keyword("bnza")) instruction = consume_branch(2, true, &success);
  else if (consume_keyword("bsa")) instruction = consume_branch(3, true, &success);
  else if (consume_keyword("bnsa")) instruction = consume_branch(4, true, &success);
  else if (consume_keyword("bca")) instruction = consume_branch(5, true, &success);
  else if (consume_keyword("bnca")) instruction = consume_branch(6, true, &success);
  else if (consume_keyword("boa")) instruction = consume_branch(7, true, &success);
  else if (consume_keyword("bnoa")) instruction = consume_branch(8, true, &success);
  else if (consume_keyword("bpa")) instruction = consume_branch(9, true, &success);
  else if (consume_keyword("bnpa")) instruction = consume_branch(10, true, &success);
  else if (consume_keyword("bga")) instruction = consume_branch(11, true, &success);
  else if (consume_keyword("bgea")) instruction = consume_branch(12, true, &success);
  else if (consume_keyword("bla")) instruction = consume_branch(13, true, &success);
  else if (consume_keyword("blea")) instruction = consume_branch(14, true, &success);
  else if (consume_keyword("baa")) instruction = consume_branch(15, true, &success);
  else if (consume_keyword("baea")) instruction = consume_branch(16, true, &success);
  else if (consume_keyword("bba")) instruction = consume_branch(17, true, &success);
  else if (consume_keyword("bbea")) instruction = consume_branch(18, true, &success);
  else if (consume_keyword("jmp")) instruction = consume_jmp(&success);

  // pc-relative to absolute address
  else if (consume_keyword("adpc")) instruction = consume_adpc(&success);
  
  // system calls
  else if (consume_keyword("sys")) instruction = consume_syscall(&success);
  
  // atomic instructions
  else if (consume_keyword("fada")) instruction = consume_atomic(true, true, &success);
  else if (consume_keyword("fad")) instruction = consume_atomic(false, true, &success);
  else if (consume_keyword("swpa")) instruction = consume_atomic(true, false, &success);
  else if (consume_keyword("swp")) instruction = consume_atomic(false, false, &success);

  // privileged instructions
  else if (consume_keyword("tlbr")) instruction = consume_tlb_op(0, &success);
  else if (consume_keyword("tlbw")) instruction = consume_tlb_op(1, &success);
  else if (consume_keyword("tlbi")) instruction = consume_tlb_op(2, &success);
  else if (consume_keyword("tlbc")) instruction = consume_tlb_op(3, &success);
  else if (consume_keyword("crmv")) instruction = consume_crmv(&success);
  else if (consume_keyword("mode")) instruction = consume_mode_op(&success);
  else if (consume_keyword("rfe")) instruction = consume_rfe(0, &success);
  else if (consume_keyword("rfi")) instruction = consume_rfe(1, &success);
  else if (consume_keyword("ipi")) instruction = consume_ipi(&success);

  // hacks to make movi and call work
  else if (consume_keyword("movu")) instruction = consume_mov_hack(0, &success);
  else if (consume_keyword("movl")) instruction = consume_mov_hack(1, &success);

  else *result = NOT_FOUND;
  
  if (!success) *result = ERROR;
  
  return instruction;
}

// Purpose: First pass to collect labels and section sizes without emitting output.
// Inputs: prog is the preprocessed source buffer for one file.
// Outputs: Returns true on success; updates label maps and section offsets.
// Invariants/Assumptions: current_file_index is set; section_offsets track byte offsets.
bool process_labels(char const* const prog){
  current = prog;
  current_buffer_start = prog - 1;
  line_count = 1;

  local_labels[current_file_index] = create_hash_map(1000);
  local_defines[current_file_index] = create_hash_map(1000);
  local_globals[current_file_index] = create_hash_map(1000);
  if (!apply_cli_defines()) return false;

  while (!is_at_end()){

    struct Slice* label = consume_label();
    if (label != NULL) {
      bool label_was_used = false;
      long label_value = pc;
      if (!is_kernel){
        if (!ensure_valid_section("label")) {
          free(label);
          return false;
        }
        label_value = (long)encode_section_offset(current_section, section_offsets[current_section]);
      }

      // check for duplicates 
      if (hash_map_contains(local_labels[current_file_index], label)){
        if (label_has_definition(local_labels[current_file_index], label)){
          // duplicate label error
          print_error();
          fprintf(stderr, "Duplicate label\n");
          free(label);
          return false;
        } else {
          make_defined(local_labels[current_file_index], label, label_value);
        }
      } else {
        hash_map_insert(local_labels[current_file_index], label, label_value, true, current_section != TEXT_SECTION);
        label_was_used = true;
      }

      // Check for duplicates on globals explicitly declared in this file.
      if (hash_map_contains(local_globals[current_file_index], label)){
        if (label_has_definition(global_labels, label)){
          // duplicate label error
          print_error();
          fprintf(stderr, "Duplicate global label\n");
          if (!label_was_used) free(label);
          return false;
        } else {
          make_defined(global_labels, label, label_value);
        }
      }

      if (!label_was_used) free(label);

    } else {
      skip();
      if (consume_keyword(".global")) {
        struct Slice* label = consume_identifier();
        if (label != NULL){
          // Track per-file global declarations to detect duplicate exports.
          if (!hash_map_contains(local_globals[current_file_index], label)){
            struct Slice* label_copy = clone_slice(label);
            hash_map_insert(local_globals[current_file_index], label_copy, 0, false,
              current_section != TEXT_SECTION); // mark as data if not in text section
          }
          if (!hash_map_contains(global_labels, label)){
            struct Slice* label_copy = clone_slice(label);
            hash_map_insert(global_labels, label_copy, 0, false, current_section != TEXT_SECTION);
          }

          if (label_has_definition(local_labels[current_file_index], label)){
            if (label_has_definition(global_labels, label)){
              print_error();
              fprintf(stderr, "Duplicate global label\n");
              return false;
            }
            make_defined(global_labels, label, hash_map_get(local_labels[current_file_index], label));
          }
          free(label);
        } else {
          print_error();
          fprintf(stderr, ".global directive requires a label\n");
          return false;
        }

        continue;
      } else if (consume_keyword(".origin")) { 
        if (!is_kernel){
          print_error();
          fprintf(stderr, ".origin can only be used in kernel mode\n");
          return false;
        }

        enum ConsumeResult result; 
        long imm = consume_literal(&result);
        if (result != FOUND){
          print_error();
          fprintf(stderr, "\n");
          return false;
        }
        if (imm < pc){
          print_error();
          fprintf(stderr, ".origin cannot be used to go backwards\n");
          return false;
        } else if (imm >= ((long)1 << 32)){
          print_error();
          fprintf(stderr, ".origin address must be a 32 bit integer\n");
          return false;
        }
        pc = imm;
        continue;
      }
      else if (consume_keyword(".text")) {
        if (is_kernel){
          print_error();
          fprintf(stderr, ".text can only be used in user mode\n");
          return false;
        }
        current_section = TEXT_SECTION;
        pc = section_offsets[current_section];
        continue;
      }
      else if (consume_keyword(".rodata")) {
        if (is_kernel){
          print_error();
          fprintf(stderr, ".rodata can only be used in user mode\n");
          return false;
        }
        current_section = RODATA_SECTION;
        pc = section_offsets[current_section];
        continue;
      }
      else if (consume_keyword(".data")) {
        if (is_kernel){
          print_error();
          fprintf(stderr, ".data can only be used in user mode\n");
          return false;
        }
        current_section = DATA_SECTION;
        pc = section_offsets[current_section];
        continue;
      }
      else if (consume_keyword(".bss")) {
        if (is_kernel){
          print_error();
          fprintf(stderr, ".bss can only be used in user mode\n");
          return false;
        }
        current_section = BSS_SECTION;
        pc = section_offsets[current_section];
        continue;
      }
      else if (consume_keyword(".fill")) {
        enum ConsumeResult result; 
        consume_define_or_literal_or_label_abs(&result, ".fill");
        if (result != FOUND){
          if (result == NOT_FOUND){
            print_error();
            fprintf(stderr, "Invalid .fill immediate; expected integer literal, label, or .define constant\n");
          }
          return false;
        }
        if (is_kernel){
          pc += kWordBytes;
        } else {
          if (!ensure_valid_section(".fill")) return false;
          if (current_section == BSS_SECTION){
            print_error();
            fprintf(stderr, ".fill not allowed in .bss section\n");
            return false;
          }
          section_offsets[current_section] += kWordBytes;
          pc = section_offsets[current_section];
        }
        continue;
      }
      else if (consume_keyword(".fild")) {
        enum ConsumeResult result; 
        consume_define_or_literal(&result, ".fild");
        if (result != FOUND){
          if (result == NOT_FOUND){
            print_error();
            fprintf(stderr, "Invalid .fild immediate; expected integer literal or .define constant\n");
          }
          return false;
        }
        if (is_kernel){
          pc += kHalfBytes;
        } else {
          if (!ensure_valid_section(".fild")) return false;
          if (current_section == BSS_SECTION){
            print_error();
            fprintf(stderr, ".fild not allowed in .bss section\n");
            return false;
          }
          section_offsets[current_section] += kHalfBytes;
          pc = section_offsets[current_section];
        }
        continue;
      }
      else if (consume_keyword(".filb")) {
        enum ConsumeResult result; 
        consume_define_or_literal(&result, ".filb");
        if (result != FOUND){
          if (result == NOT_FOUND){
            print_error();
            fprintf(stderr, "Invalid .filb immediate; expected integer literal or .define constant\n");
          }
          return false;
        }
        if (is_kernel){
          pc += kByteBytes;
        } else {
          if (!ensure_valid_section(".filb")) return false;
          if (current_section == BSS_SECTION){
            print_error();
            fprintf(stderr, ".filb not allowed in .bss section\n");
            return false;
          }
          section_offsets[current_section] += kByteBytes;
          pc = section_offsets[current_section];
        }
        continue;
      }
      else if (consume_keyword(".space")) { 
        enum ConsumeResult result; 
        long imm = consume_define_or_literal(&result, ".space");
        if (result != FOUND){
          if (result == NOT_FOUND){
            print_error();
            fprintf(stderr, "Invalid .space count; expected integer literal or .define constant\n");
          }
          return false;
        }
        if (is_kernel){
          pc += imm;
        } else {
          if (!ensure_valid_section(".space")) return false;
          section_offsets[current_section] += imm;
          pc = section_offsets[current_section];
        }
        continue;
      }
      else if (consume_keyword(".align")) {
        enum ConsumeResult result;
        uint32_t alignment = 0;
        if (!parse_alignment(&result, ".align", &alignment)) return false;
        if (is_kernel){
          pc = align_up((uint32_t)pc, alignment);
        } else {
          if (!ensure_valid_section(".align")) return false;
          section_offsets[current_section] =
            align_up(section_offsets[current_section], alignment);
          pc = section_offsets[current_section];
        }
        continue;
      }
      else if (consume_keyword(".define")) {
        bool success = true;
        record_define(&success);
        if (!success) return false;
        continue;
      }
      else if (consume_keyword(".line")) {
        // handled in second pass
        skip_line();
        continue;
      }
      else if (consume_keyword(".local")) {
        // handled in second pass
        skip_line();
        continue;
      }
      
      enum ConsumeResult result = FOUND;
      if (!is_kernel){
        if (!ensure_valid_section("instruction")) return false;
        if (current_section == BSS_SECTION){
          print_error();
          fprintf(stderr, "Instructions not allowed in .bss section\n");
          return false;
        }
        if (section_offsets[current_section] % kWordBytes != 0){
          return report_instruction_alignment_error(section_offsets[current_section], "section offset");
        }
      } else if (pc % kWordBytes != 0) {
        return report_instruction_alignment_error((uint32_t)pc, "pc");
      }
      consume_instruction(&result);
      if (result == ERROR) return false;
      if (result == NOT_FOUND) {
        print_error();
        fprintf(stderr, "Unrecognized instruction\n");
        return false;
      }
      if (is_kernel){
        pc += kWordBytes;
      } else {
        section_offsets[current_section] += kWordBytes;
        pc = section_offsets[current_section];
      }
    }
  }
  return true;
}

// Purpose: Second pass to emit instruction/data bytes into output sections.
// Inputs: prog is the preprocessed source buffer; instructions is the output list.
// Outputs: Returns true on success; appends words to instruction arrays and updates bss_size.
// Invariants/Assumptions: section_bases are computed; section_offsets track byte offsets.
bool to_binary(char const* const prog, struct InstructionArrayList* instructions){
  current = prog;
  current_buffer_start = prog - 1;
  line_count = 1;

  enum ConsumeResult success = FOUND;

  while (success == FOUND){
    // consume any labels, they were already dealt with
    while (skip_newline(), skip_label(instructions));
    skip_newline();

    if (pc > ((long)1 << 32)){
      print_error();
      fprintf(stderr, "Program does not fit in 32-bit address space\n");
      return false;
    }

    // directives
    if (consume_keyword(".global")) {
      // handled in first pass
      struct Slice* name = consume_identifier();
      if (name == NULL){
        print_error();
        fprintf(stderr, ".global directive requires a label\n");
        return false;
      }
      if (!label_has_definition(global_labels, name)){
        print_error();
        fprintf(stderr, "Global label \"");
        print_slice_err(name);
        fprintf(stderr, "\" missing from first pass\n");
        free(name);
        return false;
      }
      free(name);
    }
    else if (consume_keyword(".define")){
      skip_line();
    } // handled in first pass
    else if (consume_keyword(".origin")) { 
      if (is_kernel){
        // create a new section
        enum ConsumeResult result; 
        long imm = consume_literal(&result);
        if (result != FOUND){
          print_error();
          fprintf(stderr, "\n");
          return false;
        }
        if (imm < pc){
          print_error();
          fprintf(stderr, ".origin cannot be used to go backwards\n");
          return false;
        } else if (imm >= ((long)1 << 32)){
          print_error();
          fprintf(stderr, ".origin address must be a 32 bit integer\n");
          return false;
        } else {
          struct InstructionArray* arr = create_instruction_array(10, imm);
          instruction_array_list_append(instructions, arr);
        }
        pc = imm;
      } else {
        print_error();
        fprintf(stderr, ".origin can only be used in kernel mode\n");
        return false;
      }
    }
    else if (consume_keyword(".text")) {
      if (is_kernel){
        print_error();
        fprintf(stderr, ".text can only be used in user mode\n");
        return false;
      }
      current_section = TEXT_SECTION;
      pc = section_bases[current_section] + section_offsets[current_section];
    }
    else if (consume_keyword(".rodata")) {
      if (is_kernel){
        print_error();
        fprintf(stderr, ".rodata can only be used in user mode\n");
        return false;
      }
      current_section = RODATA_SECTION;
      pc = section_bases[current_section] + section_offsets[current_section];
    }
    else if (consume_keyword(".data")) {
      if (is_kernel){
        print_error();
        fprintf(stderr, ".data can only be used in user mode\n");
        return false;
      }
      current_section = DATA_SECTION;
      pc = section_bases[current_section] + section_offsets[current_section];
    }
    else if (consume_keyword(".bss")) {
      if (is_kernel){
        print_error();
        fprintf(stderr, ".bss can only be used in user mode\n");
        return false;
      }
      current_section = BSS_SECTION;
      pc = section_bases[current_section] + section_offsets[current_section];
    }
    else if (consume_keyword(".fill")) {
      enum ConsumeResult result; 
      long imm = consume_define_or_literal_or_label_abs(&result, ".fill");
      if (result != FOUND){
        if (result == NOT_FOUND){
          print_error();
          fprintf(stderr, "Invalid .fill immediate; expected integer literal, label, or .define constant\n");
        }
        return false;
      }
      if (imm >= -((long)1 << 31) && imm < ((long)1 << 32)){
        uint32_t value = (uint32_t)imm;
        uint8_t bytes[kWordBytes];
        encode_value_bytes(value, bytes, kWordBytes);
        if (is_kernel){
          append_bytes_kernel(instructions->tail, bytes, kWordBytes);
        } else {
          if (!ensure_valid_section(".fill")) return false;
          if (current_section == TEXT_SECTION){
            print_warning(".fill used in .text section");
          }
          switch (current_section) {
            case TEXT_SECTION:
              append_bytes_user(text_instruction_array, bytes, kWordBytes, current_section);
              break;
            case RODATA_SECTION:
              append_bytes_user(rodata_instruction_array, bytes, kWordBytes, current_section);
              break;
            case DATA_SECTION:
              append_bytes_user(data_instruction_array, bytes, kWordBytes, current_section);
              break;
            case BSS_SECTION:
              print_error();
              fprintf(stderr, ".fill not allowed in .bss section\n");
              return false;
            default:
              print_error();
              fprintf(stderr, "cannot use .fill before specifying a section\n");
              return false;
          }
        }
      } else {
        print_error();
        fprintf(stderr, ".fill immediate must fit in a 32-bit value\n");
        return false;
      }
    }
    else if (consume_keyword(".fild")) {
      enum ConsumeResult result; 
      long imm = consume_define_or_literal(&result, ".fild");
      if (result != FOUND){
        if (result == NOT_FOUND){
          print_error();
          fprintf(stderr, "Invalid .fild immediate; expected integer literal or .define constant\n");
        }
        return false;
      }
      if (imm >= -((long)1 << 15) && imm < ((long)1 << 16)){
        uint16_t value = (uint16_t)imm;
        if (is_kernel){
          uint8_t bytes[kHalfBytes];
          encode_value_bytes(value, bytes, kHalfBytes);
          append_bytes_kernel(instructions->tail, bytes, kHalfBytes);
        } else {
          if (!ensure_valid_section(".fild")) return false;
          if (current_section == TEXT_SECTION){
            print_warning(".fild used in .text section");
          }
          switch (current_section) {
            case TEXT_SECTION:
              {
                uint8_t bytes[kHalfBytes];
                encode_value_bytes(value, bytes, kHalfBytes);
                append_bytes_user(text_instruction_array, bytes, kHalfBytes, current_section);
              }
              break;
            case RODATA_SECTION:
              {
                uint8_t bytes[kHalfBytes];
                encode_value_bytes(value, bytes, kHalfBytes);
                append_bytes_user(rodata_instruction_array, bytes, kHalfBytes, current_section);
              }
              break;
            case DATA_SECTION:
              {
                uint8_t bytes[kHalfBytes];
                encode_value_bytes(value, bytes, kHalfBytes);
                append_bytes_user(data_instruction_array, bytes, kHalfBytes, current_section);
              }
              break;
            case BSS_SECTION:
              print_error();
              fprintf(stderr, ".fild not allowed in .bss section\n");
              return false;
            default:
              print_error();
              fprintf(stderr, "cannot use .fild before specifying a section\n");
              return false;
          }
        }
      } else {
        print_error();
        fprintf(stderr, ".fild immediate must fit in a 16-bit value\n");
        return false;
      }
    }
    else if (consume_keyword(".filb")) {
      enum ConsumeResult result; 
      long imm = consume_define_or_literal(&result, ".filb");
      if (result != FOUND){
        if (result == NOT_FOUND){
          print_error();
          fprintf(stderr, "Invalid .filb immediate; expected integer literal or .define constant\n");
        }
        return false;
      }
      if (imm >= -((long)1 << 7) && imm < ((long)1 << 8)){
        uint8_t value = (uint8_t)imm;
        if (is_kernel){
          append_bytes_kernel(instructions->tail, &value, kByteBytes);
        } else {
          if (!ensure_valid_section(".filb")) return false;
          if (current_section == TEXT_SECTION){
            print_warning(".filb used in .text section");
          }
          switch (current_section) {
            case TEXT_SECTION:
              append_bytes_user(text_instruction_array, &value, kByteBytes, current_section);
              break;
            case RODATA_SECTION:
              append_bytes_user(rodata_instruction_array, &value, kByteBytes, current_section);
              break;
            case DATA_SECTION:
              append_bytes_user(data_instruction_array, &value, kByteBytes, current_section);
              break;
            case BSS_SECTION:
              print_error();
              fprintf(stderr, ".filb not allowed in .bss section\n");
              return false;
            default:
              print_error();
              fprintf(stderr, "cannot use .filb before specifying a section\n");
              return false;
          }
        }
      } else {
        print_error();
        fprintf(stderr, ".filb immediate must fit in an 8-bit value\n");
        return false;
      }
    }
    else if (consume_keyword(".space")) { 
      enum ConsumeResult result; 
      long imm = consume_define_or_literal(&result, ".space");
      if (result != FOUND){
        if (result == NOT_FOUND){
          print_error();
          fprintf(stderr, "Invalid .space count; expected integer literal or .define constant\n");
        }
        return false;
      }
      if (0 <= imm && imm < ((long)1 << 32)){
        if (is_kernel){
          append_zero_bytes_kernel(instructions->tail, (uint32_t)imm);
        } else {
          if (!ensure_valid_section(".space")) return false;
          switch (current_section) {
            case TEXT_SECTION:
              append_zero_bytes_user(text_instruction_array, (uint32_t)imm, current_section);
              break;
            case RODATA_SECTION:
              append_zero_bytes_user(rodata_instruction_array, (uint32_t)imm, current_section);
              break;
            case DATA_SECTION:
              append_zero_bytes_user(data_instruction_array, (uint32_t)imm, current_section);
              break;
            case BSS_SECTION:
              bss_size += (uint32_t)imm;
              section_offsets[current_section] += (uint32_t)imm;
              pc = section_bases[current_section] + section_offsets[current_section];
              break;
            default:
              print_error();
              fprintf(stderr, "cannot use .space before specifying a section\n");
              return false;
          }
        }
      } else {
        print_error();
        fprintf(stderr, ".space immediate must be a positive 32 bit integer\n");
        return false;
      }
    }
    else if (consume_keyword(".line")) {
      // Parse filename and line number; record the address of the next instruction.
      struct Slice* filename = consume_filename();
      if (filename == NULL){
        print_error();
        fprintf(stderr, ".line directive requires a filename\n");
        return false;
      }
      enum ConsumeResult result;
      long line_num = consume_literal(&result);
      if (result != FOUND){
        print_error();
        fprintf(stderr, ".line directive requires a line number\n");
        free(filename);
        return false;
      }
      add_debug_line(debug_info_list, filename, line_num, (uint32_t)pc);
    }
    else if (consume_keyword(".local")) {
      // Parse name and bp offset; record the address where locals become visible.
      struct Slice* varname = consume_identifier();
      if (varname == NULL){
        print_error();
        fprintf(stderr, ".local directive requires a variable name\n");
        return false;
      }
      enum ConsumeResult result;
      long bp_offset = consume_literal(&result);
      if (result != FOUND){
        print_error();
        fprintf(stderr, ".local directive requires a bp offset\n");
        free(varname);
        return false;
      }
      long size_value = consume_literal(&result);
      if (result != FOUND){
        print_error();
        fprintf(stderr, ".local directive requires a size in bytes\n");
        free(varname);
        return false;
      }
      if (size_value <= 0 || size_value > UINT32_MAX) {
        print_error();
        fprintf(stderr, ".local directive size must be a positive 32-bit value\n");
        free(varname);
        return false;
      }
      add_debug_local(debug_info_list, varname, bp_offset, (size_t)size_value, (uint32_t)pc);
    }
    else if (consume_keyword(".align")) {
      enum ConsumeResult result;
      uint32_t alignment = 0;
      if (!parse_alignment(&result, ".align", &alignment)) return false;

      if (is_kernel){
        uint32_t current = (uint32_t)pc;
        uint32_t aligned = align_up(current, alignment);
        uint32_t pad = aligned - current;
        append_zero_bytes_kernel(instructions->tail, pad);
      } else {
        if (!ensure_valid_section(".align")) return false;
        uint32_t current = section_offsets[current_section];
        uint32_t aligned = align_up(current, alignment);
        uint32_t pad = aligned - current;
        switch (current_section) {
          case TEXT_SECTION:
            append_zero_bytes_user(text_instruction_array, pad, current_section);
            break;
          case RODATA_SECTION:
            append_zero_bytes_user(rodata_instruction_array, pad, current_section);
            break;
          case DATA_SECTION:
            append_zero_bytes_user(data_instruction_array, pad, current_section);
            break;
          case BSS_SECTION:
            bss_size += pad;
            section_offsets[current_section] += pad;
            pc = section_bases[current_section] + section_offsets[current_section];
            break;
          default:
            print_error();
            fprintf(stderr, "cannot use .align before specifying a section\n");
            return false;
        }
      }
      continue;
    } else {
      if (!is_kernel){
        if (!ensure_valid_section("instruction")) return false;
        if (current_section == BSS_SECTION){
          print_error();
          fprintf(stderr, "Instructions not allowed in .bss section\n");
          return false;
        }
        pc = section_bases[current_section] + section_offsets[current_section];
        if (section_offsets[current_section] % kWordBytes != 0){
          return report_instruction_alignment_error((uint32_t)pc, "pc");
        }
      } else if (pc % kWordBytes != 0) {
        return report_instruction_alignment_error((uint32_t)pc, "pc");
      }
      int instruction = consume_instruction(&success);
      if (success == FOUND) {
        if (is_kernel){
          instruction_array_append(instructions->tail, instruction);
          pc += kWordBytes;
        } else {
          if (current_section == RODATA_SECTION){
            print_warning("Instruction emitted in .rodata section");
          } else if (current_section == DATA_SECTION){
            print_warning("Instruction emitted in .data section");
          }
          switch (current_section) {
            case TEXT_SECTION:
              instruction_array_append(text_instruction_array, instruction);
              break;
            case RODATA_SECTION:
              instruction_array_append(rodata_instruction_array, instruction);
              break;
            case DATA_SECTION:
              instruction_array_append(data_instruction_array, instruction);
              break;
            default:
              break;
          }
          section_offsets[current_section] += kWordBytes;
          pc = section_bases[current_section] + section_offsets[current_section];
        }
      }
      else if (success == ERROR) return false;
    }
  }

  if (!is_at_end()) {
    print_error();
    fprintf(stderr, "Unrecognized instruction\n");
    return false;
  }

  return true;
}

static void append_labels_from_map(struct HashMap* map, struct LabelList* labels, uint32_t offset){
  for (size_t i = 0; i < map->size; ++i){
    struct HashEntry* entry = map->arr[i];
    while (entry != NULL){
      if (entry->is_defined){
        uint32_t addr = (uint32_t)(entry->value + offset);
        label_list_append(labels, entry->key->start, entry->key->len, addr, entry->is_data);
      }
      entry = entry->next;
    }
  }
}

// assemble an entire program
struct ProgramDescriptor* assemble(int num_files, int* file_names, bool kernel,
  const char *const *const argv, char** files, struct LabelList** labels_out,
  struct DebugInfoList** labels_out_c){

  is_kernel = kernel;
  pass_number = 1;
  current_section = -1;
  text_instruction_array = NULL;
  rodata_instruction_array = NULL;
  data_instruction_array = NULL;
  bss_size = 0;
  reset_section_offsets();
  for (int i = 0; i < 4; ++i) section_sizes[i] = 0;

  debug_info_list = create_debug_info_list();

  if (labels_out != NULL) *labels_out = NULL;
  if (labels_out_c != NULL) {
    *labels_out_c = debug_info_list;
  }

  current_file_index = 0;

  local_labels = malloc(num_files * sizeof(struct HashMap*));
  local_defines = malloc(num_files * sizeof(struct HashMap*));
  local_globals = malloc(num_files * sizeof(struct HashMap*));

  // make a hashmap of labels for each file + one global hashmap for global labels
  global_labels = create_hash_map(1000);
  pc = 0;
  for (int i = 0; i < num_files; ++i){
    current_file_index = i;
    current_file = argv[file_names[i]];
    if (!process_labels(files[i] + 1)) {
      for (int j = 0; j <= i; ++j) destroy_hash_map(local_labels[j]);
      for (int j = 0; j <= i; ++j) destroy_hash_map(local_defines[j]);
      for (int j = 0; j <= i; ++j) destroy_hash_map(local_globals[j]);
      free(local_labels);
      free(local_defines);
      free(local_globals);
      destroy_hash_map(global_labels);
      return NULL;
    }
  }

  if (!is_kernel){
    section_sizes[TEXT_SECTION] = align_up(section_offsets[TEXT_SECTION], kWordBytes);
    section_sizes[RODATA_SECTION] = align_up(section_offsets[RODATA_SECTION], kWordBytes);
    section_sizes[DATA_SECTION] = align_up(section_offsets[DATA_SECTION], kWordBytes);
    section_sizes[BSS_SECTION] = section_offsets[BSS_SECTION];
    compute_section_bases();
    for (int i = 0; i < num_files; ++i) adjust_label_map_for_sections(local_labels[i]);
    adjust_label_map_for_sections(global_labels);

    struct Slice start_label = {"_start", 6};
    if (!label_has_definition(global_labels, &start_label)){
      fprintf(stderr, "Missing global label _start\n");
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_labels[j]);
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_defines[j]);
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_globals[j]);
      free(local_labels);
      free(local_defines);
      free(local_globals);
      destroy_hash_map(global_labels);
      return NULL;
    }
    entry_point = (uint32_t)hash_map_get(global_labels, &start_label);
  }

  pass_number = 2;

  struct InstructionArrayList* instructions = create_instruction_array_list();

  if (!is_kernel){
    text_instruction_array = instructions->head;
    text_instruction_array->origin = section_bases[TEXT_SECTION];
    rodata_instruction_array = create_instruction_array(10, section_bases[RODATA_SECTION]);
    data_instruction_array = create_instruction_array(10, section_bases[DATA_SECTION]);
    instruction_array_list_append(instructions, rodata_instruction_array);
    instruction_array_list_append(instructions, data_instruction_array);
  }

  reset_section_offsets();
  current_section = -1;
  bss_size = 0;
  pc = is_kernel ? 0 : section_bases[TEXT_SECTION];
  for (int i = 0; i < num_files; ++i){
    current_file_index = i;
    current_file = argv[file_names[i]];
    if (!to_binary(files[i] + 1, instructions)){
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_labels[j]);
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_defines[j]);
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_globals[j]);
      free(local_labels);
      free(local_defines);
      free(local_globals);
      destroy_hash_map(global_labels);
      destroy_instruction_array_list(instructions);
      return NULL;
    }
  }

  if (labels_out != NULL){
    struct LabelList* labels = create_label_list(128);
    uint32_t offset = 0;
    for (int j = 0; j < num_files; ++j) {
      append_labels_from_map(local_labels[j], labels, offset);
    }
    *labels_out = labels;
  }

  for (int j = 0; j < num_files; ++j) destroy_hash_map(local_labels[j]);
  for (int j = 0; j < num_files; ++j) destroy_hash_map(local_defines[j]);
  for (int j = 0; j < num_files; ++j) destroy_hash_map(local_globals[j]);
  free(local_labels);
  free(local_defines);
  free(local_globals);
  destroy_hash_map(global_labels);

  struct ProgramDescriptor* program = malloc(sizeof(struct ProgramDescriptor));
  program->entry_point = entry_point;
  program->sections = instructions;
  program->bss_size = bss_size;

  return program;
}
