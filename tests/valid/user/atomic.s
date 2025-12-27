    # test addressing modes for atomic operations
    .text

    .global _start
_start:
    # absolute addressing
    fada r1, r4, [r2]        # no offset  
    fada r1, r4, [r2, 3]     # signed offset    

    swpa r1, r4, [r2]        # no offset  
    swpa r1, r4, [r2, 3]     # signed offset  

    # pc-relative addressing
    fad  r1, r4, [r2, 1]     # signed offset
    fad  r1, r4, [r2, -4]    # signed offset

    swp  r1, r4, [r2]
    swp  r1, r4, [r2, 1]

    swp  r1, r4, [37]
    fad  r1, r4, [-3]
