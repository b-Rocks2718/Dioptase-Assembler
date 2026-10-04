#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "slice.h"
#include "preprocessor.h"
#include "lexer.h"
#include "keyword.h"

/*
  Preprocessor: copies each source file into a fresh buffer with '#' comments
  removed and pseudo-ops (nop, ret, push/pop, movi, mov, call) rewritten as
  real instructions. The lexer cursor (`current`, `line_count`) is shared with
  the assembler so operand parsing and error reporting are identical.
*/

static size_t result_index;

static char* result;
static size_t capacity; // using a dynamic array here

// Double the output buffer. On failure the old buffer is kept so the caller
// can still free it.
static bool expand_capacity(void){
  char* grown = realloc(result, 2 * capacity);
  if (grown == NULL) {
    fprintf(stderr, "Assembler preprocessor: failed to grow output for %s to %zu bytes\n",
            current_file, 2 * capacity);
    return false;
  }
  result = grown;
  capacity = 2 * capacity;
  return true;
}

// Ensure `extra` more bytes fit while still leaving room for one copied
// source byte and the terminating NUL.
static bool reserve_output(size_t extra){
  while (result_index + extra + 2 >= capacity) {
    if (!expand_capacity()) return false;
  }
  return true;
}

// Append printf-formatted text to the output buffer.
static bool emit(const char* fmt, ...){
  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (len < 0 || !reserve_output((size_t)len)) return false;

  va_start(args, fmt);
  vsnprintf(result + result_index, capacity - result_index, fmt, args);
  va_end(args);
  result_index += (size_t)len;
  return true;
}

// remove single line # comments
static bool skip_comments(void){
  if (*current == '#'){
    while (*current != '\n') {
      if (*current == '\0') return false;
      current++;
    }
  }
  return true;
}

// Report a malformed pseudo-op operand at the current source line.
static bool macro_error(const char* message){
  print_error();
  fprintf(stderr, "%s", message);
  return false;
}

// Parse the general-purpose register operand every register pseudo-op takes.
// Returns -1 after reporting the error.
static int expect_macro_register(void){
  int reg = consume_register();
  if (reg == -1) macro_error("Invalid register\nValid registers are r0 - r31\n");
  return reg;
}

// Parse a general or control register for mov. *is_control reports which
// kind matched. Returns -1 after reporting the error.
static int expect_mov_register(bool* is_control){
  *is_control = false;
  int reg = consume_register();
  if (reg != -1) return reg;
  reg = consume_control_register();
  if (reg != -1) {
    *is_control = true;
    return reg;
  }
  macro_error("Invalid register\nValid registers are r0 - r31\n");
  return -1;
}

// Stack pseudo-ops: one register operand, one pre-decrement store or
// post-increment load of the matching width (ISA.md absolute memory forms).
static const struct {
  enum KeywordId id;
  const char* format;
} kStackMacros[] = {
  {KW_PUSH, "swa  r%d [sp, -4]!"},
  {KW_PSHW, "swa  r%d [sp, -4]!"},
  {KW_POP,  "lwa  r%d, [sp], 4"},
  {KW_POPW, "lwa  r%d, [sp], 4"},
  {KW_PSHD, "sda  r%d [sp, -2]!"},
  {KW_POPD, "lda  r%d, [sp], 2"},
  {KW_PSHB, "sba  r%d [sp, -1]!"},
  {KW_POPB, "lba  r%d, [sp], 1"},
};

// Expand a stack pseudo-op whose keyword has already been consumed.
static bool expand_stack_macro(const char* format){
  int ra = expect_macro_register();
  if (ra == -1) return false;
  return emit(format, ra);
}

// Expand movi into movu/movl. A numeric immediate is split here; a label is
// passed through so movu/movl can resolve it during assembly.
static bool expand_movi(void){
  int ra = expect_macro_register();
  if (ra == -1) return false;

  enum ConsumeResult c_result;
  long imm = consume_literal(&c_result);
  if (c_result == FOUND) {
    return emit("movu r%d, 0x%X; movl r%d, 0x%X", ra, (unsigned)imm, ra, (unsigned)imm);
  }
  struct Slice label;
  if (!consume_identifier(&label)) return macro_error("Expected immediate\n");
  return emit("movu r%d, %.*s; movl r%d, %.*s",
              ra, (int)label.len, label.start, ra, (int)label.len, label.start);
}

// Expand mov: general-to-general becomes add with r0; any control register
// operand becomes crmv.
static bool expand_mov(void){
  bool a_is_control;
  bool b_is_control;
  int ra = expect_mov_register(&a_is_control);
  if (ra == -1) return false;
  int rb = expect_mov_register(&b_is_control);
  if (rb == -1) return false;

  if (!a_is_control && !b_is_control) return emit("add  r%d, r%d, r0", ra, rb);
  return emit("crmv %sr%d, %sr%d",
              a_is_control ? "c" : "", ra, b_is_control ? "c" : "", rb);
}

// Expand call into an absolute address load into ra (r29) and a branch-and-link
// through it, per abi.md's return-address register.
static bool expand_call(void){
  enum ConsumeResult c_result;
  long imm = consume_literal(&c_result);
  if (c_result == FOUND) {
    return emit("movu r29, 0x%X; movl r29, 0x%X; br r29, r29", (unsigned)imm, (unsigned)imm);
  }
  struct Slice label;
  if (!consume_identifier(&label)) return macro_error("Expected immediate\n");
  return emit("movu r29, %.*s; movl r29, %.*s; br r29, r29",
              (int)label.len, label.start, (int)label.len, label.start);
}

// Expand the pseudo-op at `current`, if any. Returns false on a malformed
// operand or allocation failure; both are already reported.
static bool expand_macros(void){
  bool success = true;
  // take_keyword rejects non-pseudo lead bytes before scanning the token.
  enum KeywordId id = take_keyword(KW_CLASS_PSEUDO);
  switch (id) {
    case KW_NONE: return true;
    case KW_NOP: success = emit("and  r0, r0, r0"); break;
    case KW_RET: success = emit("jmp  r29"); break;
    case KW_MOVI: success = expand_movi(); break;
    case KW_MOV: success = expand_mov(); break;
    case KW_CALL: success = expand_call(); break;
    default:
      for (size_t i = 0; i < sizeof(kStackMacros) / sizeof(kStackMacros[0]); ++i) {
        if (kStackMacros[i].id == id) {
          success = expand_stack_macro(kStackMacros[i].format);
          break;
        }
      }
      break;
  }

  if (!success) fprintf(stderr, "Preprocesser macro error\n");

  return success;
}

// Free the finished outputs, the result table, and the in-progress buffer
// after a failure partway through file `count`.
static void free_partial_results(char** result_list, int count){
  for (int j = 0; j < count; ++j) free(result_list[j]);
  free(result_list);
  free(result);
  result = NULL;
}

// copy the program into a new string, but without the comments
// expand macros into real instructions
char** preprocess(int num_files, const char* const* paths, const char* const* files){

  char ** result_list = malloc(num_files * sizeof(*result_list));
  if (result_list == NULL) {
    fprintf(stderr, "Assembler preprocessor: failed to allocate results for %d input files\n", num_files);
    return NULL;
  }

  for (int i = 0; i < num_files; ++i){

    // initialize parser
    current = files[i];
    current_buffer_start = current;
    line_count = 1;
    result_index = 0;
    current_file = paths[i];

    // Size the output from the input so macro expansion does not recopy the
    // buffer from a 60-byte seed. One extra byte holds the leading NUL the
    // assembler uses as a sentinel, and another holds the terminating NUL.
    size_t src_len = strlen(current);
    enum { kMinPreprocessCapacity = 64 };
    capacity = src_len + 2;
    if (capacity < kMinPreprocessCapacity) capacity = kMinPreprocessCapacity;

    result = malloc(sizeof(char) * capacity);
    if (result == NULL) {
      // Earlier files and the result table remain owned here until success.
      fprintf(stderr, "Assembler preprocessor: failed to allocate output for %s\n", current_file);
      free_partial_results(result_list, i);
      return NULL;
    }

    // initial null used to detect start of program
    // used when printing errors
    result[result_index] = '\0';
    result_index++;

    while (*current != '\0'){
      // skip comments, exit if EOF is reached
      if (!skip_comments()) break;

      if (!reserve_output(0) || !expand_macros()) {
        free_partial_results(result_list, i);
        return NULL;
      }
      // A pseudo-op can be the last token in a file with no trailing newline.
      if (*current == '\0') break;

      // write one character, then repeat loop
      result[result_index] = *current;
      if (*current == '\n') line_count++;
      result_index++;
      current++;
    }

    // include null terminator, reserve_output keeps room for it
    result[result_index] = 0;
    result_list[i] = result;
    result = NULL;
  }

  return result_list;
}
