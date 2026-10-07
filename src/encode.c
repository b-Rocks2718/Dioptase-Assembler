#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include "encode.h"
#include "assembler.h"
#include "lexer.h"
#include "keyword.h"

/*
  Instruction parsing and encoding.

  consume_instruction() reads one mnemonic and its operands at `current` and
  returns the 32-bit encoding. Field layouts and opcode numbers follow
  docs/ISA.md; the section each constant comes from is noted beside it.
  Operands that name labels or constants are resolved by consume_immediate()
  in assembler.c, which returns 0 for labels during pass 1.

  Every parser reports its own errors and clears *success; the returned
  word is then meaningless.
*/

// ---- Encoding fields (ISA.md) -------------------------------------------

// Bit positions shared by most formats: opcode [31:27], rA [26:22], rB [21:17].
enum {
  OPCODE_SHIFT = 27,
  RA_SHIFT = 22,
  RB_SHIFT = 17,
};

// Major opcodes, bits [31:27].
enum Opcode {
  OP_ALU_REG = 0,          // 3 Register ALU Instructions
  OP_ALU_IMM = 1,          // ALU immediate instructions
  OP_LUI = 2,              // lui
  OP_MEM_ABS = 3,          // Memory, absolute; +3 per width (word, double, byte)
  OP_MEM_REL_REG = 4,      // Memory, PC-relative with base register
  OP_MEM_REL_IMM = 5,      // Memory, PC-relative immediate only
  OP_BRANCH_IMM = 12,      // Immediate Branches
  OP_BRANCH_ABS_REG = 13,  // Absolute Register Branches
  OP_BRANCH_REL_REG = 14,  // Relative Register Branches
  OP_TRAP = 15,            // Trap Instruction
  OP_FADA = 16,            // Atomics: fetch-add absolute; +1 rel reg, +2 rel imm
  OP_SWPA = 19,            // Atomics: swap absolute; +1 rel reg, +2 rel imm
  OP_ADPC = 22,            // adpc
  OP_PRIV = 31,            // Privileged Instructions
};

// Opcodes advance by this much from word to double to byte accesses.
enum { MEM_WIDTH_OPCODE_STRIDE = 3 };

// Access widths, in opcode order.
enum MemWidth { WIDTH_WORD = 0, WIDTH_DOUBLE = 1, WIDTH_BYTE = 2 };

// ALU operation field: [9:5] for register forms, [16:12] for immediates.
enum AluOp {
  ALU_AND = 0, ALU_NAND, ALU_OR, ALU_NOR, ALU_XOR, ALU_XNOR, ALU_NOT,
  ALU_LSL, ALU_LSR, ALU_ASR, ALU_ROTL, ALU_ROTR, ALU_LSLC, ALU_LSRC,
  ALU_ADD, ALU_ADDC, ALU_SUB, ALU_SUBB,
  ALU_SXTB, ALU_SXTD, ALU_TNCB, ALU_TNCD,
};
enum { ALU_REG_OP_SHIFT = 5, ALU_IMM_OP_SHIFT = 12 };

// Branch condition codes, bits [26:22] of every branch form.
enum BranchCond {
  COND_ALWAYS = 0, COND_Z, COND_NZ, COND_S, COND_NS, COND_C, COND_NC,
  COND_O, COND_NO, COND_PS, COND_NPS, COND_G, COND_GE, COND_L, COND_LE,
  COND_A, COND_AE, COND_B, COND_BE,
};
enum { BRANCH_REG_RA_SHIFT = 5 };

// Privileged instruction ID, bits [16:12], and its sub-operation, [11:10].
// Bit 11 alone is the "all" flag of ipi/eoi.
enum PrivId {
  PRIV_TLB = 0,   // tlbr/tlbw/tlbi/tlbc
  PRIV_CRMV = 1,  // Move to/from control regs
  PRIV_MODE = 2,  // Set mode
  PRIV_RFE = 3,   // Return from trap
  PRIV_IPI = 4,   // Inter-processor interrupts
  PRIV_EOI = 5,   // End of interrupt instruction
};
enum { PRIV_ID_SHIFT = 12, PRIV_SUBOP_SHIFT = 10, PRIV_ALL_BIT = 11 };

// crmv sub-operations: destination/source register kinds.
enum CrmvForm {
  CRMV_CR_FROM_REG = 0,   // crmv crA, rB
  CRMV_REG_FROM_CR = 1,   // crmv rA, crB
  CRMV_CR_FROM_CR = 2,    // crmv crA, crB
  CRMV_REG_FROM_REG = 3,  // crmv rA, rB
};

// ipi addresses a core with the 2-bit field n (ISA.md), so cores 0-3.
enum { IPI_CORE_COUNT = 4 };
// eoi clears one ISR bit selected by the 4-bit field n.
enum { EOI_MAX_BIT = 15 };

// Absolute memory addressing-mode field y, bits [15:14].
enum MemUpdate { MEM_OFFSET = 0, MEM_PREINCREMENT = 1, MEM_POSTINCREMENT = 2 };
enum { MEM_ABS_MODE_SHIFT = 14 };
// Load bit for register-based memory forms; the immediate-only form uses bit 21.
enum { MEM_LOAD_BIT = 16, MEM_REL_IMM_LOAD_BIT = 21 };
// Atomic base register field.
enum { ATOMIC_RB_SHIFT = 12 };

// Signed immediate widths from ISA.md.
enum {
  ARITH_IMM_BITS = 12,
  MEM_REL_IMM_BITS = 16,
  MEM_REL_LONG_IMM_BITS = 21,
  ATOMIC_IMM_BITS = 12,
  ATOMIC_LONG_IMM_BITS = 17,
  ADPC_IMM_BITS = 22,
};

// ---- Immediate encoders ---------------------------------------------------

// Return whether imm fits in a two's-complement field of `bits` bits.
static bool fits_signed(long imm, unsigned bits) {
  return -(1L << (bits - 1)) <= imm && imm < (1L << (bits - 1));
}

// Encode imm as a signed field of `bits` bits, or report that it does not fit.
// `what` begins the message: "<what> must fit in signed N bits (min to max)".
static int encode_signed_field(long imm, unsigned bits, const char* what, bool* success) {
  if (fits_signed(imm, bits)) return (int)(imm & ((1L << bits) - 1));
  print_error();
  fprintf(stderr, "%s must fit in signed %u bits (%ld to %ld)\n", what, bits,
          -(1L << (bits - 1)), (1L << (bits - 1)) - 1);
  fprintf(stderr, "Got %ld\n", imm);
  *success = false;
  return 0;
}

// Encode a memory or atomic displacement of the given signed width.
static int encode_memory_offset(long imm, unsigned bits, bool* success) {
  return encode_signed_field(imm, bits, "Invalid immediate for memory instruction\nImmediate", success);
}

// Encode a bitwise immediate: an 8-bit value i shifted left by 8*y, as yy:i.
static int encode_bitwise_immediate(long imm, bool* success){
  for (int shift = 0; shift < 4; ++shift) {
    long byte_mask = 0xFFL << (8 * shift);
    if (imm == (imm & byte_mask)) return (int)(imm >> (8 * shift)) | (shift << 8);
  }
  *success = false;
  print_error();
  fprintf(stderr, "Bitwise instruction immediate must be an 8 bit value, ");
  fprintf(stderr, "shifted by 0, 8, 16, or 24 bits\n");
  fprintf(stderr, "Got %ld\n", imm);
  return 0;
}

// Encode a shift amount: the 5-bit field i (ISA.md "Shifts").
static int encode_shift_immediate(long imm, bool* success){
  if (0 <= imm && imm <= 31) return (int)imm;
  *success = false;
  print_error();
  fprintf(stderr, "Shift instruction immediate must be in range 0 to 31\n");
  fprintf(stderr, "Got %ld\n", imm);
  return 0;
}

// Encode a signed 12-bit arithmetic immediate.
static int encode_arithmetic_immediate(long imm, bool* success){
  if (fits_signed(imm, ARITH_IMM_BITS)) return (int)(imm & 0xFFF);
  print_error();
  fprintf(stderr, "Arithmetic instruction immediate must be in range -2048 to 2047\n");
  fprintf(stderr, "Got %ld\n", imm);
  *success = false;
  return 0;
}

// Encode the lui immediate: a 32-bit value whose low 10 bits are zero,
// stored as its top 22 bits.
static int encode_lui_immediate(long imm, bool* success){
  if ((imm & 0x3FF) == 0 && imm < ((long)1 << 32)) return ((int)imm >> 10) & 0x3FFFFF;
  *success = false;
  print_error();
  fprintf(stderr, "lui immediate must be a 32 bit integer with zero for bottom 10 bits\n");
  fprintf(stderr, "Got %ld\n", imm);
  return 0;
}

// Encode an absolute-addressing offset as zz:i, a 12-bit signed i scaled by
// 2^z (ISA.md "Absolute Addressing"). The smallest shift that represents imm
// exactly is chosen.
static int encode_absolute_memory_immediate(long imm, bool* success){
  for (int shift = 0; shift < 4; ++shift) {
    // imm must be a sign-extended (11 + shift)-bit value with `shift` low zeros.
    long magnitude_mask = (0x800L << shift) - 1;
    bool sign_ok = imm == (imm & magnitude_mask) || ~imm == (~imm & magnitude_mask);
    bool aligned = (imm & ((1L << shift) - 1)) == 0;
    if (sign_ok && aligned) return (int)((imm >> shift) & 0xFFF) | (shift << 12);
  }
  print_error();
  fprintf(stderr, "Invalid immediate for memory instruction\n");
  fprintf(stderr, "Immediate must be a 12 bit number shifted by 0, 1, 2, or 3\n");
  fprintf(stderr, "Got %ld\n", imm);
  *success = false;
  return 0;
}

// Encode a branch displacement: a multiple of 4 stored as a 22-bit word count.
static int encode_branch_immediate(long imm, bool* success){
  if (-(1 << 23) <= imm && imm < (1 << 23) && (imm & 3) == 0) return (imm >> 2) & 0x3FFFFF;
  *success = false;
  print_error();
  fprintf(stderr, "branch immediate must be divisible by 4 and in range -8388608 to 8388607\n");
  fprintf(stderr, "Got %ld\n", imm);
  return 0;
}

// ---- Operand helpers -----------------------------------------------------

// Parse a required general register; -1 after reporting the error.
static int expect_register(bool* success){
  int reg = consume_register();
  if (reg == -1) {
    print_error();
    fprintf(stderr, "Invalid register\n");
    fprintf(stderr, "Valid registers are r0 - r31\n");
    *success = false;
  }
  return reg;
}

// Reject a privileged instruction outside -kernel mode. The hint is printed
// once per run so a file full of kernel code yields one diagnostic.
static bool check_privileges(bool* success){
  static bool has_printed = false;
  if (is_kernel) return true;
  *success = false;
  if (!has_printed){
    has_printed = true;
    print_error();
    fprintf(stderr, "Used privileged instruction\n");
    fprintf(stderr, "Run assembler with -kernel if this was intentional\n");
  }
  return false;
}

// A parsed "[...]" memory operand.
// rb is -1 for the immediate-only "[imm]" form.
struct MemOperand {
  int rb;
  long imm;
  enum MemUpdate update;
};

// Parse "[rB]", "[rB, imm]", "[imm]", and, when allowed, the absolute update
// forms "[rB, imm]!" and "[rB], imm". Absolute addressing requires rB.
// allow_update: parse "!" (rejected unless absolute) and, for absolute
// addressing, a post-increment literal after "]".
static bool parse_mem_operand(bool is_absolute, bool allow_update, struct MemOperand* out,
                              bool* success){
  out->imm = 0;
  out->update = MEM_OFFSET;

  if (!consume("[")){
    *success = false;
    print_error();
    fprintf(stderr, "Expected \"[\" in memory instruction\n");
    return false;
  }

  out->rb = consume_register();
  if (out->rb == -1 && is_absolute) {
    expect_register(success);
    return false;
  }

  if (consume("]")){
    if (allow_update && is_absolute){
      // postincrement: [rb], imm (literal only)
      enum ConsumeResult result;
      long imm = consume_literal(&result);
      if (result == ERROR){
        *success = false;
        return false;
      }
      if (result == FOUND){
        out->imm = imm;
        out->update = MEM_POSTINCREMENT;
      }
    }
    return true;
  }

  enum ConsumeResult result;
  out->imm = consume_immediate(&result, NULL);
  if (result != FOUND){
    print_error();
    fprintf(stderr, "Invalid immediate in memory instruction\n");
    *success = false;
    return false;
  }
  if (!consume("]")){
    print_error();
    fprintf(stderr, "Expected \"]\" in memory instruction\n");
    *success = false;
    return false;
  }
  if (allow_update && consume("!")){
    // preincrement: [rb, imm]!
    if (!is_absolute){
      print_error();
      fprintf(stderr, "Preincrement addressing not allowed for relative addressing\n");
      *success = false;
      return false;
    }
    out->update = MEM_PREINCREMENT;
  }
  return true;
}

// ---- Instruction parsers -------------------------------------------------

// ALU ops that take one source register: not, sxtb, sxtd, tncb, tncd.
static bool alu_is_unary(int alu_op){
  return alu_op == ALU_NOT || alu_op >= ALU_SXTB;
}

// Parse the final ALU operand (rC or an immediate) and encode the instruction
// for destination ra and first source rb.
static int encode_alu_operation(int alu_op, int ra, int rb, bool* success){
  int rc = consume_register();
  if (rc != -1){
    // and ra, rb, rc
    return (OP_ALU_REG << OPCODE_SHIFT) | (ra << RA_SHIFT) | (rb << RB_SHIFT) |
           (alu_op << ALU_REG_OP_SHIFT) | rc;
  }

  // and ra, rb, imm
  enum ConsumeResult result;
  long imm = consume_immediate(&result, NULL);
  if (result != FOUND){
    print_error();
    if (result == NOT_FOUND) fprintf(stderr, "Invalid register or immediate\n");
    *success = false;
    return 0;
  }

  int encoding;
  if (alu_op <= ALU_NOT){
    encoding = encode_bitwise_immediate(imm, success);
  } else if (alu_op <= ALU_LSRC){
    encoding = encode_shift_immediate(imm, success);
  } else if (alu_op <= ALU_SXTB){
    // ISA.md lists immediate forms only for add/addc/sub/subb, but the
    // assembler has always also accepted "sxtb rA, imm" here (op 18 with an
    // arithmetic immediate). Kept for compatibility; see the review notes.
    encoding = encode_arithmetic_immediate(imm, success);
  } else {
    print_error();
    fprintf(stderr, "ALU operation %d does not support immediate values\n", alu_op);
    *success = false;
    return 0;
  }
  assert(encoding == (encoding & 0xFFF)); // ensure encoding always fits in 12 bits

  return (OP_ALU_IMM << OPCODE_SHIFT) | (ra << RA_SHIFT) | (rb << RB_SHIFT) |
         (alu_op << ALU_IMM_OP_SHIFT) | encoding;
}

// Parse "op rA, rB, rC|imm", or "op rA, rC|imm" for unary operations.
static int consume_alu_op(int alu_op, bool* success){
  int ra = expect_register(success);
  if (ra == -1) return 0;

  int rb = 0;
  if (!alu_is_unary(alu_op)){
    rb = expect_register(success);
    if (rb == -1) return 0;
  }
  return encode_alu_operation(alu_op, ra, rb, success);
}

// cmp rB, rC|imm is sub r0, rB, rC|imm: only the flags are kept.
static int consume_cmp(bool* success){
  int rb = expect_register(success);
  if (rb == -1) return 0;
  return encode_alu_operation(ALU_SUB, 0, rb, success);
}

// Parse "lui rA, imm".
static int consume_lui(bool* success){
  int ra = expect_register(success);
  if (ra == -1) return 0;

  enum ConsumeResult result;
  long imm = consume_immediate(&result, NULL);
  if (result != FOUND){
    print_error();
    if (result == NOT_FOUND) fprintf(stderr, "Invalid immediate\n");
    *success = false;
    return 0;
  }

  int encoding = encode_lui_immediate(imm, success);
  assert(encoding == (encoding & 0x3FFFFF)); // ensure immediate fits in 22 bits
  return (OP_LUI << OPCODE_SHIFT) | (ra << RA_SHIFT) | encoding;
}

// Parse a load or store. Absolute forms take [rB...] with optional pre/post
// increment; relative forms take [rB, imm] or [imm] measured from pc + 4.
static int consume_mem(int width, bool is_absolute, bool is_load, bool* success){
  int ra = expect_register(success);
  if (ra == -1) return 0;

  struct MemOperand mem;
  if (!parse_mem_operand(is_absolute, true, &mem, success)) return 0;

  int opcode_base = is_absolute ? OP_MEM_ABS : (mem.rb != -1 ? OP_MEM_REL_REG : OP_MEM_REL_IMM);
  int instruction = ((opcode_base + MEM_WIDTH_OPCODE_STRIDE * width) << OPCODE_SHIFT) | (ra << RA_SHIFT);

  if (is_absolute){
    int encoding = encode_absolute_memory_immediate(mem.imm, success);
    assert(encoding == (encoding & 0x3FFF)); // ensure encoding is 14 bits
    instruction |= (mem.rb << RB_SHIFT) | (mem.update << MEM_ABS_MODE_SHIFT) | encoding;
  } else if (mem.rb != -1){
    int encoding = encode_memory_offset(mem.imm, MEM_REL_IMM_BITS, success);
    instruction |= (mem.rb << RB_SHIFT) | encoding;
  } else {
    instruction |= encode_memory_offset(mem.imm, MEM_REL_LONG_IMM_BITS, success);
  }

  if (is_load) instruction |= 1 << (mem.rb != -1 ? MEM_LOAD_BIT : MEM_REL_IMM_LOAD_BIT);
  return instruction;
}

// Parse the immediate operand of a relative branch.
static int encode_branch_target(int cond, bool* success){
  enum ConsumeResult result;
  long imm = consume_immediate(&result, NULL);
  if (result != FOUND){
    print_error();
    if (result == NOT_FOUND) fprintf(stderr, "Branch instruction expects register or immediate operand\n");
    *success = false;
    return 0;
  }
  return (OP_BRANCH_IMM << OPCODE_SHIFT) | (cond << RA_SHIFT) | encode_branch_immediate(imm, success);
}

// Parse "bCC imm", "bCC rA, rB", or "bCC rB" (rA = r0). Absolute branches
// ("...a" mnemonics) only have the register form.
static int consume_branch(int cond, bool is_absolute, bool* success){
  int ra = consume_register();
  if (ra == -1){
    if (!is_absolute) return encode_branch_target(cond, success);

    enum ConsumeResult result;
    consume_immediate(&result, NULL);
    print_error();
    if (result == NOT_FOUND) {
      fprintf(stderr, "Branch instruction expects register or immediate operand\n");
    } else if (result == FOUND) {
      fprintf(stderr, "Immediate branch is not allowed for absolute branches\n");
    }
    *success = false;
    return 0;
  }

  int rb = consume_register();
  if (rb == -1){
    // ra was omitted
    rb = ra;
    ra = 0;
  }
  int opcode = is_absolute ? OP_BRANCH_ABS_REG : OP_BRANCH_REL_REG;
  return (opcode << OPCODE_SHIFT) | (cond << RA_SHIFT) | (ra << BRANCH_REG_RA_SHIFT) | rb;
}

// jmp imm is br imm; jmp rB is bra rB (absolute).
static int consume_jmp(bool* success){
  int rb = consume_register();
  if (rb == -1) return encode_branch_target(COND_ALWAYS, success);
  return (OP_BRANCH_ABS_REG << OPCODE_SHIFT) | (COND_ALWAYS << RA_SHIFT) | rb;
}

// Parse "adpc rA, imm": rA = pc + 4 + imm.
static int consume_adpc(bool* success){
  int ra = expect_register(success);
  if (ra == -1) return 0;

  enum ConsumeResult result;
  long imm = consume_immediate(&result, NULL);
  if (result != FOUND){
    print_error();
    if (result == NOT_FOUND) fprintf(stderr, "adpc expects immediate or label\n");
    *success = false;
    return 0;
  }

  int encoding = encode_signed_field(imm, ADPC_IMM_BITS, "adpc immediate", success);
  return (OP_ADPC << OPCODE_SHIFT) | (ra << RA_SHIFT) | encoding;
}

// Parse "fad[a]/swp[a] rA, rC, [...]": rA = old memory value, memory updated
// from rC. No pre/post increment forms exist.
static int consume_atomic(bool is_absolute, bool is_fadd, bool* success){
  int ra = expect_register(success);
  if (ra == -1) return 0;
  int rc = expect_register(success);
  if (rc == -1) return 0;

  struct MemOperand mem;
  if (!parse_mem_operand(is_absolute, false, &mem, success)) return 0;

  int opcode = is_fadd ? OP_FADA : OP_SWPA;
  if (!is_absolute) opcode += (mem.rb != -1) ? 1 : 2;
  int instruction = (opcode << OPCODE_SHIFT) | (ra << RA_SHIFT) | (rc << RB_SHIFT);

  if (mem.rb != -1){
    instruction |= (mem.rb << ATOMIC_RB_SHIFT) | encode_memory_offset(mem.imm, ATOMIC_IMM_BITS, success);
  } else {
    instruction |= encode_memory_offset(mem.imm, ATOMIC_LONG_IMM_BITS, success);
  }
  return instruction;
}

// Return the fixed bits of a privileged instruction.
static int priv_base(int id, int subop){
  return (OP_PRIV << OPCODE_SHIFT) | (id << PRIV_ID_SHIFT) | (subop << PRIV_SUBOP_SHIFT);
}

// Parse tlbr rA, rB / tlbw rA, rB / tlbi rB / tlbc. tlb_op is the sub-op.
static int consume_tlb_op(int tlb_op, bool* success){
  if (!check_privileges(success)) return 0;
  assert(0 <= tlb_op && tlb_op < 4); // ensure tlb op is valid

  int instruction = priv_base(PRIV_TLB, tlb_op);
  enum { TLBR = 0, TLBW = 1, TLBI = 2, TLBC = 3 };
  if (tlb_op == TLBC) return instruction;

  int ra = 0;
  if (tlb_op != TLBI){
    ra = expect_register(success);
    if (ra == -1) return 0;
  }
  int rb = expect_register(success);
  if (rb == -1) return 0;
  return instruction | (ra << RA_SHIFT) | (rb << RB_SHIFT);
}

// Parse crmv with any mix of general and control registers.
static int consume_crmv(bool* success){
  if (!check_privileges(success)) return 0;

  enum CrmvForm form;
  int ra = consume_register();
  int rb;
  if (ra == -1){
    ra = consume_control_register();
    if (ra == -1){
      print_error();
      fprintf(stderr, "Invalid register or control register\nValid registers are r0 - r31 and cr0 - cr9, cr12 (cr10 and cr11 are reserved)\n");
      *success = false;
      return 0;
    }
    rb = consume_control_register();
    form = CRMV_CR_FROM_CR;
    if (rb == -1){
      rb = consume_register();
      form = CRMV_CR_FROM_REG;
      if (rb == -1){
        print_error();
        fprintf(stderr, "Invalid control register\nValid control registers are cr0 - cr9, cr12 (cr10 and cr11 are reserved)\n");
        *success = false;
        return 0;
      }
    }
  } else {
    rb = consume_control_register();
    form = CRMV_REG_FROM_CR;
    if (rb == -1){
      rb = consume_register();
      form = CRMV_REG_FROM_REG;
      if (rb == -1){
        print_error();
        fprintf(stderr, "Invalid register or control register\nValid registers are r0 - r31 and cr0 - cr9, cr12 (cr10 and cr11 are reserved)\n");
        *success = false;
        return 0;
      }
    }
  }
  return priv_base(PRIV_CRMV, form) | (ra << RA_SHIFT) | (rb << RB_SHIFT);
}

// Parse "eoi all" or "eoi n" (clear ISR bit n).
static int consume_eoi(bool* success){
  if (!check_privileges(success)) return 0;
  int instruction = priv_base(PRIV_EOI, 0);

  skip();
  if (consume_keyword("all")) return instruction | (1 << PRIV_ALL_BIT);

  enum ConsumeResult result;
  long imm = consume_immediate(&result, NULL);
  if (result != FOUND) {
    print_error();
    fprintf(stderr, "eoi instruction expects 'all' or an ISR bit index in range 0 to 15\n");
    *success = false;
    return 0;
  }
  if (imm < 0 || imm > EOI_MAX_BIT) {
    print_error();
    fprintf(stderr, "eoi bit index must be in range 0 to 15\n");
    fprintf(stderr, "Got %ld\n", imm);
    *success = false;
    return 0;
  }
  return instruction | (int)imm;
}

// Parse "mode run|sleep|halt". ISA.md documents sleep (1) and halt (2);
// run (0) is accepted by the assembler but is not listed there.
static int consume_mode_op(bool* success){
  if (!check_privileges(success)) return 0;

  int mode;
  if (consume("run")) mode = 0;
  else if (consume("sleep")) mode = 1;
  else if (consume("halt")) mode = 2;
  else {
    print_error();
    fprintf(stderr, "Invalid mode\n");
    fprintf(stderr, "Valid modes are: run, sleep, or halt\n");
    *success = false;
    return 0;
  }
  return priv_base(PRIV_MODE, mode);
}

// Parse "ipi n|all": interrupt core n (or every core). ipi always succeeds
// and writes no register, so the rA field is left zero.
static int consume_ipi(bool* success){
  if (!check_privileges(success)) return 0;

  int instruction = priv_base(PRIV_IPI, 0);

  skip();
  if (consume_register() != -1){
    print_error();
    fprintf(stderr, "ipi takes no result register; expected 'ipi n' or 'ipi all', found 'ipi rA, ...'\n");
    *success = false;
    return 0;
  }
  if (consume_keyword("all")) return instruction | (1 << PRIV_ALL_BIT);

  enum ConsumeResult result;
  long core = consume_literal(&result);
  if (result != FOUND || core < 0 || core >= IPI_CORE_COUNT){
    print_error();
    if (result != ERROR) fprintf(stderr, "ipi instruction expects 'all' or core num in range [0, 3]\n");
    *success = false;
    return 0;
  }
  return instruction | (int)core;
}

// movu/movl halves; MOV_PC_RELATIVE selects the label variants.
enum { MOV_UPPER = 0, MOV_LOWER = 1, MOV_PC_RELATIVE = 2 };

// Parse movu/movl, the two halves movi and call expand to (ISA.md
// "Assembler Macros"). Label immediates select movu8/movl4; numeric
// immediates use movu/movl:
//
//   movu  := lui  rA, (imm & 0xFFFFFC00)
//   movl  := addi rA, rA, (imm & 0x3FF)
//   movu8 := lui  rA, ((imm - 8) & 0xFFFFFC00)
//   movl4 := addi rA, rA, ((imm - 4) & 0x3FF)
//
// imm is measured from each half's own pc, so subtracting 8 and 4 makes both
// halves compute label - (pc_of_movu + 12).
static int consume_mov_half(int mov_type, bool* success){
  int ra = expect_register(success);
  if (ra == -1) return 0;

  enum ConsumeResult result;
  enum OperandKind kind;
  long imm = consume_immediate(&result, &kind);
  if (result != FOUND){
    print_error();
    if (result == NOT_FOUND) fprintf(stderr, "movi expects label or integer literal\n");
    *success = false;
    return 0;
  }
  if (kind == OPERAND_LABEL || kind == OPERAND_DEFERRED) {
    mov_type |= MOV_PC_RELATIVE;
    imm -= (mov_type & MOV_LOWER) ? 4 : 8;
  }

  if (mov_type & MOV_LOWER){
    int encoding = encode_arithmetic_immediate(imm & 0x3FF, success);
    return (OP_ALU_IMM << OPCODE_SHIFT) | (ra << RA_SHIFT) | (ra << RB_SHIFT) |
           (ALU_ADD << ALU_IMM_OP_SHIFT) | encoding;
  }
  int encoding = encode_lui_immediate(imm & 0xFFFFFC00, success);
  return (OP_LUI << OPCODE_SHIFT) | (ra << RA_SHIFT) | encoding;
}

// ---- Mnemonic table ------------------------------------------------------

// Parser selected by a mnemonic.
enum InsnForm {
  FORM_NONE = 0,
  FORM_ALU, FORM_CMP, FORM_LUI, FORM_MEM, FORM_BRANCH, FORM_JMP, FORM_ADPC,
  FORM_TRAP, FORM_ATOMIC, FORM_TLB, FORM_CRMV, FORM_MODE, FORM_RFE, FORM_IPI,
  FORM_EOI, FORM_MOV_HALF,
};

// Mnemonic flags.
enum { F_ABSOLUTE = 1, F_LOAD = 2, F_FADD = 4 };

// How to parse one mnemonic: its form, the form-specific operation number
// (ALU op, width, branch condition, ...), and flags.
struct Mnemonic {
  uint8_t form;
  uint8_t op;
  uint8_t flags;
};

#define ALU(kw, op) [kw] = {FORM_ALU, op, 0}
#define MEM(store, load, store_abs, load_abs, width) \
  [store] = {FORM_MEM, width, 0}, [load] = {FORM_MEM, width, F_LOAD}, \
  [store_abs] = {FORM_MEM, width, F_ABSOLUTE}, [load_abs] = {FORM_MEM, width, F_ABSOLUTE | F_LOAD}
#define BRANCH(rel, abs, cond) [rel] = {FORM_BRANCH, cond, 0}, [abs] = {FORM_BRANCH, cond, F_ABSOLUTE}

static const struct Mnemonic kMnemonics[KW_COUNT] = {
  ALU(KW_AND, ALU_AND), ALU(KW_NAND, ALU_NAND), ALU(KW_OR, ALU_OR), ALU(KW_NOR, ALU_NOR),
  ALU(KW_XOR, ALU_XOR), ALU(KW_XNOR, ALU_XNOR), ALU(KW_NOT, ALU_NOT),
  ALU(KW_LSL, ALU_LSL), ALU(KW_LSR, ALU_LSR), ALU(KW_ASR, ALU_ASR),
  ALU(KW_ROTL, ALU_ROTL), ALU(KW_ROTR, ALU_ROTR), ALU(KW_LSLC, ALU_LSLC), ALU(KW_LSRC, ALU_LSRC),
  ALU(KW_ADD, ALU_ADD), ALU(KW_ADDC, ALU_ADDC), ALU(KW_SUB, ALU_SUB), ALU(KW_SUBB, ALU_SUBB),
  ALU(KW_SXTB, ALU_SXTB), ALU(KW_SXTD, ALU_SXTD), ALU(KW_TNCB, ALU_TNCB), ALU(KW_TNCD, ALU_TNCD),
  [KW_CMP] = {FORM_CMP, 0, 0},
  [KW_LUI] = {FORM_LUI, 0, 0},
  MEM(KW_SW, KW_LW, KW_SWA, KW_LWA, WIDTH_WORD),
  MEM(KW_SD, KW_LD, KW_SDA, KW_LDA, WIDTH_DOUBLE),
  MEM(KW_SB, KW_LB, KW_SBA, KW_LBA, WIDTH_BYTE),
  BRANCH(KW_BR, KW_BRA, COND_ALWAYS),
  BRANCH(KW_BZ, KW_BZA, COND_Z), BRANCH(KW_BNZ, KW_BNZA, COND_NZ),
  BRANCH(KW_BS, KW_BSA, COND_S), BRANCH(KW_BNS, KW_BNSA, COND_NS),
  BRANCH(KW_BC, KW_BCA, COND_C), BRANCH(KW_BNC, KW_BNCA, COND_NC),
  BRANCH(KW_BO, KW_BOA, COND_O), BRANCH(KW_BNO, KW_BNOA, COND_NO),
  BRANCH(KW_BPS, KW_BPA, COND_PS), BRANCH(KW_BNPS, KW_BNPA, COND_NPS),
  BRANCH(KW_BG, KW_BGA, COND_G), BRANCH(KW_BGE, KW_BGEA, COND_GE),
  BRANCH(KW_BL, KW_BLA, COND_L), BRANCH(KW_BLE, KW_BLEA, COND_LE),
  BRANCH(KW_BA, KW_BAA, COND_A), BRANCH(KW_BAE, KW_BAEA, COND_AE),
  BRANCH(KW_BB, KW_BBA, COND_B), BRANCH(KW_BBE, KW_BBEA, COND_BE),
  [KW_JMP] = {FORM_JMP, 0, 0},
  [KW_ADPC] = {FORM_ADPC, 0, 0},
  [KW_TRAP] = {FORM_TRAP, 0, 0},
  [KW_FADA] = {FORM_ATOMIC, 0, F_ABSOLUTE | F_FADD},
  [KW_FAD] = {FORM_ATOMIC, 0, F_FADD},
  [KW_SWPA] = {FORM_ATOMIC, 0, F_ABSOLUTE},
  [KW_SWP] = {FORM_ATOMIC, 0, 0},
  [KW_TLBR] = {FORM_TLB, 0, 0}, [KW_TLBW] = {FORM_TLB, 1, 0},
  [KW_TLBI] = {FORM_TLB, 2, 0}, [KW_TLBC] = {FORM_TLB, 3, 0},
  [KW_CRMV] = {FORM_CRMV, 0, 0},
  [KW_MODE] = {FORM_MODE, 0, 0},
  [KW_RFE] = {FORM_RFE, 0, 0},
  [KW_IPI] = {FORM_IPI, 0, 0},
  [KW_EOI] = {FORM_EOI, 0, 0},
  [KW_MOVU] = {FORM_MOV_HALF, MOV_UPPER, 0},
  [KW_MOVL] = {FORM_MOV_HALF, MOV_LOWER, 0},
};

#undef ALU
#undef MEM
#undef BRANCH

// consumes a single instruction and converts it to binary or hex
int consume_instruction(enum ConsumeResult* result){
  skip();

  // One token hash finds the mnemonic. Prefixes such as "add"/"addc" stay
  // distinct because the matcher consumes the whole identifier.
  enum KeywordId id = take_keyword(KW_CLASS_MNEMONIC);
  const struct Mnemonic m = kMnemonics[id];
  bool absolute = (m.flags & F_ABSOLUTE) != 0;
  bool success = true;
  int instruction = 0;

  switch ((enum InsnForm)m.form) {
    case FORM_ALU: instruction = consume_alu_op(m.op, &success); break;
    case FORM_CMP: instruction = consume_cmp(&success); break;
    case FORM_LUI: instruction = consume_lui(&success); break;
    case FORM_MEM: instruction = consume_mem(m.op, absolute, (m.flags & F_LOAD) != 0, &success); break;
    case FORM_BRANCH: instruction = consume_branch(m.op, absolute, &success); break;
    case FORM_JMP: instruction = consume_jmp(&success); break;
    case FORM_ADPC: instruction = consume_adpc(&success); break;
    case FORM_TRAP: instruction = OP_TRAP << OPCODE_SHIFT; break;
    case FORM_ATOMIC: instruction = consume_atomic(absolute, (m.flags & F_FADD) != 0, &success); break;
    case FORM_TLB: instruction = consume_tlb_op(m.op, &success); break;
    case FORM_CRMV: instruction = consume_crmv(&success); break;
    case FORM_MODE: instruction = consume_mode_op(&success); break;
    case FORM_RFE:
      if (check_privileges(&success)) instruction = priv_base(PRIV_RFE, 0);
      break;
    case FORM_IPI: instruction = consume_ipi(&success); break;
    case FORM_EOI: instruction = consume_eoi(&success); break;
    case FORM_MOV_HALF: instruction = consume_mov_half(m.op, &success); break;
    case FORM_NONE:
      *result = NOT_FOUND;
      return 0;
  }

  if (!success) *result = ERROR;
  return instruction;
}
