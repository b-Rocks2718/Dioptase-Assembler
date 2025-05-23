# Dioptase Assembler

Assembler for the [Dioptase Architecture](https://github.com/b-Rocks2718/Dioptase/tree/main)  

See [ISA](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/ISA.md) for the instruction set and [Syntax](https://github.com/b-Rocks2718/Dioptase-Assembler/blob/main/docs/synatx.md) for all of the syntax and macros supported.  

## Usage

Build the assembler with `make assembler`

Run it with `./build/assembler <flags> <source files>`

Remove all generated files but the final executable with `make clean`

Remove everything with `make purge`

#### Supported Flags:  
`-preprocessed` if you wish to generate a file containing the output of the preprocessor (all macros expanded)  
`-bin` if you wish to generate a binary file as output instead of a `.hex` file. The assembler generates a text file containing the instructions as hex by default.  
`-both` if you want to generate the `.bin` and `.hex` files  

## Extending the assembler

To add a new instruction, ...  
Be aware that adding a new instruction will require changes to the emulator and verilog to go with it.

To add a new macro, ... 

Please add some tests for any extensions

## Testing

Run all tests with `make test`

To add a new test, add a `.s` source file and a `.ok` hex file of the same name to the `tests` directory. The `.ok` file should contain the expected assembler output.

To run a specific test case, use `make <example>.test` where you replace `<example>` with the name of your test. 
