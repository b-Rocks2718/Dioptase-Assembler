#ifndef INTERRUPTS_H
#define INTERRUPTS_H

struct InterruptEntry {
  char* name;
  int addr;
};

extern struct InterruptEntry interrupts[];

extern int NUM_INTERRUPTS;

#endif  // INTERRUPTS_H
