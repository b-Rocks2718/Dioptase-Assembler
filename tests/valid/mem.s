    # test addressing modes and increment types
_start:
    # absolute addressing
    lwa r1, [r2]        # no offset  
    lwa r1, [r2, 3]     # signed offset  
    lwa r1, [r2, -4]!   # preincrement  
    lwa r1, [r2], 0x3FF8     # postincrement  

    swa r1, [r2]        # no offset  
    swa r1, [r2, 3]     # signed offset  
    swa r1, [r2, -4]!   # preincrement  
    swa r1, [r2], 0x3FF8     # postincrement  

    # pc-relative addressing
    lw  r1, [r2, 1]     # signed offset
    sw  r1, [r2, -4]    # signed offset

    sda  r1, [r2]
    sda  r1, [r2, 0]
    ld  r1, [r2, 1]

    sb  r1, [r2, -4]
    lba r1, [r2], 0x3FF8
    sb  r0, [VAR]       # immediate

VAR:
    .fill 21

    lw  r1, [VAR]       # immediate
    sw  r1, [DATA]      # immediate

DATA:
    .fill 42
