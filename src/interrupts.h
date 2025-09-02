
struct InterruptEntry {
  char* name;
  int addr;
};

extern struct InterruptEntry interrupts[];

extern int NUM_INTERRUPTS;
