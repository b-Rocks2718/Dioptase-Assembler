#include "interrupts.h"

// needs to match the length of the array
// there's probably a better way to do this
int NUM_INTERRUPTS = 21;

struct InterruptEntry interrupts[] = {
  {"EXIT", 0},
  {"EXC_INSTR", 0x80},
  {"EXC_PRIV", 0x81},
  {"TLB_UMISS", 0x82},
  {"TLB_KMISS", 0x83},
  {"EXC_ALIGN", 0x84},
  {"INT_0", 0xF0},
  {"INT_1", 0xF1},
  {"INT_2", 0xF2},
  {"INT_3", 0xF3},
  {"INT_4", 0xF4},
  {"INT_5", 0xF5},
  {"INT_6", 0xF6},
  {"INT_7", 0xF7},
  {"INT_8", 0xF8},
  {"INT_9", 0xF9},
  {"INT_10", 0xFA},
  {"INT_11", 0xFB},
  {"INT_12", 0xFC},
  {"INT_13", 0xFD},
  {"INT_14", 0xFE},
  {"INT_15", 0xFF},
};
