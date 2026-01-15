# Syntax

### General Info

Put each instruction on a new line. Semicolons between instructions is optional.    

Labels are defined by any string that begins with a letter or underscore and is followed by any sequence of letters, numbers, underscores, or periods.
To define a label, append a colon to the end.

Example label definition: `label.example:`  
Example label use: `bnz label.example`  

Labels are local by default. To make labels global, use `.global`  

Example: `.global label.example`

Comments begin with a `#` and continue until the end of the line

Ex: `add r1 r2 r3 # this is a comment`

Valid registers: `r0` - `r31`  
`r0` is enforced by the hardware to be identically 0  
`sp` (stack pointer) is an alias for `r31`  
`bp` (base pointer)  is an alias for `r30`  
`ra` (return address) is an alias for `r29`

Valid control registers:

`cr0` - `cr8`  
`psr` (processor status register) is an alias for `cr0`  
`pid` (process ID) is an alias for `cr1`  
`isr` (interrupt status register) is an alias for `cr2`  
`imr` (interrupt mask register) is an alias for `cr3`  
`epc` (exceptional PC) is an alias for `cr4`  
`flg` (flags) is an alias for `cr5`  
`cdv` (clock divider) is an alias for `cr6`  
`tlb` (translation lookaside buffer) is an alias for `cr7`
`ksp` (kernel stack pointer) is an alias for `cr8`

You can pass any nonzero number of .s files into the assembler and it will produce a single `.hex` file.
Whenever the assembler is called, exactly one of the files passed in must contain a `_start` label.

### RRR Instructions

Instructions that take two register operands have have one register target  

Syntax is `op rA, rB, rC` where op is a valid RRR instruction or macro. `rA` will be the target, `rB` and `rC` are inputs. Commas between things are optional.

### RRI Instructions

Instructions that take a register and an immediate operand, and target a register.

Syntax is `op rA, rB, imm`, with `rA` the target and `rB` and `imm` the operands. `imm` can be in base 2 (prefix with 0b), 8 (prefix with 0o), 10, or 16 (prefix with 0x), and can be prefixed with a minus to indicate it should be negated. The case of the b, o, or x does not matter. Base 10 immediates cannot begin with a 0. `imm` could also be a label, assuming that the label is defined somewhere in the file. The label will be converted to a pc-relative offset and an error will be raised if this value does not fit in the instruction encoding.

Examples:  `add r1, r0, 10`, `add r1, r0, 0xA`, and `add r1, r0, 0b1010` are all equivalent. Something like `add r1, r0, -1` is also allowed, and so is `sw r1, [r2, data_0]`

Note that the immediates that are possible to be encoded varies per instruction. The assembler will throw an error if you give an instruction an immediate it cannot encode. See the [ISA](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/ISA.md) for info on what immediates are supported for each instruction. 

#### Memory Instructions

Memory instructions are special because they are RRI instructions that have the option to do a preincrement or postincrement. In addition, when a register is used as an address, it must be enclosed in square brackets. For `swa` and `lwa` (abosulte addressing), labels cannot be used as the immediate (use pc-relative addressing to access labels).  

Examples:  
`lwa r1, [r2]` no offset  
`lwa r1, [r2, imm]` signed offset  
`lwa r1, [r2, imm]!` preincrement  
`lwa r1, [r2], imm` postincrement  

The syntax for `swa` is analagous. 

`sw` and `lw` do pc-relative addressing.
For `sw` and `lw`, the only offset type allowed is signed immediate. The register address can be ommitted, and a different encoding will be used.

Example: `swr r1, [label]` and `swr r1, [r0, label]` are usually equivalent, but the former allows for `label` to be further away than the latter.  

### RI Instructions

Just lui for now

`lui rA imm`

### I Instructions

PC-relative branches, such as `bnz loop` where `loop` is a label defined somewhere.

### RR Instructions

Branch and link, such as `bl r1, r2`
The first operand, which specifies where the pc will be stored, can be optionally omitted. If it is, `r0` will be used.  

### Syscalls

`sys CODE` where `CODE` is a valid syscall. For now, the only one is `EXIT`

## Macros

`nop` - Alias for `and r0, r0, r0` (which is the same as `.fill 0`). Has no effect when executed.  

`push rA` - Alias for `swa rA [sp, -4]!` (decrement sp, then store)

`pop rA` - Alias for `lwa rA, [sp], 4` (load, then increment sp)

`pshw rA` - Alias for `swa rA [sp, -4]!` 

`popw rA` - Alias for `lwa rA, [sp], 4` 

`pshd rA` - Alias for `sda rA [sp, -4]!` 

`popd rA` - Alias for `lda rA, [sp], 4` 

`pshb rA` - Alias for `sba rA [sp, -4]!` 

`popb rA` - Alias for `lba rA, [sp], 4`  

`mov rA, rB` - Alias for `add rA, rB, r0` when`rA` and `rB` are not control registers. If at least one is a control register, then this is an alias for a `crmv` instruction.  

`movi rA, imm` - Alias for
```
lui rA, (imm & 0xFFFFFC00)
addi rA, rA, (imm & 0x3FF)
```

Can put any 32 bit value in a register

`call imm` - Alias for
```
movi r29, <imm - 8>
bl  r29, r29
```
Do a -8 because we want the offset from the bl instruction, not the movi

`ret` - Alias for  `jmp r29`

`jmp rA` - Alias for  `bra rA`

`jmp imm` - Alias for  `br imm`

`cmp rA, rB` - Alias for `sub r0, rA, rB`

`cmp rA, i` - Alias for `sub r0, rA, i`

## Directives

`.global label` - makes label global

`.origin a` - places the code following the directive at address `a`. Will error if you give it a value less than the current pc (can use it to forward, not backward). Only available in kernel mode.

`.text`, `.data`, `.rodata`, and `.bss` will automatically create sections starting at the next available address, and 
are only available for user mode programs.

`.fill imm` - sign extends `imm` to 32 bits, then places the value in the binary at the location of the `.fill`; `imm` may be an integer literal or a `.define`/`-D` constant (labels are not allowed)

`.fild imm` - fill 16 bit immediate

`.filb imm` - fill 8 bit immediate

`.fill`, `.fild`, and `.filb` emit bytes in little-endian order within the output word stream 

`.space n` - expands to `.filb 0`, repeated `n` times; `n` may be an integer literal or a `.define`/`-D` constant (labels are not allowed)

`.define NAME n` - macro for defining constants
