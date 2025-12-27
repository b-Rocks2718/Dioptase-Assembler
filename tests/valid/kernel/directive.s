    # Test all directives but .global
    # .global is tested separately

.define NUM 10

    .global _start
_start:
    .fill 0xAAAA5555
    .space 3
DATA:
    .fill 0x12345
    
    tlbc

    .origin 0x500

    .fill 0x42
    movi r3, NUM
    add r0, r0, NUM

test: