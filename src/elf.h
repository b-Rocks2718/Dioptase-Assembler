#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include "instruction_array.h"

struct ProgramDescriptor {
  uint32_t entry_point;
  struct InstructionArrayList* sections;
  uint32_t bss_size;
};

struct ElfHeader {
  unsigned char e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint32_t e_entry;
  uint32_t e_phoff;
  uint32_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
};

struct ElfProgramHeader {
  uint32_t p_type;
  uint32_t p_offset;
  uint32_t p_vaddr;
  uint32_t p_paddr;
  uint32_t p_filesz;
  uint32_t p_memsz;
  uint32_t p_flags;
  uint32_t p_align;
};

void destroy_program_descriptor(struct ProgramDescriptor* program);

struct ElfHeader create_elf_header(struct ProgramDescriptor* program);

struct ElfProgramHeader* create_PHT(struct ProgramDescriptor* program);

struct ElfProgramHeader create_text_program_header(uint32_t offset, uint32_t vaddr, uint32_t filesz);

struct ElfProgramHeader create_rodata_program_header(uint32_t offset, uint32_t vaddr, uint32_t filesz);

struct ElfProgramHeader create_data_program_header(uint32_t offset, uint32_t vaddr, uint32_t filesz, uint32_t memsz);

void fprint_elf_header(FILE* ptr, struct ElfHeader* header);

void fprint_pht(FILE* ptr, struct ElfProgramHeader* pht);

// Purpose: Write the ELF header as raw little-endian bytes.
// Inputs: ptr is the binary output; header describes the ELF header fields.
// Outputs: Writes the ELF header bytes to ptr.
// Invariants/Assumptions: ptr is open for binary output.
void fwrite_elf_header(FILE* ptr, const struct ElfHeader* header);

// Purpose: Write the program header table as raw little-endian bytes.
// Inputs: ptr is the binary output; pht points to 3 program header entries.
// Outputs: Writes the program header table bytes to ptr.
// Invariants/Assumptions: ptr is open for binary output.
void fwrite_pht(FILE* ptr, const struct ElfProgramHeader* pht);

#endif  // ELF_H
