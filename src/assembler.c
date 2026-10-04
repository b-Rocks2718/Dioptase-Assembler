#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
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
#include "keyword.h"
#include "charclass.h"

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

enum UserSection current_section = NO_SECTION;
struct InstructionArray* text_instruction_array = NULL;
struct InstructionArray* rodata_instruction_array = NULL;
struct InstructionArray* data_instruction_array = NULL;
static struct InstructionArray* section_arrays[SECTION_COUNT];
unsigned bss_size = 0;
static uint32_t section_offsets[SECTION_COUNT];
static uint32_t section_sizes[SECTION_COUNT];
static uint32_t section_bases[SECTION_COUNT];
static uint32_t section_load_bases[SECTION_COUNT];
static bool section_load_set[SECTION_COUNT];

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
static const uint32_t kKernelSectionAlign = 512;

// Reset section offsets.
static void reset_section_offsets(void) {
  for (int i = 0; i < SECTION_COUNT; ++i) section_offsets[i] = 0;
}

// Reset section load bases.
static void reset_section_load_bases(void) {
  for (int i = 0; i < SECTION_COUNT; ++i) {
    section_load_bases[i] = 0;
    section_load_set[i] = false;
  }
}

// Round a value up to the requested alignment.
static uint32_t align_up(uint32_t value, uint32_t align) {
  uint32_t rem = value % align;
  if (rem == 0) return value;
  return value + (align - rem);
}

// Return the load base of the section used by PC-relative calculations.
// Returns the runtime base for the section.
static uint32_t section_pc_base(enum UserSection section){
  return section_load_bases[section];
}

// Check whether a section index is valid for the active mode.
// Returns true when section is usable in the current mode.
static bool is_section_in_range(enum UserSection section){
  if (is_kernel){
    return section >= TEXT_SECTION && section <= IMPLICIT_SECTION;
  }
  return section >= TEXT_SECTION && section <= BSS_SECTION;
}

// Forward declaration for alignment parsing helpers.
static long consume_define_or_literal(enum ConsumeResult* result, const char* context);

// Check whether a value is a power-of-two alignment.
// Returns true when value is a nonzero power of two.
static bool is_power_of_two_u32(uint32_t value){
  return value != 0 && (value & (value - 1)) == 0;
}

// Parse and validate a byte alignment value for .align.
// Returns true on success and fills alignment_out.
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

// Parse a kernel section load-base directive such as .text_load.
// Returns true on success; updates section_load_bases during pass 1.
static bool parse_section_load_directive(enum UserSection section, const char* directive){
  if (!is_kernel){
    print_error();
    fprintf(stderr, "%s can only be used in kernel mode\n", directive);
    return false;
  }
  enum ConsumeResult result;
  long imm = consume_define_or_literal(&result, directive);
  if (result != FOUND){
    if (result == NOT_FOUND){
      print_error();
      fprintf(stderr, "Invalid %s value; expected integer literal or .define constant\n", directive);
    }
    return false;
  }
  if (imm < 0 || imm >= ((long)1 << 32)){
    print_error();
    fprintf(stderr, "%s address must be a 32-bit unsigned integer\n", directive);
    return false;
  }
  uint32_t addr = (uint32_t)imm;
  if ((addr % kWordBytes) != 0){
    print_error();
    fprintf(stderr, "%s address must be %u-byte aligned\n", directive, kWordBytes);
    return false;
  }

  if (pass_number == 1){
    if (section_offsets[section] != 0){
      print_error();
      fprintf(stderr, "%s must appear before any content in that section\n", directive);
      return false;
    }
    if (section_load_set[section] && section_load_bases[section] != addr){
      print_error();
      fprintf(stderr, "%s specified multiple times with different values\n", directive);
      return false;
    }
    section_load_bases[section] = addr;
    section_load_set[section] = true;
  } else {
    if (section_load_set[section] && section_load_bases[section] != addr){
      print_error();
      fprintf(stderr, "%s value does not match first pass\n", directive);
      return false;
    }
  }
  return true;
}

// Encode the least-significant bytes of value in little-endian order.
// value is the integer to encode; out must have space for count bytes; count is 1, 2, or 4.
static void encode_value_bytes(uint32_t value, uint8_t* out, uint32_t count){
  for (uint32_t i = 0; i < count; ++i){
    out[i] = (uint8_t)(value >> (8 * i));
  }
}

// Word slots needed to hold a section measured in bytes during pass 1.
static size_t section_word_capacity(enum UserSection section) {
  size_t words = ((size_t)section_sizes[section] + (kWordBytes - 1)) / kWordBytes;
  if (words == 0) words = 1;
  return words;
}

// Grow an instruction array to at least `words` slots. Existing words are kept.
static bool reserve_instruction_words(struct InstructionArray* arr, size_t words) {
  if (words < 1) words = 1;
  if (words <= arr->capacity) return true;
  int* grown = realloc(arr->instructions, words * sizeof(int));
  if (grown == NULL) {
    fprintf(stderr, "Assembler: failed to reserve %zu instruction words\n", words);
    return false;
  }
  arr->instructions = grown;
  arr->capacity = words;
  return true;
}

// Append raw bytes into a section array and advance offsets.
// section_bases are initialized and aligned.
static void append_bytes_user(struct InstructionArray* arr, const uint8_t* bytes, uint32_t count,
                              enum UserSection section){
  for (uint32_t i = 0; i < count; ++i){
    uint32_t abs_pc = section_bases[section] + section_offsets[section];
    instruction_array_append_byte(arr, bytes[i], (int)abs_pc);
    section_offsets[section] += kByteBytes;
  }
  pc = section_pc_base(section) + section_offsets[section];
}

// Append zero bytes into a section array and advance offsets.
// section_bases are initialized and aligned.
static void append_zero_bytes_user(struct InstructionArray* arr, uint32_t count, enum UserSection section){
  for (uint32_t i = 0; i < count; ++i){
    uint32_t abs_pc = section_bases[section] + section_offsets[section];
    instruction_array_append_byte(arr, 0, (int)abs_pc);
    section_offsets[section] += kByteBytes;
  }
  pc = section_pc_base(section) + section_offsets[section];
}

// Report misaligned instruction addresses with context.
// Returns false after emitting an error.
static bool report_instruction_alignment_error(uint32_t address, const char* label){
  print_error();
  fprintf(stderr, "Instruction address must be %u-byte aligned; %s is 0x%08X\n",
          kWordBytes, label, address);
  return false;
}

// Compute the user-section load addresses from their emitted sizes.
static void compute_section_bases(void) {
  section_bases[TEXT_SECTION] = USER_BASE_ADDR;
  section_bases[RODATA_SECTION] = align_up(section_bases[TEXT_SECTION] + section_sizes[TEXT_SECTION], SECTION_ALIGN);
  section_bases[DATA_SECTION] = align_up(section_bases[RODATA_SECTION] + section_sizes[RODATA_SECTION], SECTION_ALIGN);
  section_bases[BSS_SECTION] = section_bases[DATA_SECTION] + section_sizes[DATA_SECTION];
}

// Compute kernel section bases with 512-byte padding between sections.
static void compute_kernel_section_bases(void){
  uint32_t cursor = 0;
  section_bases[IMPLICIT_SECTION] = cursor;
  cursor += align_up(section_sizes[IMPLICIT_SECTION], kKernelSectionAlign);

  section_bases[TEXT_SECTION] = cursor;
  cursor += align_up(section_sizes[TEXT_SECTION], kKernelSectionAlign);

  section_bases[RODATA_SECTION] = cursor;
  cursor += align_up(section_sizes[RODATA_SECTION], kKernelSectionAlign);

  section_bases[DATA_SECTION] = cursor;
  cursor += align_up(section_sizes[DATA_SECTION], kKernelSectionAlign);

  section_bases[BSS_SECTION] = cursor;
  cursor += align_up(section_sizes[BSS_SECTION], kKernelSectionAlign);

  section_bases[END_SECTION] = cursor;
}

// Finalize runtime section bases after sizes are known.
static void finalize_section_load_bases(void){
  for (int i = 0; i < SECTION_COUNT; ++i){
    if (!section_load_set[i]) section_load_bases[i] = section_bases[i];
  }
  if (is_kernel && section_load_set[BSS_SECTION]){
    uint32_t bss_padded = align_up(section_sizes[BSS_SECTION], kKernelSectionAlign);
    section_load_bases[END_SECTION] = section_load_bases[BSS_SECTION] + bss_padded;
  }
}

// Pack a section index and offset for resolution after layout.
static uint64_t encode_section_offset(enum UserSection section, uint32_t offset) {
  // Pack section + offset for pass 1; resolved to absolute addresses after layout.
  return ((uint64_t)section << 32) | offset;
}

// Replace packed section offsets in a label map with absolute addresses.
static void adjust_label_map_for_sections(struct HashMap* map) {
  // Convert packed section offsets into absolute addresses once section sizes are known.
  for (size_t i = 0; i < map->size; ++i){
    struct HashEntry* entry = map->arr[i];
    while (entry != NULL){
      if (entry->is_defined){
        uint64_t raw = (uint64_t)entry->value;
        enum UserSection section = (enum UserSection)(raw >> 32);
        uint32_t offset = (uint32_t)(raw & 0xFFFFFFFFu);
        entry->value = (long)(section_load_bases[section] + offset);
      }
      entry = entry->next;
    }
  }
}

// Check that the current directive is being emitted in an allowed section.
static bool ensure_valid_section(const char* context) {
  if (!is_section_in_range(current_section)) {
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
  return true;
}

// Return whether a source span is a valid .define name.
static bool is_valid_define_name(const char* start, size_t len) {
  if (len == 0) return false;
  if (!is_identifier_start(start[0])) return false;
  for (size_t i = 1; i < len; ++i){
    if (!is_identifier_char(start[i])) return false;
  }
  return true;
}

// Set the command-line definitions used while preprocessing each input.
void set_cli_defines(int count, const char* const* defines){
  cli_define_count = count;
  cli_defines = defines;
}

// Insert command-line definitions into the current definition map.
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

    hash_map_insert(local_defines[current_file_index], &name_view, value, true, true);
  }
  return true;
}

// Consume name when it denotes a complete register token.
static bool consume_named_register(const char* name) {
  size_t len = strlen(name);
  if (strncmp(current, name, len) == 0 && !is_identifier_char(current[len])) {
    current += len;
    return true;
  }
  return false;
}

// Print "<kind> in <file>\nline N: "<source line>"" for the line containing
// `current`. The line is trimmed of surrounding whitespace; a blank line prints
// as "". The walk back stops at current_buffer_start or the NUL sentinel that
// precedes every preprocessed buffer.
static void print_source_context(const char* kind) {
  fprintf(stderr, "%s in %s\nline %u: \"", kind, current_file, line_count);

  char const * buffer_start = current_buffer_start != NULL ? current_buffer_start : current;
  char const * start = current;
  while (start > buffer_start && *(start - 1) != '\0' && *(start - 1) != '\n') start--;
  char const * end = current;
  while (*end != '\0' && *end != '\n') end++;

  // Trim inside [start, end) only, so a whitespace-only line cannot make the
  // span run into the neighbouring lines.
  while (start < end && ascii_isspace((unsigned char)*start)) start++;
  while (end > start && ascii_isspace((unsigned char)*(end - 1))) end--;

  struct Slice line = {start, (size_t)(end - start)};
  print_slice_err(&line);
  fprintf(stderr, "\"\n");
}

// print line causing an error
// Only the first error prints its source context; later diagnostics from the
// same failure (e.g. "Preprocesser macro error") are printed bare.
void print_error(void) {
  static bool has_printed = false;
  if (has_printed) return;
  print_source_context("Error");
  has_printed = true;
}

// Print a warning at the current source location.
static void print_warning(const char* message) {
  print_source_context("Warning");
  fprintf(stderr, "%s\n", message);
}

// is the rest of the file just whitespace?
bool is_at_end(void) {
  while (ascii_isspace((unsigned char)*current)) {
    if (*current == '\n') line_count++;
    current += 1;
  }
  if (*current != 0) return false;
  else return true;
}

// skip whitespace and commas until end of line or non-whitespace character
void skip(void) {
  while ((ascii_isspace((unsigned char)*current) && *current != '\n') || *current == ',' || *current == ';') {
    current++;
  }
}

// skip until we get to a new nonempty line
void skip_newline(void) {
  while (ascii_isspace((unsigned char)*current)) {
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
// differs from consume because we ensure token boundaries on both sides
bool consume_keyword(const char* str) {
  // Whitespace is intentionally left to the caller because preprocessing also
  // uses this matcher.
  if (current != current_buffer_start &&
      is_identifier_char(current[-1])) {
    return false;
  }

  size_t i = 0;
  while (true) {
    char const expected = str[i];
    char const found = current[i];
    if (expected == 0) {
      /* survived to the end of the expected string */
      if (ascii_isspace(found) || found == '\0' ||
          found == ',' || found == ';' || found == ':') {
        // word break
        current += i;
        return true;
      } else {
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
// On success *out views the identifier in the source buffer.
bool consume_identifier(struct Slice* out) {
  skip();
  if (!is_identifier_start(current[0])) return false;
  // identifiers begin with a letter or underscore, then letters, digits,
  // underscores, and periods
  size_t i = 1;
  while (is_identifier_char(current[i])) i++;
  out->start = current;
  out->len = i;
  current += i;
  return true;
}

// Return whether c ends an unquoted .line filename.
static bool ends_filename(char c) {
  return c == '\0' || ascii_isspace((unsigned char)c) || c == ',' || c == ';';
}

// attempt to consume a filename, has no effect if a match is not found
// Debug file names may be relative or absolute and are emitted without
// quotes. Consume until the next whitespace or statement separator.
static bool consume_filename(struct Slice* out) {
  skip();
  if (ends_filename(current[0])) return false;
  size_t i = 1;
  while (!ends_filename(current[i])) i++;
  out->start = current;
  out->len = i;
  current += i;
  return true;
}

// Consume an identifier followed by ':' into *out. On failure nothing is
// consumed beyond leading separators.
bool consume_label(struct Slice* out){
  skip();
  char const * old_current = current;
  if (consume_identifier(out) && consume(":")) return true;

  // undo side effects
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
  else if (current[0] == 'r' && ascii_isdigit((unsigned char)current[1])) {
    int v = 0;
    size_t i = 1;
    while(ascii_isdigit((unsigned char)current[i])) {
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
  if (current[0] == 'c' && current[1] == 'r' && ascii_isdigit((unsigned char)current[2])) {
    int v = 0;
    size_t i = 2;
    while(ascii_isdigit((unsigned char)current[i])) {
      // then followed by numbers
      v = 10 * v + current[i] - '0';
      i += 1;
    }

    if (v > 12 || is_identifier_char(current[i])) return -1;
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
    else if (consume_named_register("tlba")) return 7;
    else if (consume_named_register("ksp")) return 8;
    else if (consume_named_register("cid")) return 9;
    else if (consume_named_register("mbi")) return 10;
    else if (consume_named_register("mbo")) return 11;
    else if (consume_named_register("tlbf")) return 12;
    else return -1;
  }
}

// Return the value of c as a hexadecimal digit, or -1.
static int digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Radix prefixes accepted after a leading 0 (either letter case).
static const struct {
  char prefix;
  int radix;
  const char* name;       // used mid-sentence: "Invalid hex literal"
  const char* name_cap;   // used to start a sentence: "Hex literal requires..."
} kRadixPrefixes[] = {
  {'b', 2, "binary", "Binary"},
  {'o', 8, "octal", "Octal"},
  {'x', 16, "hex", "Hex"},
};

// attempt to consume an integer literal
// Accepts an optional '-', then decimal (no leading zero), a lone 0, or a
// 0b/0o/0x prefixed literal. Binary and octal literals run over decimal
// digits and hex over alphanumerics, so an out-of-range digit is an ERROR
// rather than the silent end of the literal.
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
  char next = current[1];
  if (*current == '0' &&
      (ascii_isspace((unsigned char)next) || next == '\0' || next == ']' || next == '#')){
    *result = FOUND;
    current++;
    return 0;
  }

  long v = 0;
  if (ascii_isdigit((unsigned char)*current) && *current != '0') {
    // decimal literal
    do {
      v = 10*v + ((*current) - '0');
      current += 1;
    } while (ascii_isdigit((unsigned char)*current));
    *result = FOUND;
    return negate ? -v : v;
  }

  if (*current == '0') {
    for (size_t p = 0; p < sizeof(kRadixPrefixes) / sizeof(kRadixPrefixes[0]); ++p) {
      int radix = kRadixPrefixes[p].radix;
      if ((next | 0x20) != kRadixPrefixes[p].prefix) continue;  // ASCII lower-case

      current += 2;
      bool saw_digit = false;
      while (radix == 16 ? ascii_isalnum((unsigned char)*current)
                         : ascii_isdigit((unsigned char)*current)) {
        int d = digit_value(*current);
        if (d < 0 || d >= radix) {
          print_error();
          fprintf(stderr, "Invalid %s literal\n", kRadixPrefixes[p].name);
          *result = ERROR;
          return 0;
        }
        saw_digit = true;
        v = radix * v + d;
        current += 1;
      }
      if (!saw_digit) {
        print_error();
        fprintf(stderr, "%s literal requires at least one digit\n", kRadixPrefixes[p].name_cap);
        *result = ERROR;
        return 0;
      }
      *result = FOUND;
      return negate ? -v : v;
    }
  }

  current = old_current;
  *result = NOT_FOUND;
  return 0;
}

// Parse a numeric literal or a .define constant (no labels allowed).
// Returns the literal or constant value when FOUND; returns 0 otherwise.
static long consume_define_or_literal(enum ConsumeResult* result, const char* context) {
  long imm = consume_literal(result);
  if (*result != NOT_FOUND) return imm;

  struct Slice name;
  if (!consume_identifier(&name)) {
    *result = NOT_FOUND;
    return 0;
  }

  if (hash_map_contains(local_defines[current_file_index], &name)) {
    imm = hash_map_get(local_defines[current_file_index], &name);
    *result = FOUND;
  } else {
    print_error();
    if (context != NULL) {
      fprintf(stderr, "%s constant \"", context);
    } else {
      fprintf(stderr, "Constant \"");
    }
    print_slice_err(&name);
    fprintf(stderr, "\" has not been defined\n");
    *result = ERROR;
  }

  return imm;
}

// Parse a numeric literal, .define constant, or label absolute address.
// Returns the literal, constant, or label address when FOUND; returns 0 otherwise.
static long consume_define_or_literal_or_label_abs(enum ConsumeResult* result,
                                                   const char* context) {
  long imm = consume_literal(result);
  if (*result != NOT_FOUND) {
    return imm;
  }

  struct Slice name;
  if (!consume_identifier(&name)) {
    *result = NOT_FOUND;
    return 0;
  }

  if (hash_map_contains(local_defines[current_file_index], &name)) {
    imm = hash_map_get(local_defines[current_file_index], &name);
    *result = FOUND;
    return imm;
  }

  // Allow labels in pass 1 without forcing a definition yet.
  if (pass_number == 1) {
    *result = FOUND;
    return 0;
  }

  if (label_has_definition(local_labels[current_file_index], &name)) {
    imm = hash_map_get(local_labels[current_file_index], &name);
    // Kernel labels are stored as offsets, so emit absolute addresses for .fill.
    *result = FOUND;
  } else if (label_has_definition(global_labels, &name)) {
    imm = hash_map_get(global_labels, &name);
    // Kernel labels are stored as offsets, so emit absolute addresses for .fill.
    *result = FOUND;
  } else {
    print_error();
    if (context != NULL) {
      fprintf(stderr, "%s constant/label \"", context);
    } else {
      fprintf(stderr, "Constant/label \"");
    }
    print_slice_err(&name);
    fprintf(stderr, "\" has not been defined\n");
    *result = ERROR;
  }

  return imm;
}

// Consume a label operand and resolve it to an immediate value.
long consume_label_imm(enum ConsumeResult* result){
  struct Slice label;
  if (!consume_identifier(&label)){
    *result = NOT_FOUND;
    return 0;
  }

  // don't try to decode labels on first pass
  if (pass_number == 1) {
    *result = FOUND;
    return 0;
  }

  long imm = 0;
  if (label_has_definition(local_labels[current_file_index], &label)){
    imm = hash_map_get(local_labels[current_file_index], &label) - pc - 4;

    // If this label is global in this file, the global entry should match.
    if (hash_map_contains(local_globals[current_file_index], &label) &&
        label_has_definition(global_labels, &label))
      assert(imm == hash_map_get(global_labels, &label) - pc - 4);

    *result = FOUND;
  } else if (label_has_definition(global_labels, &label)){
    imm = hash_map_get(global_labels, &label) - pc - 4;
    *result = FOUND;
  } else if (hash_map_contains(local_defines[current_file_index], &label)){
    imm = hash_map_get(local_defines[current_file_index], &label);
    *result = FOUND;
  } else {
    print_error();
    fprintf(stderr, "Label \"");
    print_slice_err(&label);
    fprintf(stderr, "\" has not been defined\n");
    *result = ERROR;
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

// Encode an immediate accepted by the bitwise-immediate instruction form.
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

// Encode an immediate accepted by the shift instruction form.
int encode_shift_immediate(long imm, bool* success){
  if (0 <= imm && imm <= 31){
    return imm;
  } else {
    *success = false;
    print_error();
    fprintf(stderr, "Shift instruction immediate must be in range 0 to 31\n");
    fprintf(stderr, "Got %ld\n", imm);
    return 0;
  }
}

// Encode a signed 12-bit arithmetic immediate.
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
    instruction |= ra << 22;
    instruction |= rb << 17;
    instruction |= rc;
    instruction |= alu_op << 5;
  }
  
  return instruction; 
}

// Parse a compare instruction and return its encoded word.
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
    instruction |= rb << 17;
    instruction |= rc;
    instruction |= 16 << 5; // alu_op
  }
  
  return instruction; 
}

// Encode the aligned immediate field required by LUI.
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

// Parse a LUI instruction and return its encoded word.
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
    if (result == NOT_FOUND) fprintf(stderr, "Invalid immediate\n");
    *success = false;
    return 0;
  }

  int encoding = encode_lui_immediate(imm, success);

  assert(encoding == (encoding & 0x3FFFFF)); // ensure immediate fits in 22 bits

  int instruction = 2 << 27;
  instruction |= ra << 22;
  instruction |= encoding;
  return instruction;
}

// Encode an absolute memory address in the instruction's split immediate fields.
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

// Encode a signed 16-bit memory offset.
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

// Encode the wider relative offset used by long memory instructions.
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

// Parse a memory instruction, including its addressing mode and width.
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

// Encode a signed branch displacement.
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

// Encode the displacement used by ADPC.
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

// Parse a conditional branch and return its encoded word.
int consume_branch(int branch_code, bool is_absolute, bool* success){
  int instruction = 0;

  assert(0 <= branch_code && branch_code < 19); // ensure branch code is valid

  int ra = consume_register();
  if (ra == -1){
    // it's an immediate branch
    enum ConsumeResult result;
    long imm = consume_immediate(&result);
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

// Parse an ADPC instruction and return its encoded word.
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
    long imm = consume_immediate(&result);
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

// Parse a trap instruction and return its encoded word.
int consume_trap(bool* success){
  (void)success;
  return 15 << 27;
}

// Encode the short immediate form accepted by an atomic instruction.
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

// Encode the long immediate form accepted by an atomic instruction.
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

// Parse an atomic instruction and return its encoded word.
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

// Reject an instruction unavailable in the current privilege mode.
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

// Parse a TLB-management instruction and return its encoded word.
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

// Parse a control-register move instruction and return its encoded word.
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

// Parse an end-of-interrupt instruction.
int consume_eoi(bool* success){
  check_privileges(success);
  if (!*success) return 0;

  int instruction = 31 << 27; // opcode
  instruction |= 5 << 12; // privileged ID for eoi

  skip();
  if (consume_keyword("all")) {
    instruction |= 1 << 11;
    return instruction;
  }

  enum ConsumeResult result;
  long imm = consume_immediate(&result);
  if (result != FOUND) {
    print_error();
    fprintf(stderr, "eoi instruction expects 'all' or an ISR bit index in range 0 to 15\n");
    *success = false;
    return 0;
  }
  if (imm < 0 || imm > 15) {
    print_error();
    fprintf(stderr, "eoi bit index must be in range 0 to 15\n");
    fprintf(stderr, "Got %ld\n", imm);
    *success = false;
    return 0;
  }

  instruction |= imm & 0xF;
  return instruction;
}

// Parse an instruction that changes processor mode state.
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

// Parse a return-from-exception instruction.
int consume_rfe(bool* success){
  check_privileges(success);
  if (!*success) return 0;

  int instruction = 31 << 27;
  instruction |= 3 << 12;

  return instruction;
}

// Parse an interprocessor-interrupt instruction.
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
    long imm = consume_literal(&result);
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
    struct Slice label;
    consume_identifier(&label);

    // hack to see if this was a .define and not a label
    if (!hash_map_contains(local_defines[current_file_index], &label)) mov_type |= 2;
  }
  else imm = consume_literal(&result);
  if (result != FOUND){
    print_error();
    if (result == NOT_FOUND) fprintf(stderr, "movi expects label or integer literal\n");
    *success = false;
    return 0;
  }

  // Label immediates select movu8/movl4; numeric immediates use movu/movl.

  // [0] movu := lui rA, (imm & 0xFFFFFC00)
  // [1] movl := addi rA, rA, (imm & 0x3FF)
  // [2] movu8 := lui rA, ((imm - 8) & 0xFFFFFC00)
  // [3] movl4 := addi rA, rA, ((imm - 4) & 0x3FF)

  if (mov_type == 2) imm -= 8;
  else if (mov_type == 3) imm -= 4;
  
  int instruction = 0;

  if (mov_type & 1){

    instruction |= 1 << 27; // opcode for add
    instruction |= ra << 22;
    instruction |= ra << 17;
    instruction |= 14 << 12; // add is 14

    int encoding = encode_arithmetic_immediate(imm & 0x3FF, success);

    assert(encoding == (encoding & 0xFFF)); // ensure encoding always fits in 12 bits

    instruction |= encoding;
  } else {
    int encoding = encode_lui_immediate(imm & 0xFFFFFC00, success);

    assert(encoding == (encoding & 0x3FFFFF)); // ensure immediate fits in 22 bits

    instruction = 2 << 27; // opcode for lui
    instruction |= ra << 22;
    instruction |= encoding;
  }
  
  return instruction; 
}

// Parse and store a .define directive in the current definition map.
void record_define(bool* success){
  struct Slice label;
  if (!consume_identifier(&label)){
    // error
    print_error();
    fprintf(stderr, "Expected label\n");
    *success = false;
    return;
  }

  enum ConsumeResult result;
  long imm = consume_literal(&result);
  if (result == NOT_FOUND){
    struct Slice value_label;
    if (!consume_identifier(&value_label)){
      print_error();
      fprintf(stderr, "Expected integer literal or label\n");
      *success = false;
      return;
    }
    if (hash_map_contains(local_defines[current_file_index], &value_label)){
      imm = hash_map_get(local_defines[current_file_index], &value_label);
    } else if (label_has_definition(local_labels[current_file_index], &value_label)){
      imm = hash_map_get(local_labels[current_file_index], &value_label);
    } else if (label_has_definition(global_labels, &value_label)){
      imm = hash_map_get(global_labels, &value_label);
    } else {
      print_error();
      fprintf(stderr, "Label \"");
      print_slice_err(&value_label);
      fprintf(stderr, "\" has not been defined\n");
      *success = false;
      return;
    }
  } else if (result != FOUND){
    // error
    print_error();
    fprintf(stderr, "Expected integer literal or label\n");
    *success = false;
    return;
  }

  if (hash_map_contains(local_defines[current_file_index], &label)){
    // error
    print_error();
    fprintf(stderr, "constant has multiple definitions\n");
    *success = false;
    return;
  }
  hash_map_insert(local_defines[current_file_index], &label, imm, true, true);
}

// consumes a single instruction and converts it to binary or hex
int consume_instruction(enum ConsumeResult* result){
  int instruction = 0;
  bool success = true;

  // user instructions
  skip();

  // One token hash replaces the mnemonic cascade. Prefixes such as "add"/"addc"
  // stay distinct because the matcher consumes the whole identifier.
  switch (take_keyword(KW_CLASS_MNEMONIC)) {
    case KW_AND: instruction = consume_alu_op(0, &success); break;
    case KW_NAND: instruction = consume_alu_op(1, &success); break;
    case KW_OR: instruction = consume_alu_op(2, &success); break;
    case KW_NOR: instruction = consume_alu_op(3, &success); break;
    case KW_XOR: instruction = consume_alu_op(4, &success); break;
    case KW_XNOR: instruction = consume_alu_op(5, &success); break;
    case KW_NOT: instruction = consume_alu_op(6, &success); break;
    case KW_LSL: instruction = consume_alu_op(7, &success); break;
    case KW_LSR: instruction = consume_alu_op(8, &success); break;
    case KW_ASR: instruction = consume_alu_op(9, &success); break;
    case KW_ROTL: instruction = consume_alu_op(10, &success); break;
    case KW_ROTR: instruction = consume_alu_op(11, &success); break;
    case KW_LSLC: instruction = consume_alu_op(12, &success); break;
    case KW_LSRC: instruction = consume_alu_op(13, &success); break;
    case KW_ADD: instruction = consume_alu_op(14, &success); break;
    case KW_ADDC: instruction = consume_alu_op(15, &success); break;
    case KW_SUB: instruction = consume_alu_op(16, &success); break;
    case KW_SUBB: instruction = consume_alu_op(17, &success); break;
    case KW_CMP: instruction = consume_cmp(&success); break;
    case KW_SXTB: instruction = consume_alu_op(18, &success); break;
    case KW_SXTD: instruction = consume_alu_op(19, &success); break;
    case KW_TNCB: instruction = consume_alu_op(20, &success); break;
    case KW_TNCD: instruction = consume_alu_op(21, &success); break;
    case KW_LUI: instruction = consume_lui(&success); break;
    case KW_SWA: instruction = consume_mem(0, true, false, &success); break;
    case KW_LWA: instruction = consume_mem(0, true, true, &success); break;
    case KW_SW: instruction = consume_mem(0, false, false, &success); break;
    case KW_LW: instruction = consume_mem(0, false, true, &success); break;
    case KW_SDA: instruction = consume_mem(1, true, false, &success); break;
    case KW_LDA: instruction = consume_mem(1, true, true, &success); break;
    case KW_SD: instruction = consume_mem(1, false, false, &success); break;
    case KW_LD: instruction = consume_mem(1, false, true, &success); break;
    case KW_SBA: instruction = consume_mem(2, true, false, &success); break;
    case KW_LBA: instruction = consume_mem(2, true, true, &success); break;
    case KW_SB: instruction = consume_mem(2, false, false, &success); break;
    case KW_LB: instruction = consume_mem(2, false, true, &success); break;
    case KW_BR: instruction = consume_branch(0, false, &success); break;
    case KW_BZ: instruction = consume_branch(1, false, &success); break;
    case KW_BNZ: instruction = consume_branch(2, false, &success); break;
    case KW_BS: instruction = consume_branch(3, false, &success); break;
    case KW_BNS: instruction = consume_branch(4, false, &success); break;
    case KW_BC: instruction = consume_branch(5, false, &success); break;
    case KW_BNC: instruction = consume_branch(6, false, &success); break;
    case KW_BO: instruction = consume_branch(7, false, &success); break;
    case KW_BNO: instruction = consume_branch(8, false, &success); break;
    case KW_BPS: instruction = consume_branch(9, false, &success); break;
    case KW_BNPS: instruction = consume_branch(10, false, &success); break;
    case KW_BG: instruction = consume_branch(11, false, &success); break;
    case KW_BGE: instruction = consume_branch(12, false, &success); break;
    case KW_BL: instruction = consume_branch(13, false, &success); break;
    case KW_BLE: instruction = consume_branch(14, false, &success); break;
    case KW_BA: instruction = consume_branch(15, false, &success); break;
    case KW_BAE: instruction = consume_branch(16, false, &success); break;
    case KW_BB: instruction = consume_branch(17, false, &success); break;
    case KW_BBE: instruction = consume_branch(18, false, &success); break;
    case KW_BRA: instruction = consume_branch(0, true, &success); break;
    case KW_BZA: instruction = consume_branch(1, true, &success); break;
    case KW_BNZA: instruction = consume_branch(2, true, &success); break;
    case KW_BSA: instruction = consume_branch(3, true, &success); break;
    case KW_BNSA: instruction = consume_branch(4, true, &success); break;
    case KW_BCA: instruction = consume_branch(5, true, &success); break;
    case KW_BNCA: instruction = consume_branch(6, true, &success); break;
    case KW_BOA: instruction = consume_branch(7, true, &success); break;
    case KW_BNOA: instruction = consume_branch(8, true, &success); break;
    case KW_BPA: instruction = consume_branch(9, true, &success); break;
    case KW_BNPA: instruction = consume_branch(10, true, &success); break;
    case KW_BGA: instruction = consume_branch(11, true, &success); break;
    case KW_BGEA: instruction = consume_branch(12, true, &success); break;
    case KW_BLA: instruction = consume_branch(13, true, &success); break;
    case KW_BLEA: instruction = consume_branch(14, true, &success); break;
    case KW_BAA: instruction = consume_branch(15, true, &success); break;
    case KW_BAEA: instruction = consume_branch(16, true, &success); break;
    case KW_BBA: instruction = consume_branch(17, true, &success); break;
    case KW_BBEA: instruction = consume_branch(18, true, &success); break;
    case KW_JMP: instruction = consume_jmp(&success); break;
    case KW_ADPC: instruction = consume_adpc(&success); break;
    case KW_TRAP: instruction = consume_trap(&success); break;
    case KW_FADA: instruction = consume_atomic(true, true, &success); break;
    case KW_FAD: instruction = consume_atomic(false, true, &success); break;
    case KW_SWPA: instruction = consume_atomic(true, false, &success); break;
    case KW_SWP: instruction = consume_atomic(false, false, &success); break;
    case KW_TLBR: instruction = consume_tlb_op(0, &success); break;
    case KW_TLBW: instruction = consume_tlb_op(1, &success); break;
    case KW_TLBI: instruction = consume_tlb_op(2, &success); break;
    case KW_TLBC: instruction = consume_tlb_op(3, &success); break;
    case KW_CRMV: instruction = consume_crmv(&success); break;
    case KW_MODE: instruction = consume_mode_op(&success); break;
    case KW_RFE: instruction = consume_rfe(&success); break;
    case KW_IPI: instruction = consume_ipi(&success); break;
    case KW_EOI: instruction = consume_eoi(&success); break;
    // hacks to make movi and call work
    case KW_MOVU: instruction = consume_mov_hack(0, &success); break;
    case KW_MOVL: instruction = consume_mov_hack(1, &success); break;
    default: *result = NOT_FOUND; break;
  }

  if (!success) *result = ERROR;

  return instruction;
}

// First pass to collect labels and section sizes without emitting output.
// Returns true on success; updates label maps and section offsets.
bool process_labels(char const* const prog){
  current = prog;
  current_buffer_start = prog - 1;
  line_count = 1;

  // Bucket counts track source size. Compiler output is roughly one label per
  // few dozen bytes; .define names and per-file .global sets stay small, so
  // those tables do not need a thousand empty buckets each.
  enum {
    kMinLabelBuckets = 32,
    kMaxLabelBuckets = 16384,
    kLabelBytesPerBucket = 48,
    kSparseSymbolBuckets = 32
  };
  size_t src_len = 0;
  while (prog[src_len] != '\0') src_len++;
  size_t label_buckets = kMinLabelBuckets;
  size_t label_need = src_len / kLabelBytesPerBucket;
  if (label_need < kMinLabelBuckets) label_need = kMinLabelBuckets;
  if (label_need > kMaxLabelBuckets) label_need = kMaxLabelBuckets;
  while (label_buckets < label_need) label_buckets *= 2;

  local_labels[current_file_index] = create_hash_map(label_buckets);
  local_defines[current_file_index] = create_hash_map(kSparseSymbolBuckets);
  local_globals[current_file_index] = create_hash_map(kSparseSymbolBuckets);
  if (local_labels[current_file_index] == NULL ||
      local_defines[current_file_index] == NULL ||
      local_globals[current_file_index] == NULL) {
    return false;
  }
  if (!apply_cli_defines()) return false;

  while (!is_at_end()){

    struct Slice label;
    if (consume_label(&label)) {
      if (!ensure_valid_section("label")) return false;
      long label_value = (long)encode_section_offset(current_section, section_offsets[current_section]);

      // check for duplicates
      if (hash_map_contains(local_labels[current_file_index], &label)){
        if (label_has_definition(local_labels[current_file_index], &label)){
          // duplicate label error
          print_error();
          fprintf(stderr, "Duplicate label\n");
          return false;
        } else {
          make_defined(local_labels[current_file_index], &label, label_value);
        }
      } else {
        hash_map_insert(local_labels[current_file_index], &label, label_value, true, current_section != TEXT_SECTION);
      }

      // Check for duplicates on globals explicitly declared in this file.
      if (hash_map_contains(local_globals[current_file_index], &label)){
        if (label_has_definition(global_labels, &label)){
          // duplicate label error
          print_error();
          fprintf(stderr, "Duplicate global label\n");
          return false;
        } else {
          make_defined(global_labels, &label, label_value);
        }
      }

    } else {
      skip();
      // One lookup classifies the directive. The token stays put when it is not one.
      enum KeywordId dir = take_keyword(KW_CLASS_DIRECTIVE);
      if ((dir == KW_DIR_GLOBAL)) {
        struct Slice label;
        if (consume_identifier(&label)){
          // Track per-file global declarations to detect duplicate exports.
          if (!hash_map_contains(local_globals[current_file_index], &label)){
            hash_map_insert(local_globals[current_file_index], &label, 0, false,
              current_section != TEXT_SECTION); // mark as data if not in text section
          }
          if (!hash_map_contains(global_labels, &label)){
            hash_map_insert(global_labels, &label, 0, false, current_section != TEXT_SECTION);
          }

          if (label_has_definition(local_labels[current_file_index], &label)){
            if (label_has_definition(global_labels, &label)){
              print_error();
              fprintf(stderr, "Duplicate global label\n");
              return false;
            }
            make_defined(global_labels, &label, hash_map_get(local_labels[current_file_index], &label));
          }
        } else {
          print_error();
          fprintf(stderr, ".global directive requires a label\n");
          return false;
        }

        continue;
      } else if ((dir == KW_DIR_ORIGIN)) { 
        if (!is_kernel){
          print_error();
          fprintf(stderr, ".origin can only be used in kernel mode\n");
          return false;
        }
        if (current_section != IMPLICIT_SECTION){
          print_error();
          fprintf(stderr, ".origin can only be used before selecting an explicit section\n");
          fprintf(stderr, "Move .origin directives before .text/.rodata/.data/.bss\n");
          return false;
        }

        enum ConsumeResult result;
        long imm = consume_define_or_literal(&result, ".origin");
        if (result != FOUND){
          if (result == NOT_FOUND){
            print_error();
            fprintf(stderr, "Invalid .origin value; expected integer literal or .define constant\n");
          }
          return false;
        }
        if (imm < (long)section_offsets[current_section]){
          print_error();
          fprintf(stderr, ".origin cannot be used to go backwards\n");
          return false;
        } else if (imm >= ((long)1 << 32)){
          print_error();
          fprintf(stderr, ".origin address must be a 32 bit integer\n");
          return false;
        }
        section_offsets[current_section] = (uint32_t)imm;
        pc = section_offsets[current_section];
        continue;
      }
      else if ((dir == KW_DIR_TEXT)) {
        current_section = TEXT_SECTION;
        pc = section_offsets[current_section];
        continue;
      }
      else if ((dir == KW_DIR_RODATA)) {
        current_section = RODATA_SECTION;
        pc = section_offsets[current_section];
        continue;
      }
      else if ((dir == KW_DIR_DATA)) {
        current_section = DATA_SECTION;
        pc = section_offsets[current_section];
        continue;
      }
      else if ((dir == KW_DIR_BSS)) {
        current_section = BSS_SECTION;
        pc = section_offsets[current_section];
        continue;
      }
      else if ((dir == KW_DIR_TEXT_LOAD)) {
        if (!parse_section_load_directive(TEXT_SECTION, ".text_load")) return false;
        continue;
      }
      else if ((dir == KW_DIR_RODATA_LOAD)) {
        if (!parse_section_load_directive(RODATA_SECTION, ".rodata_load")) return false;
        continue;
      }
      else if ((dir == KW_DIR_DATA_LOAD)) {
        if (!parse_section_load_directive(DATA_SECTION, ".data_load")) return false;
        continue;
      }
      else if ((dir == KW_DIR_BSS_LOAD)) {
        if (!parse_section_load_directive(BSS_SECTION, ".bss_load")) return false;
        continue;
      }
      else if ((dir == KW_DIR_FILL)) {
        enum ConsumeResult result; 
        consume_define_or_literal_or_label_abs(&result, ".fill");
        if (result != FOUND){
          if (result == NOT_FOUND){
            print_error();
            fprintf(stderr, "Invalid .fill immediate; expected integer literal, label, or .define constant\n");
          }
          return false;
        }
        if (!ensure_valid_section(".fill")) return false;
        if (current_section == BSS_SECTION){
          print_error();
          fprintf(stderr, ".fill not allowed in .bss section\n");
          return false;
        }
        section_offsets[current_section] += kWordBytes;
        pc = section_offsets[current_section];
        continue;
      }
      else if ((dir == KW_DIR_FILD)) {
        enum ConsumeResult result; 
        consume_define_or_literal(&result, ".fild");
        if (result != FOUND){
          if (result == NOT_FOUND){
            print_error();
            fprintf(stderr, "Invalid .fild immediate; expected integer literal or .define constant\n");
          }
          return false;
        }
        if (!ensure_valid_section(".fild")) return false;
        if (current_section == BSS_SECTION){
          print_error();
          fprintf(stderr, ".fild not allowed in .bss section\n");
          return false;
        }
        section_offsets[current_section] += kHalfBytes;
        pc = section_offsets[current_section];
        continue;
      }
      else if ((dir == KW_DIR_FILB)) {
        enum ConsumeResult result; 
        consume_define_or_literal(&result, ".filb");
        if (result != FOUND){
          if (result == NOT_FOUND){
            print_error();
            fprintf(stderr, "Invalid .filb immediate; expected integer literal or .define constant\n");
          }
          return false;
        }
        if (!ensure_valid_section(".filb")) return false;
        if (current_section == BSS_SECTION){
          print_error();
          fprintf(stderr, ".filb not allowed in .bss section\n");
          return false;
        }
        section_offsets[current_section] += kByteBytes;
        pc = section_offsets[current_section];
        continue;
      }
      else if ((dir == KW_DIR_SPACE)) { 
        enum ConsumeResult result; 
        long imm = consume_define_or_literal(&result, ".space");
        if (result != FOUND){
          if (result == NOT_FOUND){
            print_error();
            fprintf(stderr, "Invalid .space count; expected integer literal or .define constant\n");
          }
          return false;
        }
        if (!ensure_valid_section(".space")) return false;
        section_offsets[current_section] += imm;
        pc = section_offsets[current_section];
        continue;
      }
      else if ((dir == KW_DIR_ALIGN)) {
        enum ConsumeResult result;
        uint32_t alignment = 0;
        if (!parse_alignment(&result, ".align", &alignment)) return false;
        if (!ensure_valid_section(".align")) return false;
        section_offsets[current_section] =
          align_up(section_offsets[current_section], alignment);
        pc = section_offsets[current_section];
        continue;
      }
      else if ((dir == KW_DIR_DEFINE)) {
        bool success = true;
        record_define(&success);
        if (!success) return false;
        continue;
      }
      else if ((dir == KW_DIR_LINE)) {
        // handled in second pass
        skip_line();
        continue;
      }
      else if ((dir == KW_DIR_LOCAL)) {
        // handled in second pass
        skip_line();
        continue;
      }
      
      enum ConsumeResult result = FOUND;
      if (!ensure_valid_section("instruction")) return false;
      if (current_section == BSS_SECTION){
        print_error();
        fprintf(stderr, "Instructions not allowed in .bss section\n");
        return false;
      }
      if (section_offsets[current_section] % kWordBytes != 0){
        return report_instruction_alignment_error(section_offsets[current_section], "section offset");
      }
      consume_instruction(&result);
      if (result == ERROR) return false;
      if (result == NOT_FOUND) {
        print_error();
        fprintf(stderr, "Unrecognized instruction\n");
        return false;
      }
      section_offsets[current_section] += kWordBytes;
      pc = section_offsets[current_section];
    }
  }
  return true;
}

// Second pass to emit instruction/data bytes into output sections.
// Returns true on success; appends words to instruction arrays and updates bss_size.
bool to_binary(char const* const prog, struct InstructionArrayList* instructions){
  current = prog;
  current_buffer_start = prog - 1;
  line_count = 1;

  enum ConsumeResult success = FOUND;

  while (success == FOUND){
    // consume any labels, they were already dealt with
    struct Slice defined_label;
    while (skip_newline(), consume_label(&defined_label));
    skip_newline();

    if (pc > ((long)1 << 32)){
      print_error();
      fprintf(stderr, "Program does not fit in 32-bit address space\n");
      return false;
    }

    // directives
    enum KeywordId dir = take_keyword(KW_CLASS_DIRECTIVE);
    if ((dir == KW_DIR_GLOBAL)) {
      // handled in first pass
      struct Slice name;
      if (!consume_identifier(&name)){
        print_error();
        fprintf(stderr, ".global directive requires a label\n");
        return false;
      }
      if (!label_has_definition(global_labels, &name)){
        print_error();
        fprintf(stderr, "Global label \"");
        print_slice_err(&name);
        fprintf(stderr, "\" missing from first pass\n");
        return false;
      }
    }
    else if ((dir == KW_DIR_DEFINE)){
      skip_line();
    } // handled in first pass
    else if ((dir == KW_DIR_ORIGIN)) { 
      if (is_kernel){
        enum ConsumeResult result;
        long imm = consume_define_or_literal(&result, ".origin");
        if (result != FOUND){
          if (result == NOT_FOUND){
            print_error();
            fprintf(stderr, "Invalid .origin value; expected integer literal or .define constant\n");
          }
          return false;
        }
        if (current_section != IMPLICIT_SECTION){
          print_error();
          fprintf(stderr, ".origin can only be used before selecting an explicit section\n");
          fprintf(stderr, "Move .origin directives before .text/.rodata/.data/.bss\n");
          return false;
        }
        if (imm < (long)section_offsets[current_section]){
          print_error();
          fprintf(stderr, ".origin cannot be used to go backwards\n");
          return false;
        } else if (imm >= ((long)1 << 32)){
          print_error();
          fprintf(stderr, ".origin address must be a 32 bit integer\n");
          return false;
        }
        uint32_t target = (uint32_t)imm;
        uint32_t current = section_offsets[current_section];
        uint32_t pad = target - current;
        append_zero_bytes_user(section_arrays[current_section], pad, current_section);
        pc = section_pc_base(current_section) + section_offsets[current_section];
      } else {
        print_error();
        fprintf(stderr, ".origin can only be used in kernel mode\n");
        return false;
      }
    }
    else if ((dir == KW_DIR_TEXT)) {
      current_section = TEXT_SECTION;
      pc = section_pc_base(current_section) + section_offsets[current_section];
    }
    else if ((dir == KW_DIR_RODATA)) {
      current_section = RODATA_SECTION;
      pc = section_pc_base(current_section) + section_offsets[current_section];
    }
    else if ((dir == KW_DIR_DATA)) {
      current_section = DATA_SECTION;
      pc = section_pc_base(current_section) + section_offsets[current_section];
    }
    else if ((dir == KW_DIR_BSS)) {
      current_section = BSS_SECTION;
      pc = section_pc_base(current_section) + section_offsets[current_section];
    }
    else if ((dir == KW_DIR_TEXT_LOAD)) {
      if (!parse_section_load_directive(TEXT_SECTION, ".text_load")) return false;
    }
    else if ((dir == KW_DIR_RODATA_LOAD)) {
      if (!parse_section_load_directive(RODATA_SECTION, ".rodata_load")) return false;
    }
    else if ((dir == KW_DIR_DATA_LOAD)) {
      if (!parse_section_load_directive(DATA_SECTION, ".data_load")) return false;
    }
    else if ((dir == KW_DIR_BSS_LOAD)) {
      if (!parse_section_load_directive(BSS_SECTION, ".bss_load")) return false;
    }
    else if ((dir == KW_DIR_FILL)) {
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
        if (!ensure_valid_section(".fill")) return false;
        if (current_section == TEXT_SECTION){
          print_warning(".fill used in .text section");
        }
        if (current_section == BSS_SECTION){
          print_error();
          fprintf(stderr, ".fill not allowed in .bss section\n");
          return false;
        }
        append_bytes_user(section_arrays[current_section], bytes, kWordBytes, current_section);
      } else {
        print_error();
        fprintf(stderr, ".fill immediate must fit in a 32-bit value\n");
        return false;
      }
    }
    else if ((dir == KW_DIR_FILD)) {
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
        if (!ensure_valid_section(".fild")) return false;
        if (current_section == TEXT_SECTION){
          print_warning(".fild used in .text section");
        }
        if (current_section == BSS_SECTION){
          print_error();
          fprintf(stderr, ".fild not allowed in .bss section\n");
          return false;
        }
        uint8_t bytes[kHalfBytes];
        encode_value_bytes(value, bytes, kHalfBytes);
        append_bytes_user(section_arrays[current_section], bytes, kHalfBytes, current_section);
      } else {
        print_error();
        fprintf(stderr, ".fild immediate must fit in a 16-bit value\n");
        return false;
      }
    }
    else if ((dir == KW_DIR_FILB)) {
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
        if (!ensure_valid_section(".filb")) return false;
        if (current_section == TEXT_SECTION){
          print_warning(".filb used in .text section");
        }
        if (current_section == BSS_SECTION){
          print_error();
          fprintf(stderr, ".filb not allowed in .bss section\n");
          return false;
        }
        append_bytes_user(section_arrays[current_section], &value, kByteBytes, current_section);
      } else {
        print_error();
        fprintf(stderr, ".filb immediate must fit in an 8-bit value\n");
        return false;
      }
    }
    else if ((dir == KW_DIR_SPACE)) { 
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
        if (!ensure_valid_section(".space")) return false;
        if (current_section == BSS_SECTION){
          bss_size += (uint32_t)imm;
          section_offsets[current_section] += (uint32_t)imm;
          pc = section_pc_base(current_section) + section_offsets[current_section];
        } else {
          append_zero_bytes_user(section_arrays[current_section], (uint32_t)imm, current_section);
        }
      } else {
        print_error();
        fprintf(stderr, ".space immediate must be a positive 32 bit integer\n");
        return false;
      }
    }
    else if ((dir == KW_DIR_LINE)) {
      // Parse filename and line number; record the address of the next instruction.
      struct Slice filename;
      if (!consume_filename(&filename)){
        print_error();
        fprintf(stderr, ".line directive requires a filename\n");
        return false;
      }
      enum ConsumeResult result;
      long line_num = consume_literal(&result);
      if (result != FOUND){
        print_error();
        fprintf(stderr, ".line directive requires a line number\n");
        return false;
      }
      if (debug_info_list != NULL) {
        add_debug_line(debug_info_list, &filename, line_num, (uint32_t)pc);
      }
    }
    else if ((dir == KW_DIR_LOCAL)) {
      // Parse name and bp offset; record the address where locals become visible.
      struct Slice varname;
      if (!consume_identifier(&varname)){
        print_error();
        fprintf(stderr, ".local directive requires a variable name\n");
        return false;
      }
      enum ConsumeResult result;
      long bp_offset = consume_literal(&result);
      if (result != FOUND){
        print_error();
        fprintf(stderr, ".local directive requires a bp offset\n");
        return false;
      }
      long size_value = consume_literal(&result);
      if (result != FOUND){
        print_error();
        fprintf(stderr, ".local directive requires a size in bytes\n");
        return false;
      }
      if (size_value <= 0 || size_value > UINT32_MAX) {
        print_error();
        fprintf(stderr, ".local directive size must be a positive 32-bit value\n");
        return false;
      }
      if (debug_info_list != NULL) {
        add_debug_local(debug_info_list, &varname, bp_offset, (size_t)size_value, (uint32_t)pc);
      }
    }
    else if ((dir == KW_DIR_ALIGN)) {
      enum ConsumeResult result;
      uint32_t alignment = 0;
      if (!parse_alignment(&result, ".align", &alignment)) return false;

      if (!ensure_valid_section(".align")) return false;
      uint32_t current = section_offsets[current_section];
      uint32_t aligned = align_up(current, alignment);
      uint32_t pad = aligned - current;
      if (current_section == BSS_SECTION){
        bss_size += pad;
        section_offsets[current_section] += pad;
        pc = section_pc_base(current_section) + section_offsets[current_section];
      } else {
        append_zero_bytes_user(section_arrays[current_section], pad, current_section);
      }
      continue;
    } else {
      if (!ensure_valid_section("instruction")) return false;
      pc = section_pc_base(current_section) + section_offsets[current_section];
      int instruction = consume_instruction(&success);
      if (success == FOUND) {
        if (current_section == BSS_SECTION){
          print_error();
          fprintf(stderr, "Instructions not allowed in .bss section\n");
          return false;
        }
        if (section_offsets[current_section] % kWordBytes != 0){
          return report_instruction_alignment_error((uint32_t)pc, "pc");
        }
        if (current_section == RODATA_SECTION){
          print_warning("Instruction emitted in .rodata section");
        } else if (current_section == DATA_SECTION){
          print_warning("Instruction emitted in .data section");
        }
        instruction_array_append(section_arrays[current_section], instruction);
        section_offsets[current_section] += kWordBytes;
        pc = section_pc_base(current_section) + section_offsets[current_section];
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

// Append labels from a definition map to the output label list.
static void append_labels_from_map(struct HashMap* map, struct LabelList* labels, uint32_t offset){
  for (size_t i = 0; i < map->size; ++i){
    struct HashEntry* entry = map->arr[i];
    while (entry != NULL){
      if (entry->is_defined){
        uint32_t addr = (uint32_t)(entry->value + offset);
        label_list_append(labels, entry->key.start, entry->key.len, addr, entry->is_data);
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
  current_section = is_kernel ? IMPLICIT_SECTION : NO_SECTION;
  text_instruction_array = NULL;
  rodata_instruction_array = NULL;
  data_instruction_array = NULL;
  for (int i = 0; i < SECTION_COUNT; ++i) section_arrays[i] = NULL;
  bss_size = 0;
  reset_section_offsets();
  reset_section_load_bases();
  for (int i = 0; i < SECTION_COUNT; ++i) section_sizes[i] = 0;
  for (int i = 0; i < SECTION_COUNT; ++i) section_bases[i] = 0;

  // Source-line records are only retained when the caller asked for -g output.
  debug_info_list = (labels_out_c != NULL) ? create_debug_info_list() : NULL;

  if (labels_out != NULL) *labels_out = NULL;
  if (labels_out_c != NULL) *labels_out_c = NULL;

  current_file_index = 0;

  local_labels = malloc(num_files * sizeof(struct HashMap*));
  local_defines = malloc(num_files * sizeof(struct HashMap*));
  local_globals = malloc(num_files * sizeof(struct HashMap*));

  // Shared export table. Kernel images export more symbols than any one file.
  enum { kGlobalSymbolBuckets = 4096 };
  global_labels = create_hash_map(kGlobalSymbolBuckets);
  if (global_labels == NULL) {
    free(local_labels);
    free(local_defines);
    free(local_globals);
    destroy_debug_info_list(debug_info_list);
    debug_info_list = NULL;
    return NULL;
  }
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
      destroy_debug_info_list(debug_info_list);
      debug_info_list = NULL;
      return NULL;
    }
  }

  if (!is_kernel){
    section_sizes[TEXT_SECTION] = align_up(section_offsets[TEXT_SECTION], kWordBytes);
    section_sizes[RODATA_SECTION] = align_up(section_offsets[RODATA_SECTION], kWordBytes);
    section_sizes[DATA_SECTION] = align_up(section_offsets[DATA_SECTION], kWordBytes);
    section_sizes[BSS_SECTION] = section_offsets[BSS_SECTION];
    compute_section_bases();
  } else {
    for (int i = 0; i < SECTION_COUNT; ++i){
      section_sizes[i] = section_offsets[i];
    }
    section_sizes[END_SECTION] = kWordBytes;
    compute_kernel_section_bases();
  }

  finalize_section_load_bases();

  for (int i = 0; i < num_files; ++i) adjust_label_map_for_sections(local_labels[i]);
  adjust_label_map_for_sections(global_labels);

  if (!is_kernel){
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
      destroy_debug_info_list(debug_info_list);
      debug_info_list = NULL;
      return NULL;
    }
    entry_point = (uint32_t)hash_map_get(global_labels, &start_label);
  }

  pass_number = 2;

  struct InstructionArrayList* instructions = create_instruction_array_list();

  // Pass 1 already measured each section. Allocate the word buffer once instead
  // of doubling from a 10-word seed. .bss is not materialized as bytes.
  if (is_kernel){
    if (!reserve_instruction_words(instructions->head, section_word_capacity(IMPLICIT_SECTION))) {
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_labels[j]);
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_defines[j]);
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_globals[j]);
      free(local_labels);
      free(local_defines);
      free(local_globals);
      destroy_hash_map(global_labels);
      destroy_instruction_array_list(instructions);
      destroy_debug_info_list(debug_info_list);
      debug_info_list = NULL;
      return NULL;
    }
    instructions->head->origin = section_bases[IMPLICIT_SECTION];
    section_arrays[IMPLICIT_SECTION] = instructions->head;
    struct InstructionArray* arr_text = create_instruction_array(section_word_capacity(TEXT_SECTION), section_bases[TEXT_SECTION]);
    struct InstructionArray* arr_rodata = create_instruction_array(section_word_capacity(RODATA_SECTION), section_bases[RODATA_SECTION]);
    struct InstructionArray* arr_data = create_instruction_array(section_word_capacity(DATA_SECTION), section_bases[DATA_SECTION]);
    struct InstructionArray* arr_bss = create_instruction_array(1, section_bases[BSS_SECTION]);
    struct InstructionArray* arr_end = create_instruction_array(section_word_capacity(END_SECTION), section_bases[END_SECTION]);
    instruction_array_list_append(instructions, arr_text);
    instruction_array_list_append(instructions, arr_rodata);
    instruction_array_list_append(instructions, arr_data);
    instruction_array_list_append(instructions, arr_bss);
    instruction_array_list_append(instructions, arr_end);
    section_arrays[TEXT_SECTION] = arr_text;
    section_arrays[RODATA_SECTION] = arr_rodata;
    section_arrays[DATA_SECTION] = arr_data;
    section_arrays[BSS_SECTION] = arr_bss;
    section_arrays[END_SECTION] = arr_end;
  } else {
    text_instruction_array = instructions->head;
    if (!reserve_instruction_words(text_instruction_array, section_word_capacity(TEXT_SECTION))) {
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_labels[j]);
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_defines[j]);
      for (int j = 0; j < num_files; ++j) destroy_hash_map(local_globals[j]);
      free(local_labels);
      free(local_defines);
      free(local_globals);
      destroy_hash_map(global_labels);
      destroy_instruction_array_list(instructions);
      destroy_debug_info_list(debug_info_list);
      debug_info_list = NULL;
      return NULL;
    }
    text_instruction_array->origin = section_bases[TEXT_SECTION];
    rodata_instruction_array = create_instruction_array(section_word_capacity(RODATA_SECTION), section_bases[RODATA_SECTION]);
    data_instruction_array = create_instruction_array(section_word_capacity(DATA_SECTION), section_bases[DATA_SECTION]);
    instruction_array_list_append(instructions, rodata_instruction_array);
    instruction_array_list_append(instructions, data_instruction_array);
    section_arrays[TEXT_SECTION] = text_instruction_array;
    section_arrays[RODATA_SECTION] = rodata_instruction_array;
    section_arrays[DATA_SECTION] = data_instruction_array;
  }

  reset_section_offsets();
  current_section = is_kernel ? IMPLICIT_SECTION : NO_SECTION;
  bss_size = 0;
  pc = is_kernel ? section_pc_base(IMPLICIT_SECTION) : section_pc_base(TEXT_SECTION);
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
      destroy_debug_info_list(debug_info_list);
      debug_info_list = NULL;
      return NULL;
    }
  }

  if (is_kernel){
    uint32_t sentinel = 0xAAAAAAAAu;
    uint8_t bytes[kWordBytes];
    encode_value_bytes(sentinel, bytes, kWordBytes);
    append_bytes_user(section_arrays[END_SECTION], bytes, kWordBytes, END_SECTION);
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

  if (labels_out_c != NULL) {
    *labels_out_c = debug_info_list;
  } else {
    destroy_debug_info_list(debug_info_list);
  }
  debug_info_list = NULL;

  return program;
}
