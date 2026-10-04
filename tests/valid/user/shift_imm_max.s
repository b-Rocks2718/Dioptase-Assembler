    # Shift immediates are a 5-bit field (ISA.md "Shifts"), so 31 is the
    # largest legal amount and must assemble for every shift mnemonic.
    .text

    .global _start
_start:
    lsl  r1, r2, 31
    lsr  r3, r4, 31
    asr  r5, r6, 31
    rotl r7, r8, 31
    rotr r9, r10, 31
    lslc r11, r12, 31
    lsrc r13, r14, 31
