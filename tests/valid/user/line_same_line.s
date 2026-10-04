    # .line/.local followed by another statement on the same line: both passes
    # must assemble the trailing instruction so later labels stay in place.
    .text

    .global _start
_start:
    .line main.c 3; add r1, r2, 1
    .local x -4 4; add r5, r6, 2
    br end
end:
    add r3, r4, 1
