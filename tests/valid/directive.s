    # Test all directives but .global
    # .global is tested separately
    .kernel
_start:
    .fill 0xAAAA5555
    .space 3
DATA:
    .fill 0x12345
    
    tlbc

    .origin 128

    .fill 0x42

test: