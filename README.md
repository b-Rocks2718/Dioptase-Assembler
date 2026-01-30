# Dioptase Assembler

Assembler for the [Dioptase Architecture](https://github.com/b-Rocks2718/Dioptase/tree/main)  

See [ISA](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/ISA.md) for the instruction set and [Syntax](https://github.com/b-Rocks2718/Dioptase-Assembler/blob/main/docs/synatx.md) for all of the syntax and macros supported.  

## Usage

Build the assembler with `make all`

Run it with `./build/assembler <flags> <source files>`

Remove all generated files but the final executable with `make clean`

Remove everything with `make purge`

For user programs, the assembler expects a global `_start` label to be defined in one of the source files. This is the entry point. 

#### Supported Flags 
`-pre` if you wish to print the output of the preprocessor (can be useful for debugging)  
`-o` to name the output file (./a.hex is the default)  
`-bin` to write a raw binary image instead of hex words (default output becomes ./a.bin)  
`-g` to output debug info  
`-kernel` to allow the use of privileged instructions (normally disallowed) and output a kernel-mode hex file instead of an ELF hex file  
`-crt` to prepend `crt/crt0.s` and `crt/arithmetic.s` to the input list so `_start` is emitted first  

Notes on `-bin`:
- Output is little-endian bytes instead of text hex.
- `-bin` is not compatible with `-g` (debug labels are emitted as text).
- For kernel builds, `.origin` gaps are zero-filled so the binary is a flat memory image starting at address 0.

Kernel section layout:
- `.text`, `.rodata`, `.data`, and `.bss` are supported in kernel mode.
- Any content before the first explicit section directive goes into an implicit section.
- The implicit section is placed first; the other sections follow in the fixed order `.text`, `.rodata`, `.data`, `.bss`.
- Each section is padded to a multiple of 512 bytes before concatenation.
- A final end section is emitted after `.bss` with a single `0xAAAAAAAA` word; its address can be used to compute the padded `.bss` size.
- `.bss` does not emit bytes in kernel mode; it only advances the size.
- `.origin` is only allowed in kernel mode and only before selecting an explicit section.

## Syntax Highlighting

cd into the `dioptase-assembly` directory and run

```bash
code --install-extension dioptase-assembly-0.0.9.vsix
```

for VSCode syntax highlighting

## Testing

Run all tests with `make test`
