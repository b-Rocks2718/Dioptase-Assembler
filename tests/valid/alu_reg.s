    # test different registers
    # test all opcodes
_start:
    and  r0, r1, r2
    nand r3, r4, r5
    or   r6, r7, r8
    nor  r9, r10, r11
    xor  r11, r12, r13
    xnor r14, r15, r16
    not  r17, r18
    lsl  r19, r20, r21
    lsr  r22, r23, r24
    asr  r25, r26, r27
    rotl r28, r29, r30
    rotr r31, r0, r0
    lslc r0, r0, r0
    lsrc r0, r0, r0
    add  r0, r0, r0
    addc r0, r0, r0
    sub  r0, r0, r0
    subb r0, r0, r0
    cmp  r1, r2
