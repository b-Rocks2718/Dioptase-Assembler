#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "slice.h"
#include "preprocessor.h"
#include "assembler.h"

static size_t result_index;

static char* result;
static size_t capacity; // using a dynamic array here

// expand dynamic array
bool expand_capacity(void){
  // resize
  result = realloc(result, 2 * capacity);
  if (result == NULL) {
    fprintf(stderr, "Preprocesser memory error\n");
    return false;
  }
  capacity = 2 * capacity;
  return true;
}

// expand dynamic array if necessary
bool check_capacity(void){
  // leave room for null terminator
  if (result_index >= capacity - 2) return expand_capacity();
  return true;
}

// remove single line # comments
bool skip_comments(void){
  if (*current == '#'){
    while (*current != '\n') {
      if (*current == '\0') return false;
      current++;
    }
  }
  return true;
}

void expand_nop(void){
  #define NOP_EXPANSION "and  r0, r0, r0"

  size_t expansion_len = strlen(NOP_EXPANSION) + 1; // account for null
  while (result_index + expansion_len >= capacity - 2) expand_capacity();
  result_index += sprintf(result + result_index, NOP_EXPANSION);
}

void expand_ret(void){
  #define RET_EXPANSION "jmp  r29"

  size_t expansion_len = strlen(RET_EXPANSION) + 1; // account for null
  while (result_index + expansion_len >= capacity - 2) expand_capacity();
  result_index += sprintf(result + result_index, RET_EXPANSION);
}

void expand_push(bool* success){
  #define PUSH_EXPANSION "swa  r%d [sp, -4]!"

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return;
  }

  size_t expansion_len = strlen(PUSH_EXPANSION) + 2; // account for null
  while (result_index + expansion_len >= capacity - 2) expand_capacity();
  result_index += sprintf(result + result_index, PUSH_EXPANSION, ra);
}

void expand_pop(bool* success){
  #define POP_EXPANSION "lwa  r%d, [sp], 4"

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return;
  }

  size_t expansion_len = strlen(POP_EXPANSION) + 2; // account for null
  while (result_index + expansion_len >= capacity - 2) expand_capacity();
  result_index += sprintf(result + result_index, POP_EXPANSION, ra);
}

void expand_pshd(bool* success){
  #define PSHD_EXPANSION "sda  r%d [sp, -2]!"

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return;
  }

  size_t expansion_len = strlen(PSHD_EXPANSION) + 2; // account for null
  while (result_index + expansion_len >= capacity - 2) expand_capacity();
  result_index += sprintf(result + result_index, PSHD_EXPANSION, ra);
}

void expand_popd(bool* success){
  #define POPD_EXPANSION "lda  r%d, [sp], 2"

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return;
  }

  size_t expansion_len = strlen(POPD_EXPANSION) + 2; // account for null
  while (result_index + expansion_len >= capacity - 2) expand_capacity();
  result_index += sprintf(result + result_index, POPD_EXPANSION, ra);
}

void expand_pshb(bool* success){
  #define PSHB_EXPANSION "sba  r%d [sp, -1]!"

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return;
  }

  size_t expansion_len = strlen(PSHB_EXPANSION) + 2; // account for null
  while (result_index + expansion_len >= capacity - 2) expand_capacity();
  result_index += sprintf(result + result_index, PSHB_EXPANSION, ra);
}

void expand_popb(bool* success){
  #define POPB_EXPANSION "lba  r%d, [sp], 1"

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return;
  }

  size_t expansion_len = strlen(POPB_EXPANSION) + 2; // account for null
  while (result_index + expansion_len >= capacity - 2) expand_capacity();
  result_index += sprintf(result + result_index, POPB_EXPANSION, ra);
}

void expand_movi(bool* success){
  #define MOVI_EXPANSION_LIT "movu r%d, 0x%X; movl r%d, 0x%X"
  #define MOVI_EXPANSION_LBL_1 "movu r%d, "
  #define MOVI_EXPANSION_LBL_2 "; movl r%d, "

  int ra = consume_register();
  if (ra == -1){
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
    return;
  }

  enum ConsumeResult c_result;
  long imm = consume_literal(&c_result);
  if (c_result == FOUND){
    // was a number
    size_t expansion_len = strlen(MOVI_EXPANSION_LIT) + 40; // could be a big number
    while (result_index + expansion_len >= capacity - 2) expand_capacity();
    result_index += sprintf(result + result_index, MOVI_EXPANSION_LIT, 
      ra, (unsigned)imm, ra, (unsigned)imm);
  } else {
    // check if its a string/label
    struct Slice* label = consume_identifier();
    if (label != NULL){

      size_t expansion_len = 
        strlen(MOVI_EXPANSION_LBL_1) + strlen(MOVI_EXPANSION_LBL_2) + 2 * label->len + 2;
      while (result_index + expansion_len >= capacity - 2) expand_capacity();
      result_index += sprintf(result + result_index, MOVI_EXPANSION_LBL_1, ra);
      strncpy(result + result_index, label->start, label->len);
      result_index += label->len;
      result_index += sprintf(result + result_index, MOVI_EXPANSION_LBL_2, ra);
      strncpy(result + result_index, label->start, label->len);
      result_index += label->len;

      free(label);
    } else {
      // error
      print_error();
      fprintf(stderr, "Expected immediate\n");
      *success = false;
      return;
    }
  }
}

void expand_mov(bool* success){
  #define MOV_EXPANSION_USR "add  r%d, r%d, r0"
  #define MOV_EXPANSION_CR_1 "crmv r%d, cr%d"
  #define MOV_EXPANSION_CR_2 "crmv cr%d, r%d"
  #define MOV_EXPANSION_CR_3 "crmv cr%d, cr%d"

  int ra = consume_register();
  if (ra == -1){
    ra = consume_control_register();
    if (ra == -1){
      print_error();
      fprintf(stderr, "Invalid register\n");
      fprintf(stderr, "Valid registers are r0 - r31\n");
      *success = false;
      return;
    }
    int rb = consume_register();
    if (rb == -1){
      rb = consume_control_register();
      if (rb == -1){
        print_error();
        fprintf(stderr, "Invalid register\n");
        fprintf(stderr, "Valid registers are r0 - r31\n");
        *success = false;
        return;
      }
      size_t expansion_len = strlen(MOV_EXPANSION_CR_3) + 2; // account for null
      while (result_index + expansion_len >= capacity - 2) expand_capacity();
      result_index += sprintf(result + result_index, MOV_EXPANSION_CR_3, ra, rb);
      return;
    }
    size_t expansion_len = strlen(MOV_EXPANSION_CR_2) + 2; // account for null
    while (result_index + expansion_len >= capacity - 2) expand_capacity();
    result_index += sprintf(result + result_index, MOV_EXPANSION_CR_2, ra, rb);
    return;
  }

  int rb = consume_register();
  if (rb == -1){
    rb = consume_control_register();
    if (rb == -1){
      print_error();
      fprintf(stderr, "Invalid register\n");
      fprintf(stderr, "Valid registers are r0 - r31\n");
      *success = false;
      return;
    }

    size_t expansion_len = strlen(MOV_EXPANSION_CR_1) + 2; // account for null
    while (result_index + expansion_len >= capacity - 2) expand_capacity();
    result_index += sprintf(result + result_index, MOV_EXPANSION_CR_1, ra, rb);
    return;
  }

  size_t expansion_len = strlen(MOV_EXPANSION_USR) + 2; // account for null
  while (result_index + expansion_len >= capacity - 2) expand_capacity();
  result_index += sprintf(result + result_index, MOV_EXPANSION_USR, ra, rb);
  return;
}

void expand_call(bool* success){
  // immediates can be numbers or labels

  #define CALL_EXPANSION_LIT "movu r29, 0x%X; movl r29, 0x%X; br r29, r29"

  #define CALL_EXPANSION_LBL_1 "movu r29, "
  #define CALL_EXPANSION_LBL_2 "; movl r29, "
  #define CALL_EXPANSION_LBL_3 "; br r29, r29"

  enum ConsumeResult c_result;
  long imm = consume_literal(&c_result);
  if (c_result == FOUND){
    // was a number
    size_t expansion_len = strlen(CALL_EXPANSION_LIT) + 20; // could be a big number
    while (result_index + expansion_len >= capacity - 2) expand_capacity();
    result_index += sprintf(result + result_index, CALL_EXPANSION_LIT, (unsigned)imm, (unsigned)imm);
  } else {
    // check if its a string/label
    struct Slice* label = consume_identifier();
    if (label != NULL){

      size_t expansion_len = strlen(CALL_EXPANSION_LBL_1) + strlen(CALL_EXPANSION_LBL_2) + 
        strlen(CALL_EXPANSION_LBL_3) + label->len * 2 + 2;
      while (result_index + expansion_len >= capacity - 2) expand_capacity();
      result_index += sprintf(result + result_index, CALL_EXPANSION_LBL_1);
      strncpy(result + result_index, label->start, label->len);
      result_index += label->len;
      result_index += sprintf(result + result_index, CALL_EXPANSION_LBL_2);
      strncpy(result + result_index, label->start, label->len);
      result_index += label->len;
      result_index += sprintf(result + result_index, CALL_EXPANSION_LBL_3);

      free(label);
    } else {
      // error
      print_error();
      fprintf(stderr, "Expected immediate\n");
      *success = false;
      return;
    }
  }
}

bool expand_macros(void){
  bool success = true;
  if (consume_keyword("nop")) expand_nop();
  else if (consume_keyword("ret")) expand_ret();
  else if (consume_keyword("push")) expand_push(&success);
  else if (consume_keyword("pop")) expand_pop(&success);
  else if (consume_keyword("pshw")) expand_push(&success);
  else if (consume_keyword("popw")) expand_pop(&success);
  else if (consume_keyword("pshd")) expand_pshd(&success);
  else if (consume_keyword("popd")) expand_popd(&success);
  else if (consume_keyword("pshb")) expand_pshb(&success);
  else if (consume_keyword("popb")) expand_popb(&success);
  else if (consume_keyword("movi")) expand_movi(&success);
  else if (consume_keyword("mov")) expand_mov(&success);
  else if (consume_keyword("call")) expand_call(&success);

  if (!success) fprintf(stderr, "Preprocesser macro error\n");

  return success;
}

// copy the program into a new string, but without the comments
// expand macros into real instructions
char** preprocess(int num_files, int* file_names, bool has_start,
  const char *const *const argv, const char * const * const files){

  char ** result_list = malloc(num_files * sizeof(char**));

  for (int i = 0; i < num_files; ++i){

    // initialize parser
    current = files[i];
    line_count = 1;
    pc = 0;
    is_kernel = false;
    result_index = 0;
    capacity = 60;
    current_file = argv[file_names[i]];

    result = malloc(sizeof(char) * capacity);
    if (result == NULL) return NULL;

    // initial null used to detect start of program
    // used when printing errors
    result[result_index] = '\0';
    result_index++; 

    // add a jump to _start
    if (i == 0 && has_start){
      result_index += sprintf(result + result_index, 
        "  movu r29, _start; movl r29, _start; br r29, r29\n");
    }

    while (*current != '\0'){
      // expand dynamic array if necessary, exit if realloc fails 
      if (!check_capacity()) {
        for (int j = 0; j < i; ++j) free(result_list[j]);
        free(result_list);
        return NULL;
      }

      // skip comments, exit if EOF is reached
      if (!skip_comments()) goto end;

      if (!expand_macros()) {
        for (int j = 0; j < i; ++j) free(result_list[j]);
        free(result);
        free(result_list);
        return NULL;
      }

      // write one character, then repeat loop
      result[result_index] = *current;
      if (*current == '\n') line_count++;
      result_index++;
      current++;
    }

    // include null terminator, realloc should ensure there's always room
    end:  result[result_index] = 0;
    result_list[i] = result;
  }

  return result_list;
}