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
#include "encode.h"
#include "charclass.h"

/*
  Two-pass assembler.
  First pass calculates addresses of labels
  Second pass converts to text into binary
*/

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
static long consume_constant(enum ConsumeResult* result, const char* context);

// Check whether a value is a power-of-two alignment.
// Returns true when value is a nonzero power of two.
static bool is_power_of_two_u32(uint32_t value){
  return value != 0 && (value & (value - 1)) == 0;
}

// Parse and validate a byte alignment value for .align.
// Returns true on success and fills alignment_out.
static bool parse_alignment(enum ConsumeResult* result, const char* directive,
                            uint32_t* alignment_out){
  long imm = consume_constant(result, directive);
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
  long imm = consume_constant(&result, directive);
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

// Look up a .define / -D constant visible in the current file.
static bool lookup_define(const struct Slice* name, long* value) {
  struct HashEntry* entry = hash_map_find(local_defines[current_file_index], name);
  if (entry == NULL) return false;
  *value = entry->value;
  return true;
}

// Look up a defined label visible from the current file: this file's labels
// first, then labels exported with .global by any file. Before layout
// (pass 1) the value is a packed section offset; afterwards it is absolute.
static bool lookup_label(const struct Slice* name, long* value) {
  struct HashMap* locals = local_labels[current_file_index];
  if (label_has_definition(locals, name)) {
    *value = hash_map_get(locals, name);
    // If this label is global in this file, the global entry should match.
    assert(!(hash_map_contains(local_globals[current_file_index], name) &&
             label_has_definition(global_labels, name)) ||
           hash_map_get(global_labels, name) == *value);
    return true;
  }
  if (label_has_definition(global_labels, name)) {
    *value = hash_map_get(global_labels, name);
    return true;
  }
  return false;
}

// Resolve an operand name according to `flags` (see enum OperandFlags).
// Reports an undefined name. `context` names the directive in that message;
// NULL selects the instruction wording ("Label ... has not been defined").
static long resolve_operand_name(const struct Slice* name, unsigned flags, const char* context,
                                 enum ConsumeResult* result, enum OperandKind* kind) {
  bool labels = (flags & OPERAND_LABELS) != 0;
  bool defines_first = (flags & OPERAND_LABELS_FIRST) == 0;
  long value = 0;
  *result = FOUND;

  if ((flags & OPERAND_DEFINES) && defines_first && lookup_define(name, &value)) {
    *kind = OPERAND_DEFINE;
    return value;
  }
  // Labels may be defined later in the file or in a later file, and pass 1
  // only needs sizes, so any name is accepted as 0 until layout is known.
  if (labels && (flags & OPERAND_DEFER_LABELS) && pass_number == 1) {
    *kind = OPERAND_DEFERRED;
    return 0;
  }
  if (labels && lookup_label(name, &value)) {
    *kind = OPERAND_LABEL;
    // PC-relative operands are measured from the next instruction (ISA.md).
    return (flags & OPERAND_PC_RELATIVE) ? value - (long)pc - 4 : value;
  }
  if ((flags & OPERAND_DEFINES) && !defines_first && lookup_define(name, &value)) {
    *kind = OPERAND_DEFINE;
    return value;
  }

  print_error();
  if (context == NULL) {
    fprintf(stderr, "Label \"");
  } else {
    fprintf(stderr, "%s %s \"", context, labels ? "constant/label" : "constant");
  }
  print_slice_err(name);
  fprintf(stderr, "\" has not been defined\n");
  *kind = OPERAND_UNDEFINED;
  *result = ERROR;
  return 0;
}

// Parse an integer literal or a name and resolve it per `flags`.
// NOT_FOUND (cursor unchanged) when neither is present. kind_out may be NULL.
long consume_operand(unsigned flags, const char* context, enum ConsumeResult* result,
                     enum OperandKind* kind_out) {
  enum OperandKind kind = OPERAND_LITERAL;
  long value = consume_literal(result);
  struct Slice name;
  if (*result == NOT_FOUND && consume_identifier(&name)) {
    value = resolve_operand_name(&name, flags, context, result, &kind);
  }
  if (kind_out != NULL) *kind_out = kind;
  return value;
}

// Parse a literal or .define constant; labels are not allowed.
static long consume_constant(enum ConsumeResult* result, const char* context) {
  return consume_operand(OPERAND_DEFINES, context, result, NULL);
}

// consume a literal immediate or label immediate
// Labels are PC-relative. Unlike every directive operand, a label here takes
// precedence over a .define of the same name; this predates the shared
// resolver and is preserved so existing programs encode identically.
long consume_immediate(enum ConsumeResult* result, enum OperandKind* kind){
  return consume_operand(OPERAND_INSTRUCTION, NULL, result, kind);
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

  // Labels are looked up as they stand at this point in pass 1.
  enum ConsumeResult result;
  enum OperandKind kind;
  long imm = consume_operand(OPERAND_DEFINES | OPERAND_LABELS, NULL, &result, &kind);
  if (result == NOT_FOUND || (result == ERROR && kind == OPERAND_LITERAL)){
    print_error();
    fprintf(stderr, "Expected integer literal or label\n");
    *success = false;
    return;
  }
  if (result != FOUND){
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
        long imm = consume_constant(&result, ".origin");
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
        consume_operand(OPERAND_DATA, ".fill", &result, NULL);
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
        consume_constant(&result, ".fild");
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
        consume_constant(&result, ".filb");
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
        long imm = consume_constant(&result, ".space");
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
        long imm = consume_constant(&result, ".origin");
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
      long imm = consume_operand(OPERAND_DATA, ".fill", &result, NULL);
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
      long imm = consume_constant(&result, ".fild");
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
      long imm = consume_constant(&result, ".filb");
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
      long imm = consume_constant(&result, ".space");
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
