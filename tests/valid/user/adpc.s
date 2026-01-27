    .text

    .global _start
_start:
    adpc r1, 0
    adpc r2, 4
    adpc r3, -4
    adpc r4, label
label:
    nop
