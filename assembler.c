#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>

#include "slice.h"
#include "assembler.h"
#include "hashmap.h"
#include "instruction_array.h"

/*
  Two-pass assembler.
  First pass calculates addresses of labels
  Second pass converts to text into binary
*/

static char const * program;
static char const * current;

static unsigned line_count = 1;
static unsigned long pc = 0;

// map labels to their addresses
static struct HashMap* valid_labels;

// does the file wish to use pivileges instructions?
static bool is_kernel = false;

// print line causing an error
static void print_error() {
  printf("Error in line %u: \"", line_count);
  char const * end = current;
  while (*end != '\0' && *end != '\n') end++;
  struct Slice unrecognized = {current, end - current};
  print_slice(&unrecognized);
  printf("\"\n");
}

// is the rest of the file just whitespace?
static bool is_at_end() {
  while (isspace(*current)) {
    current += 1;
  }
  if (*current != 0) return false;
  else return true;
}

// skip whitespace and commas until end of line or non-whitespace character
static void skip() {
  while ((isspace(*current) && *current != '\n') || *current == ',') {
    current += 1;
  }
}

// skip until we get to a new nonempty line
static void skip_newline() {
  while (*current == '\n') {
    current += 1;
    line_count += 1;
  }
}

// attempt to consume a string, has no effect if a match is not found
static bool consume(const char* str) {
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
static bool consume_keyword(const char* str) {
  skip();
  size_t i = 0;
  while (true) {
    char const expected = str[i];
    char const found = current[i];
    if (expected == 0) {
      /* survived to the end of the expected string */
      if (isspace(found)) {
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
static struct Slice* consume_identifier(void) {
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

// label is an identifier followed by a colon
static struct Slice* consume_label(void){
  skip();
  char const * old_current = current;
  struct Slice* label = consume_identifier();
  if (consume(":")) return label;

  // undo side effects
  current = old_current;
  return NULL;
}

// attempt to consume a register
static int consume_register(void) {
  skip();
  size_t i = 0;
  // registers begin with an r
  if (current[i] == 'r') {
    int v = 0;
    i += 1;
    while(isdigit(current[i])) {
      // then followed by numbers
      v = 10 * v + current[i] - '0';
      i += 1;
    }

    if (v > 31) return -1;
    current += i;
    return v;
  } else {
    if (consume("sp")) return 1;
    else if (consume("bp")) return 2;
    else if (consume("ra")) return 31;
    else return -1;
  }
}

// attempt to consume a control register
static int consume_control_register(void) {
  skip();
  size_t i = 0;
  // registers begin with an r
  if (current[i] == 'c' && current[i + 1] == 'r') {
    int v = 0;
    i += 2;
    while(isdigit(current[i])) {
      // then followed by numbers
      v = 10 * v + current[i] - '0';
      i += 1;
    }

    if (v > 6) return -1;
    current += i;
    return v;
  } else {
    if (consume("psr")) return 0;
    else if (consume("pid")) return 1;
    else if (consume("isr")) return 2;
    else if (consume("imr")) return 3;
    else if (consume("epc")) return 4;
    else if (consume("efg")) return 5;
    else if (consume("epc")) return 6;
    else return -1;
  }
}

// attempt to consume an integer literal
static long consume_literal(enum ConsumeResult* result) {
  skip();
  bool negate = false;
  char const * old_current = current;
  if (*current == '-') {
    negate = true;
    current++;
    skip();
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
    while (isdigit(*current)) {
      if ((*current) - '0' > 1){
        print_error();
        printf("Invalid binary literal\n");
        *result = ERROR;
        return 0;
      }
      v = 2*v + ((*current) - '0');
      current += 1;
    }

    *result = FOUND;
    if (negate) v *= -1;
    return v;
  } else if (*current == '0' && (*(current + 1) == 'o' || *(current + 1) == 'O')) {
    current += 2;
    // octal literal
    long v = 0;
    while (isdigit(*current)) {
      if ((*current) - '7' > 1){
        print_error();
        printf("Invalid octal literal\n");
        *result = ERROR;
        return 0;
      }
      v = 8*v + ((*current) - '0');
      current += 1;
    }

    *result = FOUND;
    if (negate) v *= -1;
    return v;
  } else if (*current == '0' && (*(current + 1) == 'x' || *(current + 1) == 'X')) {
    long v = 0;
    current += 2;
    while (isalnum(*current)) {
      int d;
      if (isdigit(*current)){
        d = *current - '0';
      } else if (isupper(*current) && *current <= 'F'){
        d = *current - 'A' + 10;
      } else if (islower(*current) && *current <= 'f'){
        d = *current - 'a' + 10;
      } else {
        print_error();
        printf("Invalid hex literal\n");
        *result = ERROR;
        return 0;
      }

      v = 16*v + d;
      current += 1;
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

// consume a literal immediate or label immediate
static long consume_immediate(enum ConsumeResult* result){
  long imm;
  struct Slice* label = consume_identifier();
  if (label != NULL){
    if (hash_map_contains(valid_labels, label)){
      imm = hash_map_get(valid_labels, label) - pc - 1;
      *result = FOUND;
    } else {
      print_error();
      printf("Label \"");
      print_slice(label);
      printf("\" has not been defined\n");
      *result = ERROR;
    }
  } else {
    imm = consume_literal(result);
  }
  return imm;
}

static int encode_bitwise_immediate(long imm, bool* success){
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
    printf("Bitwise instruction immediate must be an 8 bit value, ");
    printf("shifted by 0, 8, 16, or 24 bits\n");
    printf("Got %ld\n", imm);
    return 0;
  }
}

static int encode_shift_immediate(long imm, bool* success){
  if (0 <= imm && imm < 31){
    return imm;
  } else {
    *success = false;
    print_error();
    printf("Shift instruction immediate must be in range 0 to 31\n");
    printf("Got %ld\n", imm);
    return 0;
  }
}

static int encode_arithmetic_immediate(long imm, bool* success){
  if (-(1 << 11) <= imm && imm < (1 << 11)){
    return imm & 0xFFF;
  } else {
    print_error();
    printf("Arithmetic instruction immediate must be in range -2048 to 2047\n");
    printf("Got %ld\n", imm);
    *success = false;
    return 0;
  }
}

// consume an alu instruction and return the corresponding encoding
static int consume_alu_op(int alu_op, bool* success){
  assert(0 <= alu_op && alu_op < 32); // ensure alu_op is valid

  int ra = consume_register();
  if (ra == -1){
    print_error();
    printf("Invalid register\n");
    printf("Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }
  int rb = consume_register();
  if (rb == -1){
    print_error();
    printf("Invalid register\n");
    printf("Valid registers are r0 - r31\n");
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
      printf("Invalid register or immediate\n");
      printf("Valid registers are r0 - r31\n");
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

static int encode_lui_immediate(long imm, bool* success){
  if ((imm & 0x3FF) == 0 && imm < ((long)1 << 32)){
    return (int)imm >> 10;
  } else {
    *success = false;
    print_error();
    printf("lui immediate must be a 32 bit integer with zero for bottom 10 bits\n");
    printf("Got %ld\n", imm);
    return 0;
  }
}

static int consume_lui(bool* success){
  enum ConsumeResult result;
  int ra = consume_register();
  if (ra == -1){
    print_error();
    printf("Invalid register\n");
    printf("Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  long imm = consume_immediate(&result);
  if (result != FOUND){
    print_error();
    printf("Invalid immediate\n");
    *success = false;
  }

  int encoding = encode_lui_immediate(imm, success);

  assert(encoding == (encoding & 0x3FFFFF)); // ensure immediate fits in 22 bits

  int instruction = 2 << 27;
  instruction |= ra << 22;
  instruction |= encoding;
  return instruction;
}

static int encode_memory_immediate(long imm, bool* success){
  if (imm == (imm & 0xFFF)){
    return imm;
  } else if (imm == (imm & 0x1FFE)){
    return (imm >> 1) | (1 << 12);
  } else if (imm == (imm & 0x3FFC)) {
    return (imm >> 2) | (2 << 12);
  } else if (imm == (imm & 0x7FF8)){
    return (imm >> 3) | (3 << 12);
  } else {
    // can't encode
    print_error();
    printf("Invalid immediate for memory instruction\n");
    printf("Immediate must be a 12 bit number shifted by 0, 1, 2, or 3\n");
    printf("Got %ld\n", imm);
    *success = false;
    return 0;
  }
}

static int consume_mem(bool is_absolute, bool is_load, bool* success){
  int instruction = 0;
  if (is_absolute){
    instruction |= 3 << 27;
  } else {
    instruction |= 4 << 27;
  }

  if (is_load){
    instruction |= 1 << 16;
  }

  int ra = consume_register();
  if (ra == -1){
    print_error();
    printf("Invalid register\n");
    printf("Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  if (!consume("[")){
    *success = false;
    print_error();
    printf("Expected \"[\" in memory instruction\n");
    return 0;
  }

  int rb = consume_register();
  if (rb == -1){
    if (!is_absolute){
      // relative addressing - use r0
      rb = 0;
    }
    else {
      print_error();
      printf("Invalid register\n");
      printf("Valid registers are r0 - r31\n");
      *success = false;
      return 0;
    }
  }

  instruction |= ra << 22;
  instruction |= rb << 17;

  long imm;
  int y;

  if (consume("]") && is_absolute){
    enum ConsumeResult result;
    imm = consume_immediate(&result);
    if (result == FOUND){
      // postincrement
      if (!is_absolute){
        print_error();
        printf("Postincrement addressing not allowed for relative addressing\n");
        *success = false;
        return 0;
      }
      y = 2;
    } else if (result == NOT_FOUND){
      // no offset
      imm = 0;
      y = 0;
    } else {
      // error
      return 0;
    }
  } else {
    enum ConsumeResult result;
    imm = consume_immediate(&result);
    if (result == FOUND){
      if (!consume("]")){
        print_error();
        printf("Expected \"]\" in memory instruction\n");
        *success = false;
        return 0;
      }
      if (consume("!")){
        // preincrement
        if (!is_absolute){
          print_error();
          printf("Preincrement addressing not allowed for relative addressing\n");
          *success = false;
          return 0;
        }
        y = 1;
      } else {
        // signed offset
        y = 0;
      }
    } else {
      // error
      print_error();
      printf("Invalid immediate in memory instruction\n");
      *success = false;
      return 0;
    }
  }
  int encoding = encode_memory_immediate(imm, success);
  assert(encoding == (encoding & 0x3FFF)); // ensure encoding is 14 bits

  instruction |= (y << 14);
  instruction |= encoding;

  return instruction;
}

static int encode_branch_immediate(long imm, bool* success){
  if (-(1 << 23) <= imm && imm < (1 << 23) && ((imm & 0x3) == 0)){
    return imm;
  } else {
    *success = false;
    print_error();
    printf("branch immediate must be in range -8388608 to 8388607, and be divisible by 4\n");
    printf("Got %ld\n", imm);
    return 0;
  }
}

static int consume_branch(int branch_code, bool is_absolute, bool* success){
  int instruction = 0;

  assert(0 <= branch_code && branch_code < 19); // ensure branch code is valid

  int ra = consume_register();
  if (ra == -1){
    // it's an immediate branch
    enum ConsumeResult result;
    int imm = consume_immediate(&result);
    if (result != FOUND){
      print_error();
      printf("Branch instruction expects register or immediate operand\n");
      *success = false;
      return 0;
    }
    if (is_absolute){
      print_error();
      printf("Immediate branch is not allowed for absolute branches\n");
      *success = false;
      return 0;
    }
    int encoding = encode_branch_immediate(imm, success);
    instruction |= 5 << 27; // opcode
    instruction |= branch_code <<22;
    instruction |= encoding;
  } else {
    // register branch
    int rb = consume_register();
    if (rb == -1){
      // ra was omitted
      rb = ra;
      ra = 0;
    }
    if (is_absolute) instruction |= 6 << 27;
    else instruction |= 7 << 27;
    instruction |= branch_code << 22;
    instruction |= ra << 5;
    instruction |= rb;
  }

  return instruction;
}

static int consume_syscall(bool* success){
  if (consume("EXIT")){
    int instruction = (8 << 27);

    return instruction;
  } else {
    // unrecognized call
    print_error();
    printf("Unrecognized syscall\n");
    printf("Supported syscalls are: EXIT\n");
    *success = false;
    return 0;
  }
}

static void check_privileges(bool* success){
  if (!is_kernel){
    *success = false;
    print_error();
    printf("Used privileged instruction\n");
    printf("Put .kernel somewhere in the file if this was intentional\n");
  }
}

static int consume_tlb_op(int tlb_op, bool* success){
  check_privileges(success);
  assert(0 <= tlb_op && tlb_op < 4); // ensure tlb op is valid

  int instruction = 31 << 27; // opcode

  if (tlb_op == 3){
    instruction |= 1 << 11;
  } else {
    int ra = consume_register();
    if (ra == -1){
      print_error();
      printf("Invalid register\n");
      printf("Valid registers are r0 - r31\n");
      *success = false;
      return 0;
    }
    int rb = consume_register();
    if (rb == -1){
      print_error();
      printf("Invalid register\n");
      printf("Valid registers are r0 - r31\n");
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

static int consume_crmv(bool* success){
  check_privileges(success);
  int instruction = 31 << 27;
  instruction |= 1 << 12;

  int ra = consume_register();
  int rb;
  if (ra == -1){
    ra = consume_control_register();
    if (ra == -1){
      print_error();
      printf("Invalid register or control register\n");
      *success = false;
      return 0; 
    }
    rb = consume_control_register();
    if (rb == -1) {
      rb = consume_register();
      if (rb == -1){
        print_error();
        printf("Invalid control register\n");
        *success = false;
        return 0; 
      }
      // crmv crA, rB
    } else {
      // crmv crA, crB
      instruction |= 1 << 10;
    }
  } else {
    rb = consume_control_register();
    if (rb == -1){
      print_error();
      printf("Invalid control register\n");
      *success = false;
      return 0; 
    }
    // crmv rA, crB
    instruction |= 2 << 10;
  }
  instruction |= ra << 22;
  instruction |= rb << 17;

  return instruction;
}

static int consume_mode_op(bool* success){
  check_privileges(success);

  int instruction = 31 << 27; // opcode
  instruction |= 2 << 12;

  if (consume("run"));
  else if (consume("sleep")){
    instruction |= 1 << 8;
  } else if (consume("halt")){
    instruction |= 2 << 8;
  } else {
    print_error();
    printf("Invalid mode\n");
    printf("Valid modes are: run, sleep, or halt\n");
    *success = false;
    return 0;
  }

  return instruction;
}

static int consume_rfe(bool* success){
  check_privileges(success);

  int instruction = 31 << 27;
  instruction |= 3 << 12;

  int ra = consume_register();
  if (ra == -1){
    print_error();
    printf("Invalid register\n");
    printf("Valid registers are r0 - r31\n");
    *success = false;
    return 0;
  }

  instruction |= ra << 22;

  return instruction;
}

// consumes a single instruction and converts it to binary or hex
static int consume_instruction(bool* success){
  // user instructions
  if (consume_keyword("and")) return consume_alu_op(0, success);
  if (consume_keyword("nand")) return consume_alu_op(1, success);
  if (consume_keyword("or")) return consume_alu_op(2, success);
  if (consume_keyword("nor")) return consume_alu_op(3, success);
  if (consume_keyword("xor")) return consume_alu_op(4, success);
  if (consume_keyword("xnor")) return consume_alu_op(5, success);
  if (consume_keyword("not")) return consume_alu_op(6, success);
  if (consume_keyword("lsl")) return consume_alu_op(7, success);
  if (consume_keyword("lsr")) return consume_alu_op(8, success);
  if (consume_keyword("asr")) return consume_alu_op(9, success);
  if (consume_keyword("rotl")) return consume_alu_op(10, success);
  if (consume_keyword("rotr")) return consume_alu_op(11, success);
  if (consume_keyword("lslc")) return consume_alu_op(12, success);
  if (consume_keyword("lsrc")) return consume_alu_op(13, success);
  if (consume_keyword("add")) return consume_alu_op(14, success);
  if (consume_keyword("addc")) return consume_alu_op(15, success);
  if (consume_keyword("sub")) return consume_alu_op(16, success);
  if (consume_keyword("subb")) return consume_alu_op(17, success);
  if (consume_keyword("mul")) return consume_alu_op(18, success);
  if (consume_keyword("lui")) return consume_lui(success);
  if (consume_keyword("sw")) return consume_mem(true, false, success);
  if (consume_keyword("lw")) return consume_mem(true, true, success);
  if (consume_keyword("swr")) return consume_mem(false, false, success);
  if (consume_keyword("lwr")) return consume_mem(false, true, success);
  if (consume_keyword("br")) return consume_branch(0, false, success);
  if (consume_keyword("bz")) return consume_branch(1, false, success);
  if (consume_keyword("bnz")) return consume_branch(2, false, success);
  if (consume_keyword("bs")) return consume_branch(3, false, success);
  if (consume_keyword("bns")) return consume_branch(4, false, success);
  if (consume_keyword("bc")) return consume_branch(5, false, success);
  if (consume_keyword("bnc")) return consume_branch(6, false, success);
  if (consume_keyword("bo")) return consume_branch(7, false, success);
  if (consume_keyword("bno")) return consume_branch(8, false, success);
  if (consume_keyword("bp")) return consume_branch(9, false, success);
  if (consume_keyword("bnp")) return consume_branch(10, false, success);
  if (consume_keyword("bg")) return consume_branch(11, false, success);
  if (consume_keyword("bge")) return consume_branch(12, false, success);
  if (consume_keyword("bl")) return consume_branch(13, false, success);
  if (consume_keyword("ble")) return consume_branch(14, false, success);
  if (consume_keyword("ba")) return consume_branch(15, false, success);
  if (consume_keyword("bae")) return consume_branch(16, false, success);
  if (consume_keyword("bb")) return consume_branch(17, false, success);
  if (consume_keyword("bbe")) return consume_branch(18, false, success);
  if (consume_keyword("bra")) return consume_branch(0, true, success);
  if (consume_keyword("bza")) return consume_branch(1, true, success);
  if (consume_keyword("bnza")) return consume_branch(2, true, success);
  if (consume_keyword("bsa")) return consume_branch(3, true, success);
  if (consume_keyword("bnsa")) return consume_branch(4, true, success);
  if (consume_keyword("bca")) return consume_branch(5, true, success);
  if (consume_keyword("bnca")) return consume_branch(6, true, success);
  if (consume_keyword("boa")) return consume_branch(7, true, success);
  if (consume_keyword("bnoa")) return consume_branch(8, true, success);
  if (consume_keyword("bpa")) return consume_branch(9, true, success);
  if (consume_keyword("bnpa")) return consume_branch(10, true, success);
  if (consume_keyword("bga")) return consume_branch(11, true, success);
  if (consume_keyword("bgea")) return consume_branch(12, true, success);
  if (consume_keyword("bla")) return consume_branch(13, true, success);
  if (consume_keyword("blea")) return consume_branch(14, true, success);
  if (consume_keyword("baa")) return consume_branch(15, true, success);
  if (consume_keyword("baea")) return consume_branch(16, true, success);
  if (consume_keyword("bba")) return consume_branch(17, true, success);
  if (consume_keyword("bbea")) return consume_branch(18, true, success);
  if (consume_keyword("sys")) return consume_syscall(success);

  // privileged instructions
  if (consume_keyword("tlbr")) return consume_tlb_op(0, success);
  if (consume_keyword("tlbw")) return consume_tlb_op(1, success);
  if (consume_keyword("tlbc")) return consume_tlb_op(2, success);
  if (consume_keyword("crmv")) return consume_crmv(success);
  if (consume_keyword("mode")) return consume_mode_op(success);
  if (consume_keyword("rfe")) return consume_rfe(success);
  
  *success = false;
  return 0;
}

// assemble an entire program
struct InstructionArray* assemble(char const* prog){
  program = prog;
  current = prog;

  valid_labels = create_hash_map(10000);

  bool success = true;

  struct InstructionArray* instructions = create_instruction_array(1000);

  while (success){
    // consume any labels, they were already dealt with
    while (skip_newline(), consume_label());

    if (pc > ((long)1 << 32)){
      print_error();
      printf("Program does not fit in 32-bit address space\n");
      goto error;
    }

    // directives
    if (consume_keyword(".global")) {
      // handled in first pass

      // ensure the first pass didn't miss anything
      struct Slice* name = consume_identifier();
      assert(name);
      assert(hash_map_contains(valid_labels, name));
    }
    else if (consume_keyword(".kernel")); // handled in first pass
    else if (consume_keyword(".origin")) { 
      enum ConsumeResult result; 
      long imm = consume_literal(&result);
      if (result != FOUND){
        print_error();
        printf("\n");
        goto error;
      }
      if (imm < pc){
        print_error();
        printf(".origin cannot be used to go backwards\n");
        goto error;
      } else if (imm >= ((long)1 << 32)){
        print_error();
        printf(".origin address must be a 32 bit integer\n");
        goto error;
      } else {
        for (int i = 0; i < imm - pc; ++i) 
          instruction_array_append(instructions, 0);
      }
      pc = imm;
    }
    else if (consume_keyword(".fill")) {
      enum ConsumeResult result; 
      long imm = consume_literal(&result);
      if (result != FOUND){
        print_error();
        printf("\n");
        goto error;
      }
      if (0 <= imm && imm < ((long)1 << 32)){
        instruction_array_append(instructions, imm);
      } else {
        print_error();
        printf(".fill immediate must be a positive 32 bit integer\n");
        goto error;
      }
    }
    else if (consume_keyword(".space")) { 
      enum ConsumeResult result; 
      long imm = consume_literal(&result);
      if (result != FOUND){
        print_error();
        printf("\n");
        goto error;
      }
      if (0 <= imm && imm < ((long)1 << 32)){
        for (int i = 0; i < imm; ++i) 
          instruction_array_append(instructions, 0);
      } else {
        print_error();
        printf(".space immediate must be a positive 32 bit integer\n");
        goto error;
      }
      pc += imm;
    } else {
      int instruction = consume_instruction(&success);
      if (success) instruction_array_append(instructions, instruction);
      pc++;
    }
  }

  if (!is_at_end()) {
    print_error();
    printf("Unrecognized instruction\n");
    goto error;
  }

  return instructions;

  error:
    destroy_instruction_array(instructions);
    return NULL;
}