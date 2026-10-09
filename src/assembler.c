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
#include "elf.h"
#include "debug.h"
#include "keyword.h"
#include "encode.h"
#include "charclass.h"

/*
  Two-pass assembler.

  Pass 1 walks every file to define labels and measure each section; labels
  are stored as packed (section, offset) pairs because section addresses are
  not known yet. Layout then assigns every section its address, labels are
  rewritten to absolute addresses, and pass 2 walks the files again with the
  same statement handlers, this time emitting bytes.

  Both passes run the same code (assemble_file and the handle_* functions),
  so they always agree on section offsets. Validation happens in pass 1;
  pass 2 only adds what needs final addresses: label values, range checks on
  label-valued operands, emitted bytes, warnings, and debug records.
*/

unsigned long pc = 0;

// does the file wish to use pivileges instructions?
bool is_kernel = false;

static int pass_number = 1;
static int current_file_index;
static enum UserSection current_section = NO_SECTION;

// Layout and emission state for one output section.
struct Section {
  struct InstructionArray* words;  // pass 2 image; NULL for user-mode .bss
  uint32_t offset;     // bytes emitted so far in the current pass
  uint32_t size;       // total bytes, fixed after pass 1
  uint32_t base;       // image address assigned by layout
  uint32_t load_base;  // runtime address used for labels and pc; == base unless set by *_load
  bool load_set;       // load_base given by .text_load/.rodata_load/.data_load/.bss_load
};
static struct Section sections[SECTION_COUNT];

static struct DebugInfoList* debug_info_list = NULL;

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

// User programs load at USER_BASE_ADDR with page-aligned text/rodata/data
// (syntax doc "ELF layout"); kernel sections are padded to 512-byte blocks.
#define USER_BASE_ADDR 0x80000000u
#define SECTION_ALIGN 0x1000u
static const uint32_t kKernelSectionAlign = 512;
// Written to the kernel end section so the padded .bss size can be computed.
static const uint32_t kKernelEndSentinel = 0xAAAAAAAAu;

// Round a value up to the requested alignment.
static uint32_t align_up(uint32_t value, uint32_t align) {
  uint32_t rem = value % align;
  if (rem == 0) return value;
  return value + (align - rem);
}

// Check whether a section index is valid for the active mode.
// Returns true when section is usable in the current mode.
static bool is_section_in_range(enum UserSection section){
  if (is_kernel){
    return section >= TEXT_SECTION && section <= IMPLICIT_SECTION;
  }
  return section >= TEXT_SECTION && section <= BSS_SECTION;
}

// Recompute pc for the current section. Computed in 64 bits so that the
// pass 2 address-space check sees a section running past 4 GiB.
static void update_pc(void){
  struct Section* sec = &sections[current_section];
  pc = (unsigned long)sec->load_base + sec->offset;
}

// Advance the current section by `count` bytes. In pass 2 the bytes are also
// written to the section image: `bytes`, or zeros when bytes is NULL. .bss
// has no image, so only its offset grows.
static void emit_bytes(const uint8_t* bytes, uint32_t count){
  struct Section* sec = &sections[current_section];
  if (pass_number == 2 && current_section != BSS_SECTION){
    for (uint32_t i = 0; i < count; ++i){
      instruction_array_append_byte(sec->words, bytes != NULL ? bytes[i] : 0,
                                    (int)(sec->base + sec->offset + i));
    }
  }
  sec->offset += count;
  update_pc();
}

// Emit one aligned instruction word.
static void emit_word(int word){
  struct Section* sec = &sections[current_section];
  if (pass_number == 2) instruction_array_append(sec->words, word);
  sec->offset += kWordBytes;
  update_pc();
}

// Encode the least-significant bytes of value in little-endian order.
// value is the integer to encode; out must have space for count bytes; count is 1, 2, or 4.
static void encode_value_bytes(uint32_t value, uint8_t* out, uint32_t count){
  for (uint32_t i = 0; i < count; ++i){
    out[i] = (uint8_t)(value >> (8 * i));
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

// Report a directive that emits data being used in .bss, which has no image.
static bool reject_in_bss(const char* what){
  if (current_section != BSS_SECTION) return true;
  print_error();
  fprintf(stderr, "%s not allowed in .bss section\n", what);
  return false;
}

// Pack a section index and offset for resolution after layout.
static long encode_section_offset(enum UserSection section, uint32_t offset) {
  return (long)(((uint64_t)section << 32) | offset);
}

// Replace packed section offsets in a label map with absolute addresses.
static void adjust_label_map_for_sections(struct HashMap* map) {
  for (size_t i = 0; i < map->size; ++i){
    for (struct HashEntry* entry = map->arr[i]; entry != NULL; entry = entry->next){
      if (!entry->is_defined) continue;
      uint64_t raw = (uint64_t)entry->value;
      enum UserSection section = (enum UserSection)(raw >> 32);
      uint32_t offset = (uint32_t)(raw & 0xFFFFFFFFu);
      entry->value = (long)(sections[section].load_base + offset);
    }
  }
}

// Forward declaration for alignment parsing helpers.
static long consume_constant(enum ConsumeResult* result, const char* context);

// Check whether a value is a power-of-two alignment.
// Returns true when value is a nonzero power of two.
static bool is_power_of_two_u32(uint32_t value){
  return value != 0 && (value & (value - 1)) == 0;
}

// Parse a literal or .define operand for `directive`, reporting a missing one.
static bool parse_constant_operand(const char* directive, const char* expected, long* value){
  enum ConsumeResult result;
  *value = consume_constant(&result, directive);
  if (result == FOUND) return true;
  if (result == NOT_FOUND){
    print_error();
    fprintf(stderr, "Invalid %s %s; expected integer literal or .define constant\n", directive, expected);
  }
  return false;
}

// Parse and validate a byte alignment value for .align.
// Returns true on success and fills alignment_out.
static bool parse_alignment(const char* directive, uint32_t* alignment_out){
  long imm;
  if (!parse_constant_operand(directive, "value", &imm)) return false;
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
// Pass 1 records the address; pass 2 re-parses it and checks it still agrees.
static bool parse_section_load_directive(enum UserSection section, const char* directive){
  if (!is_kernel){
    print_error();
    fprintf(stderr, "%s can only be used in kernel mode\n", directive);
    return false;
  }
  long imm;
  if (!parse_constant_operand(directive, "value", &imm)) return false;
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

  struct Section* sec = &sections[section];
  if (pass_number == 1){
    if (sec->offset != 0){
      print_error();
      fprintf(stderr, "%s must appear before any content in that section\n", directive);
      return false;
    }
    if (sec->load_set && sec->load_base != addr){
      print_error();
      fprintf(stderr, "%s specified multiple times with different values\n", directive);
      return false;
    }
    sec->load_base = addr;
    sec->load_set = true;
  } else if (sec->load_set && sec->load_base != addr){
    print_error();
    fprintf(stderr, "%s value does not match first pass\n", directive);
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

// Split "NAME=value" into its name and integer value. The value is parsed
// with the assembler's literal syntax. Reports the problem and returns false
// when the definition is malformed.
static bool parse_cli_define(const char* def, struct Slice* name, long* value){
  const char* eq = strchr(def, '=');
  if (eq == NULL || eq == def || *(eq + 1) == '\0'){
    fprintf(stderr, "Invalid -D definition: %s (expected -DNAME=value)\n", def);
    return false;
  }
  name->start = def;
  name->len = (size_t)(eq - def);
  if (!is_valid_define_name(name->start, name->len)){
    fprintf(stderr, "Invalid -D name: %.*s\n", (int)name->len, name->start);
    return false;
  }

  // Run the lexer over the value text, then restore the cursor.
  const char* old_current = current;
  const char* old_buffer = current_buffer_start;
  unsigned old_line = line_count;
  const char* old_file = current_file;

  current = eq + 1;
  current_buffer_start = current;
  line_count = 1;
  current_file = "<command line>";

  enum ConsumeResult result;
  *value = consume_literal(&result);
  skip();
  bool ok = (result == FOUND) && (*current == '\0');

  current = old_current;
  current_buffer_start = old_buffer;
  line_count = old_line;
  current_file = old_file;

  if (!ok){
    fprintf(stderr, "Invalid -D value for %.*s\n", (int)name->len, name->start);
    return false;
  }
  return true;
}

// Validate and record the -DNAME=value definitions every file starts with.
// The strings must outlive assemble(); they are not copied.
bool set_cli_defines(int count, const char* const* defines){
  for (int i = 0; i < count; ++i){
    struct Slice name;
    long value;
    if (!parse_cli_define(defines[i], &name, &value)) return false;
    for (int j = 0; j < i; ++j){
      struct Slice earlier = {defines[j], (size_t)(strchr(defines[j], '=') - defines[j])};
      if (compare_slice_to_slice(&earlier, &name)){
        fprintf(stderr, "Duplicate -D definition for %.*s\n", (int)name.len, name.start);
        return false;
      }
    }
  }
  cli_define_count = count;
  cli_defines = defines;
  return true;
}

// Insert the validated command-line definitions into this file's .define map.
static void apply_cli_defines(void){
  for (int i = 0; i < cli_define_count; ++i){
    struct Slice name;
    long value;
    bool ok = parse_cli_define(cli_defines[i], &name, &value);
    assert(ok && "set_cli_defines validated every definition");
    (void)ok;
    hash_map_insert(local_defines[current_file_index], &name, value, true, true);
  }
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

// Parse an optional `+ literal` or `- literal` after a name, on the same line
// with only spaces or tabs between (so a separator such as ',' or ';' never
// joins two operands). Returns the signed offset, or 0 with the cursor
// unchanged when no '+' or '-' follows. A sign without a literal is an error.
static long consume_name_offset(const struct Slice* name, enum ConsumeResult* result) {
  const char* start = current;
  while (*current == ' ' || *current == '\t') current++;
  bool negate = *current == '-';
  if (!negate && *current != '+') {
    current = start;
    return 0;
  }
  current++;
  enum ConsumeResult literal_result;
  long offset = consume_literal(&literal_result);
  if (literal_result != FOUND) {
    if (literal_result == NOT_FOUND) {
      print_error();
      fprintf(stderr, "Expected integer literal after '%c' in offset for \"", negate ? '-' : '+');
      print_slice_err(name);
      fprintf(stderr, "\"\n");
    }
    *result = ERROR;
    return 0;
  }
  return negate ? -offset : offset;
}

// Parse an integer literal, or a name with an optional `+ imm` / `- imm`
// offset, and resolve it per `flags`. The offset is added to the name's
// value, so a PC-relative label operand encodes (label + imm) - (pc + 4).
// NOT_FOUND (cursor unchanged) when neither is present. kind_out may be NULL.
long consume_operand(unsigned flags, const char* context, enum ConsumeResult* result,
                     enum OperandKind* kind_out) {
  enum OperandKind kind = OPERAND_LITERAL;
  long value = consume_literal(result);
  struct Slice name;
  if (*result == NOT_FOUND && consume_identifier(&name)) {
    value = resolve_operand_name(&name, flags, context, result, &kind);
    if (*result == FOUND) {
      value += consume_name_offset(&name, result);
    }
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
static void record_define(bool* success){
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

// ---- Statement handlers (shared by both passes) ---------------------------

// Define a label at the current position (pass 1). Pass 2 has nothing to do.
static bool handle_label(const struct Slice* label){
  if (pass_number == 2) return true;
  if (!ensure_valid_section("label")) return false;
  long label_value = encode_section_offset(current_section, sections[current_section].offset);

  struct HashMap* locals = local_labels[current_file_index];
  if (label_has_definition(locals, label)){
    print_error();
    fprintf(stderr, "Duplicate label\n");
    return false;
  }
  hash_map_insert(locals, label, label_value, true, current_section != TEXT_SECTION);

  // A label declared .global earlier in this file also defines the export.
  if (hash_map_contains(local_globals[current_file_index], label)){
    if (label_has_definition(global_labels, label)){
      print_error();
      fprintf(stderr, "Duplicate global label\n");
      return false;
    }
    make_defined(global_labels, label, label_value);
  }
  return true;
}

// .global NAME: export NAME. Pass 1 records the declaration (and the
// definition, if NAME is already defined in this file); pass 2 reports a
// declaration that no file defined.
static bool handle_global(void){
  struct Slice label;
  if (!consume_identifier(&label)){
    print_error();
    fprintf(stderr, ".global directive requires a label\n");
    return false;
  }

  if (pass_number == 2){
    if (label_has_definition(global_labels, &label)) return true;
    print_error();
    fprintf(stderr, "Global label \"");
    print_slice_err(&label);
    fprintf(stderr, "\" is declared .global but never defined\n");
    return false;
  }

  // Track per-file global declarations to detect duplicate exports.
  bool is_data = current_section != TEXT_SECTION;
  if (!hash_map_contains(local_globals[current_file_index], &label)){
    hash_map_insert(local_globals[current_file_index], &label, 0, false, is_data);
  }
  if (!hash_map_contains(global_labels, &label)){
    hash_map_insert(global_labels, &label, 0, false, is_data);
  }

  struct HashMap* locals = local_labels[current_file_index];
  if (label_has_definition(locals, &label)){
    if (label_has_definition(global_labels, &label)){
      print_error();
      fprintf(stderr, "Duplicate global label\n");
      return false;
    }
    make_defined(global_labels, &label, hash_map_get(locals, &label));
  }
  return true;
}

// .origin ADDR (kernel, implicit section only): pad forward to ADDR.
static bool handle_origin(void){
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

  long imm;
  if (!parse_constant_operand(".origin", "value", &imm)) return false;
  uint32_t offset = sections[current_section].offset;
  if (imm < (long)offset){
    print_error();
    fprintf(stderr, ".origin cannot be used to go backwards\n");
    return false;
  } else if (imm >= ((long)1 << 32)){
    print_error();
    fprintf(stderr, ".origin address must be a 32 bit integer\n");
    return false;
  }
  emit_bytes(NULL, (uint32_t)imm - offset);
  return true;
}

// Data-emitting directives: width and accepted range of each.
struct FillDirective {
  const char* name;
  uint32_t bytes;
  unsigned operand_flags;   // .fill also accepts labels (absolute address)
  long min;                 // values from min to max are accepted, so both
  long max;                 // signed and unsigned spellings fit
  const char* missing;      // message when no operand is present
  const char* range;        // message when the value does not fit
};

static const struct FillDirective kFill = {
  ".fill", 4, OPERAND_DATA, -(1L << 31), (1L << 32) - 1,
  "Invalid .fill immediate; expected integer literal, label, or .define constant",
  ".fill immediate must fit in a 32-bit value",
};
static const struct FillDirective kFild = {
  ".fild", 2, OPERAND_DEFINES, -(1L << 15), (1L << 16) - 1,
  "Invalid .fild immediate; expected integer literal or .define constant",
  ".fild immediate must fit in a 16-bit value",
};
static const struct FillDirective kFilb = {
  ".filb", 1, OPERAND_DEFINES, -(1L << 7), (1L << 8) - 1,
  "Invalid .filb immediate; expected integer literal or .define constant",
  ".filb immediate must fit in an 8-bit value",
};

// .fill/.fild/.filb VALUE: emit VALUE little-endian in the directive's width.
static bool handle_fill(const struct FillDirective* fill){
  enum ConsumeResult result;
  long imm = consume_operand(fill->operand_flags, fill->name, &result, NULL);
  if (result != FOUND){
    if (result == NOT_FOUND){
      print_error();
      fprintf(stderr, "%s\n", fill->missing);
    }
    return false;
  }
  if (!ensure_valid_section(fill->name)) return false;
  if (!reject_in_bss(fill->name)) return false;
  if (imm < fill->min || imm > fill->max){
    print_error();
    fprintf(stderr, "%s\n", fill->range);
    return false;
  }

  if (pass_number == 2 && current_section == TEXT_SECTION){
    char message[32];
    snprintf(message, sizeof(message), "%s used in .text section", fill->name);
    print_warning(message);
  }
  uint8_t bytes[4];
  encode_value_bytes((uint32_t)imm, bytes, fill->bytes);
  emit_bytes(bytes, fill->bytes);
  return true;
}

// .space N: N zero bytes (only reserved, not emitted, in .bss).
static bool handle_space(void){
  long imm;
  if (!parse_constant_operand(".space", "count", &imm)) return false;
  if (!ensure_valid_section(".space")) return false;
  if (imm < 0 || imm >= ((long)1 << 32)){
    print_error();
    fprintf(stderr, ".space immediate must be a positive 32 bit integer\n");
    return false;
  }
  emit_bytes(NULL, (uint32_t)imm);
  return true;
}

// .align N: zero-pad to the next multiple of N (a power of two).
static bool handle_align(void){
  uint32_t alignment = 0;
  if (!parse_alignment(".align", &alignment)) return false;
  if (!ensure_valid_section(".align")) return false;
  uint32_t offset = sections[current_section].offset;
  emit_bytes(NULL, align_up(offset, alignment) - offset);
  return true;
}

// .line FILE N: map the next emitted address to a source line (for -g).
static bool handle_line(void){
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
  if (pass_number == 2 && debug_info_list != NULL) {
    add_debug_line(debug_info_list, &filename, (int)line_num, (uint32_t)pc);
  }
  return true;
}

// .local NAME BP_OFFSET SIZE: a source variable visible from the next address (for -g).
static bool handle_local(void){
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
  if (pass_number == 2 && debug_info_list != NULL) {
    add_debug_local(debug_info_list, &varname, (int)bp_offset, (size_t)size_value, (uint32_t)pc);
  }
  return true;
}

// Switch the current section (.text/.rodata/.data/.bss).
static bool select_section(enum UserSection section){
  current_section = section;
  update_pc();
  return true;
}

// Handle the directive `dir`, whose keyword has already been consumed.
static bool handle_directive(enum KeywordId dir){
  switch (dir) {
    case KW_DIR_GLOBAL: return handle_global();
    case KW_DIR_ORIGIN: return handle_origin();
    case KW_DIR_TEXT: return select_section(TEXT_SECTION);
    case KW_DIR_RODATA: return select_section(RODATA_SECTION);
    case KW_DIR_DATA: return select_section(DATA_SECTION);
    case KW_DIR_BSS: return select_section(BSS_SECTION);
    case KW_DIR_TEXT_LOAD: return parse_section_load_directive(TEXT_SECTION, ".text_load");
    case KW_DIR_RODATA_LOAD: return parse_section_load_directive(RODATA_SECTION, ".rodata_load");
    case KW_DIR_DATA_LOAD: return parse_section_load_directive(DATA_SECTION, ".data_load");
    case KW_DIR_BSS_LOAD: return parse_section_load_directive(BSS_SECTION, ".bss_load");
    case KW_DIR_FILL: return handle_fill(&kFill);
    case KW_DIR_FILD: return handle_fill(&kFild);
    case KW_DIR_FILB: return handle_fill(&kFilb);
    case KW_DIR_SPACE: return handle_space();
    case KW_DIR_ALIGN: return handle_align();
    case KW_DIR_LINE: return handle_line();
    case KW_DIR_LOCAL: return handle_local();
    case KW_DIR_DEFINE:
      // .define is evaluated once, in pass 1, so later labels are never seen.
      // Pass 2 consumes the same tokens and ignores them; pass 1 already
      // proved they resolve.
      if (pass_number == 2) {
        struct Slice name;
        enum ConsumeResult result;
        consume_identifier(&name);
        consume_operand(OPERAND_DEFINES | OPERAND_LABELS, NULL, &result, NULL);
        return true;
      } else {
        bool success = true;
        record_define(&success);
        return success;
      }
    default:
      assert(!"take_keyword returned a non-directive id");
      return false;
  }
}

// Assemble one instruction into the current section.
static bool handle_instruction(void){
  if (!ensure_valid_section("instruction")) return false;
  if (current_section == BSS_SECTION){
    print_error();
    fprintf(stderr, "Instructions not allowed in .bss section\n");
    return false;
  }
  uint32_t offset = sections[current_section].offset;
  if (offset % kWordBytes != 0){
    print_error();
    fprintf(stderr, "Instruction address must be %u-byte aligned; section offset is 0x%08X\n",
            kWordBytes, offset);
    return false;
  }

  update_pc();
  enum ConsumeResult result = FOUND;
  int instruction = consume_instruction(&result);
  if (result == ERROR) return false;
  if (result == NOT_FOUND) {
    print_error();
    fprintf(stderr, "Unrecognized instruction\n");
    return false;
  }

  if (pass_number == 2){
    if (current_section == RODATA_SECTION){
      print_warning("Instruction emitted in .rodata section");
    } else if (current_section == DATA_SECTION){
      print_warning("Instruction emitted in .data section");
    }
  }
  emit_word(instruction);
  return true;
}

// Run the current pass over one preprocessed file. prog follows the NUL
// sentinel the preprocessor puts before every buffer.
static bool assemble_file(char const* const prog){
  current = prog;
  current_buffer_start = prog - 1;
  line_count = 1;

  while (!is_at_end()){
    if (pass_number == 2 && pc > ((unsigned long)1 << 32)){
      print_error();
      fprintf(stderr, "Program does not fit in 32-bit address space\n");
      return false;
    }

    struct Slice label;
    if (consume_label(&label)) {
      if (!handle_label(&label)) return false;
      continue;
    }

    skip();
    // One lookup classifies the directive. The token stays put when it is not one.
    enum KeywordId dir = take_keyword(KW_CLASS_DIRECTIVE);
    bool ok = (dir != KW_NONE) ? handle_directive(dir) : handle_instruction();
    if (!ok) return false;
  }
  return true;
}

// Create this file's symbol tables and seed them with -D definitions.
static bool create_file_symbols(char const* const prog){
  // Bucket counts track source size. Compiler output is roughly one label per
  // few dozen bytes; .define names and per-file .global sets stay small, so
  // those tables do not need a thousand empty buckets each.
  enum {
    kMinLabelBuckets = 32,
    kMaxLabelBuckets = 16384,
    kLabelBytesPerBucket = 48,
    kSparseSymbolBuckets = 32
  };
  size_t label_need = strlen(prog) / kLabelBytesPerBucket;
  if (label_need < kMinLabelBuckets) label_need = kMinLabelBuckets;
  if (label_need > kMaxLabelBuckets) label_need = kMaxLabelBuckets;
  size_t label_buckets = kMinLabelBuckets;
  while (label_buckets < label_need) label_buckets *= 2;

  local_labels[current_file_index] = create_hash_map(label_buckets);
  local_defines[current_file_index] = create_hash_map(kSparseSymbolBuckets);
  local_globals[current_file_index] = create_hash_map(kSparseSymbolBuckets);
  if (local_labels[current_file_index] == NULL ||
      local_defines[current_file_index] == NULL ||
      local_globals[current_file_index] == NULL) {
    return false;
  }
  apply_cli_defines();
  return true;
}

// Run one pass over every file in order. Section and pc state carry over
// from one file to the next, so a file continues in the previous file's section.
static bool run_pass(int pass, int num_files, const char* const* names, char** files){
  pass_number = pass;
  current_section = is_kernel ? IMPLICIT_SECTION : NO_SECTION;
  for (int i = 0; i < SECTION_COUNT; ++i) sections[i].offset = 0;
  pc = (pass == 1) ? 0 : sections[is_kernel ? IMPLICIT_SECTION : TEXT_SECTION].load_base;

  for (int i = 0; i < num_files; ++i){
    current_file_index = i;
    current_file = names[i];
    if (pass == 1 && !create_file_symbols(files[i] + 1)) return false;
    if (!assemble_file(files[i] + 1)) return false;
  }
  return true;
}

// ---- Layout ---------------------------------------------------------------

// Assign section addresses from the sizes measured in pass 1, then rewrite
// every label from a packed section offset to its absolute address.
static void layout_sections(int num_files){
  if (!is_kernel){
    // ELF segments: text at USER_BASE_ADDR, rodata and data each on the next
    // page, .bss directly after .data. elf.c's create_PHT uses the same rule.
    for (int i = TEXT_SECTION; i <= DATA_SECTION; ++i){
      sections[i].size = align_up(sections[i].offset, kWordBytes);
    }
    sections[BSS_SECTION].size = sections[BSS_SECTION].offset;
    sections[TEXT_SECTION].base = USER_BASE_ADDR;
    sections[RODATA_SECTION].base = align_up(sections[TEXT_SECTION].base + sections[TEXT_SECTION].size, SECTION_ALIGN);
    sections[DATA_SECTION].base = align_up(sections[RODATA_SECTION].base + sections[RODATA_SECTION].size, SECTION_ALIGN);
    sections[BSS_SECTION].base = sections[DATA_SECTION].base + sections[DATA_SECTION].size;
  } else {
    // Kernel image: implicit, text, rodata, data, bss, end, each padded to
    // a 512-byte block. The end section holds one sentinel word.
    for (int i = 0; i < SECTION_COUNT; ++i) sections[i].size = sections[i].offset;
    sections[END_SECTION].size = kWordBytes;
    static const enum UserSection kKernelOrder[] = {
      IMPLICIT_SECTION, TEXT_SECTION, RODATA_SECTION, DATA_SECTION, BSS_SECTION, END_SECTION,
    };
    uint32_t cursor = 0;
    for (size_t i = 0; i < sizeof(kKernelOrder) / sizeof(kKernelOrder[0]); ++i){
      struct Section* sec = &sections[kKernelOrder[i]];
      sec->base = cursor;
      cursor += align_up(sec->size, kKernelSectionAlign);
    }
  }

  // Runtime addresses default to image addresses unless a *_load directive
  // moved the section. With .bss_load, the end section follows the padded .bss.
  for (int i = 0; i < SECTION_COUNT; ++i){
    if (!sections[i].load_set) sections[i].load_base = sections[i].base;
  }
  if (is_kernel && sections[BSS_SECTION].load_set){
    sections[END_SECTION].load_base =
      sections[BSS_SECTION].load_base + align_up(sections[BSS_SECTION].size, kKernelSectionAlign);
  }

  for (int i = 0; i < num_files; ++i) adjust_label_map_for_sections(local_labels[i]);
  adjust_label_map_for_sections(global_labels);
}

// Create the pass 2 section images in output order. Capacities come from the
// pass 1 sizes so the arrays never regrow; .bss is never materialized.
static struct InstructionArrayList* create_section_images(void){
  static const enum UserSection kUserImages[] = { TEXT_SECTION, RODATA_SECTION, DATA_SECTION };
  static const enum UserSection kKernelImages[] = {
    IMPLICIT_SECTION, TEXT_SECTION, RODATA_SECTION, DATA_SECTION, BSS_SECTION, END_SECTION,
  };
  const enum UserSection* order = is_kernel ? kKernelImages : kUserImages;
  size_t count = is_kernel ? sizeof(kKernelImages) / sizeof(kKernelImages[0])
                           : sizeof(kUserImages) / sizeof(kUserImages[0]);

  struct InstructionArrayList* list = create_instruction_array_list();
  for (size_t i = 0; i < count; ++i){
    struct Section* sec = &sections[order[i]];
    size_t words = (order[i] == BSS_SECTION) ? 1 : (sec->size + kWordBytes - 1) / kWordBytes;
    if (words == 0) words = 1;
    sec->words = create_instruction_array(words, (int)sec->base);
    if (sec->words == NULL){
      fprintf(stderr, "Assembler: failed to allocate %zu words for section image at 0x%08X\n",
              words, sec->base);
      destroy_instruction_array_list(list);
      return NULL;
    }
    instruction_array_list_append(list, sec->words);
  }
  return list;
}

// ---- Driver ---------------------------------------------------------------

// Append labels from a definition map to the output label list.
static void append_labels_from_map(struct HashMap* map, struct LabelList* labels){
  for (size_t i = 0; i < map->size; ++i){
    for (struct HashEntry* entry = map->arr[i]; entry != NULL; entry = entry->next){
      if (entry->is_defined){
        label_list_append(labels, entry->key.start, entry->key.len, (uint32_t)entry->value, entry->is_data);
      }
    }
  }
}

// Free every symbol table. Tables that were never created are NULL.
static void destroy_symbol_tables(int num_files){
  for (int j = 0; j < num_files; ++j){
    if (local_labels != NULL) destroy_hash_map(local_labels[j]);
    if (local_defines != NULL) destroy_hash_map(local_defines[j]);
    if (local_globals != NULL) destroy_hash_map(local_globals[j]);
  }
  free(local_labels);
  free(local_defines);
  free(local_globals);
  destroy_hash_map(global_labels);
  local_labels = local_defines = local_globals = NULL;
  global_labels = NULL;
}

// assemble an entire program
struct ProgramDescriptor* assemble(int num_files, const char* const* paths, char** files,
  bool kernel, struct LabelList** labels_out, struct DebugInfoList** labels_out_c){

  is_kernel = kernel;
  memset(sections, 0, sizeof(sections));
  if (labels_out != NULL) *labels_out = NULL;
  if (labels_out_c != NULL) *labels_out_c = NULL;

  // Source-line records are only retained when the caller asked for -g output.
  debug_info_list = (labels_out_c != NULL) ? create_debug_info_list() : NULL;

  local_labels = calloc((size_t)num_files, sizeof(struct HashMap*));
  local_defines = calloc((size_t)num_files, sizeof(struct HashMap*));
  local_globals = calloc((size_t)num_files, sizeof(struct HashMap*));
  // Shared export table. Kernel images export more symbols than any one file.
  enum { kGlobalSymbolBuckets = 4096 };
  global_labels = create_hash_map(kGlobalSymbolBuckets);

  struct InstructionArrayList* images = NULL;
  struct ProgramDescriptor* program = NULL;
  uint32_t entry_point = 0;
  if (local_labels == NULL || local_defines == NULL ||
      local_globals == NULL || global_labels == NULL) {
    fprintf(stderr, "Assembler: failed to allocate symbol tables for %d files\n", num_files);
    goto done;
  }
  if (!run_pass(1, num_files, paths, files)) goto done;
  layout_sections(num_files);

  if (!is_kernel){
    struct Slice start_label = {"_start", 6};
    if (!label_has_definition(global_labels, &start_label)){
      fprintf(stderr, "Missing global label _start\n");
      goto done;
    }
    entry_point = (uint32_t)hash_map_get(global_labels, &start_label);
  }

  images = create_section_images();
  if (images == NULL || !run_pass(2, num_files, paths, files)) goto done;

  if (is_kernel){
    current_section = END_SECTION;
    uint8_t bytes[4];
    encode_value_bytes(kKernelEndSentinel, bytes, kWordBytes);
    emit_bytes(bytes, kWordBytes);
  }

  if (labels_out != NULL){
    struct LabelList* labels = create_label_list(128);
    for (int j = 0; j < num_files; ++j) append_labels_from_map(local_labels[j], labels);
    *labels_out = labels;
  }

  program = malloc(sizeof(struct ProgramDescriptor));
  program->entry_point = entry_point;
  program->sections = images;
  program->bss_size = sections[BSS_SECTION].offset;
  images = NULL;

  if (labels_out_c != NULL) {
    *labels_out_c = debug_info_list;
    debug_info_list = NULL;
  }

done:
  destroy_symbol_tables(num_files);
  if (images != NULL) destroy_instruction_array_list(images);
  destroy_debug_info_list(debug_info_list);
  debug_info_list = NULL;
  return program;
}
