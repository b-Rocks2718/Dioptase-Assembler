    # Test all directives but .global
    # .global is tested separately
    .kernel

    .fill 0xAAAA5555
    .space 3
DATA:
    .fill 0x12345
    
    tlbc

    .origin 32

    .fill 0x42
