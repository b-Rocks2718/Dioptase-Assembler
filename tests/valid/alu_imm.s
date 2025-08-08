    # test different registers
    # test arithmetic immediate encoding
    add r0, r2, 11
    add r3, r4, 0b11
    add r5, r6, 0o11
loop:
    add r7, r8, 0x11
    add r9, r10, -11
    add r11, r12, -0b11
    add r13, r14, -0o11
    add r15, r16, -1
    add r17, r18, loop

    # test shift immediate encoding
    lsl r1, r19, 11
    lsl r20, r21, 0

    # test bitwise immediate encoding
    and r22, r23, 0xAF
    and r24, r25, 0xAF00
    and r26, r27, 0xAF0000
    and r28, r29, 0xAF000000

    # test all opcodes
    nand r30, r31, 0
    or   r0, r0, 0
    nor  r0, r0, 0
    xor  r0, r0, 0
    xnor r0, r0, 0
    not  r0, 0
    lsl  r0, r0, 0
    lsr  r0, r0, 0
    asr  r0, r0, 0
    rotl r0, r0, 0
    rotr r0, r0, 0
    lslc r0, r0, 0
    lsrc r0, r0, 0
    add  r0, r0, 0
    addc r0, r0, 0
    sub  r0, r0, 0
    subb r0, r0, 0
    mul  r0, r0, 0
