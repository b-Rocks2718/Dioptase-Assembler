# Dioptase Assembler

Assembler for the [Dioptase Architecture](https://github.com/b-Rocks2718/Dioptase/tree/main)  

See [ISA](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/ISA.md) for the instruction set and [Syntax](https://github.com/b-Rocks2718/Dioptase-Assembler/blob/main/docs/synatx.md) for all of the syntax and macros supported.  

## Usage

Build the assembler with `make all`

Run it with `./build/assembler <flags> <source files>`

Remove all generated files but the final executable with `make clean`

Remove everything with `make purge`

#### Supported Flags 
`-pre` if you wish to print the output of the preprocessor (can be useful for debugging)  
`-o` to name the output file (./a.hex is the default)  
`-nostart` if your program does not define a start label to jump to (execution will begin at address 0)  
`--debug` to append `#label` annotations (label name + address) to the output and emit a `.debug` file instead of `.hex`  

## Syntax Highlighting

cd into the `dioptase-assembly` directory and run

```bash
code --install-extension dioptase-assembly-0.0.5.vsix
```

for VSCode syntax highlighting

## Testing

Run all tests with `make test`

To run a specific test case, use `make <example>.test` where you replace `<example>` with the name of your test. 
