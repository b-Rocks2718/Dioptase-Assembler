# Dioptase Assembler

Assembler for the [Dioptase Architecture](https://github.com/b-Rocks2718/Dioptase/tree/main)  

See [ISA](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/ISA.md) for the instruction set and [Syntax](https://github.com/b-Rocks2718/Dioptase-Assembler/blob/main/docs/synatx.md) for all of the syntax and macros supported.  

## Usage

Build the assembler with `make assembler`

Run it with `./build/assembler <flags> <source files>`

Remove all generated files but the final executable with `make clean`

Remove everything with `make purge`

#### Supported Flags:  
`-pre` if you wish to generate a file containing the output of the preprocessor (all macros expanded)  
`-bin` if you wish to generate a binary file as output instead of a `.hex` file. The assembler generates a text file containing the instructions as hex by default.  
`-both` if you want to generate the `.bin` and `.hex` files  

## Extending the assembler

To add a new instruction, you will need to write a function that consumes that instruction and outputs the corresponding integer. assembler.c contains useful functions to consume registers, immediates, and other tokens. Then edit `consume_instruction` in assembler.c. Add a new branch to the massive if-statement to detect the keyword for your new instruction, then call your consume function. 
Be aware that adding a new instruction will require changes to the emulator and verilog to go with it.  

To add a new macro, modify the branch in `expand_macros` in preprocessor.c to detect the new keyword, then use a format string for the macro's expansion. Don't put newline characters in the macro expansions or the assembler may give incorrect error messages.  

Please add some tests for any extensions

## Testing

Run all tests with `make test`

To add a new test, add a `.s` source file and a `.ok` hex file of the same name to the `tests` directory. The `.ok` file should contain the expected assembler output.

To run a specific test case, use `make <example>.test` where you replace `<example>` with the name of your test. 
