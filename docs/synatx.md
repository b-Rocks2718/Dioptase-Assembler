# Syntax

### General Info

Put each instruction on a new line  

Labels are defined by any string of non-whitespace characters and are terminated with a colon.  

Ex: `label.example:`

Comments begin with a `#` and continue until the end of the line

Ex: `add r1 r2 r3 # this is a comment`

Valid registers: `r0` - `r31`  
`r0` is enforced by the hardware to be identically 0  
`sp` (stack pointer) is an alias for `r1`  
`bp` (base pointer)  is an alias for `r2`  
`ra` (return address) is an alias for `r31`

Valid control registers:

`cr0` - `cr6`  
`PSR` (processor status register) is an alias for `cr0`  
`PID` (process ID) is an alias for `cr1`  
`ISR` (interrupt status register) is an alias for `cr2`  
`IMR` (interrupt mask register) is an alias for `cr3`  
`EPC` (exceptional PC) is an alias for `cr4`  
`EFG` (exceptional flags) is an alias for `cr5`  
`CDV` (clock divider) is an alias for `cr6`  

### RRR Instructions

Instructions that take two register operands have have one register target  

Syntax is `op rA, rB, rC` where op is a valid RRR instruction or macro. `rA` will be the target, `rB` and `rC` are inputs. Commas between things are optional.

### RRI Instructions

Instructions that take a register and an immediate operand, and target a register.

Syntax is `op rA, rB, imm`, with `rA` the target and `rB` and `imm` the operands. `imm` can be in base 2 (prefix with 0b), 10, or 16 (prefix with 0x), and can be prefixed with a minus to indicate it should be negated. `imm` could also be a label, assuming that the label is defined somewhere in the file. The label will be converted to a pc-relative offset and an error will be raised if this value does not fit in the instruction encoding.

Examples:  `addi r1, r0, 10`, `addi r1, r0, 0xA`, and `addi r1, r0, 0b1010` are all equivalent. Something like `addi r1, r0, -1` is also allowed, and so is `swr r1, [r2, data_0]`

Note that the immediates that are possible to be encoded varies per instruction. The assembler will throw an error if you give an instruction an immediate it cannot encode. See the [ISA](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/ISA.md) for info on what immediates are supported for each instruction. 

#### Memory Instructions

Memory instructions are special because they are RRR instructions that have the option to do a preincrement or postincrement. In addition, when a register is used as an address, it must be enclosed in square brackets.

Examples:  
`lw r1, [r2]` no offset  
`lw r1, [r2, imm]` signed offset  
`lw r1, [r2, imm]!` preincrement  
`lw r1, [r2], imm` postincrement  

The syntax for `sw` is analagous. 

For `swr` and `lwr`, the only offset type allowed is signed immediate. The register address can be optionally ommitted, and `r0` will be used.

Example: `swr r1, [r0, label]` and `swr r1, label` are equivalent.

### RI Instructions

Just lui for now

`lui rA imm`

### I Instructions

PC-relative branches, such as `bnz loop` where `loop` is a label defined somewhere

### RR Instructions

Branch and link, such as `bl r1, r2`

### Syscalls

`sys CODE` where `CODE` is a valid syscall. For now, the only one is `EXIT`

## Macros

`push rA` - Alias for `sw rA [sp, -4]!` (decrement sp, then store)

`pop rA` - Alias for `lw rA, [sp], 4` (load, then increment sp)


`movi rA, imm` - Alias for
```
lui rA, (imm & 0xFFFFFC00)
addi rA, rA, (imm & 0x3FF)
```

`call imm` - Alias for
```
movi r31, imm
brl  r31, r31
```


`ret` - Alias for  `brl r0, r31`

`.fill imm` - zero extends `imm` to 32 bits, then places the value in the binary at the location of the `.fill`

`.space n` - expands to `.fill 0`, repeated `n` times
