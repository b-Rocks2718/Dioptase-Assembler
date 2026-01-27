# Dioptase Assembler

Assembler for the [Dioptase Architecture](https://github.com/b-Rocks2718/Dioptase/tree/main)  

See [ISA](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/ISA.md) for the instruction set and [Syntax](https://github.com/b-Rocks2718/Dioptase-Assembler/blob/main/docs/synatx.md) for all of the syntax and macros supported.  

## Usage

Build the assembler with `make all`

Run it with `./build/assembler <flags> <source files>`

Remove all generated files but the final executable with `make clean`

Remove everything with `make purge`

Assembler expects a global `_start` label to be defined in one of the source files. For user mode files, `_start` is the entry point. For kernel mode files, `_start` is where the kernel begins execution on boot.

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

## Syntax Highlighting

cd into the `dioptase-assembly` directory and run

```bash
code --install-extension dioptase-assembly-0.0.9.vsix
```

for VSCode syntax highlighting

## Testing

Run all tests with `make test`
