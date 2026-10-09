    .text

    .global _start
_start:
    adpc r1, data + 8
    adpc r2, data - 4
    lw r3, [data + 4]
    sw r3, [data - 4]
    lb r4, [r5, data + 9]
    movi r6, data + 12
    movi r7, data-4
    bz target + 4
    jmp target - 4
target:
    nop
    nop
    .data
    .fill 1
data:
    .fill 2
    .fill 3
    .fill 4
    .fill 5
    .fill data + 4
