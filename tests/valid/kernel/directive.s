    # Test all directives but .global
    # .global is tested separately

.define NUM 10
    .origin 0x400
    .global _start
_start:
    .fill 0xAAAA5555
    .fild 0xBEEF
    .fild 0xDEAD
    .fild 0xAAAA
    .filb 0x7F
    .filb 0x80
    .space 12
DATA:
    .fill 0x12345
    
    tlbc

    .origin 0x500

    .fill 0x42
    .filb 0xFF
    .align 4
    movi r3, NUM
    add r0, r0, NUM

test: