    # A .define followed by another statement on the same line: both passes
    # must assemble the trailing add, or the label after it is misplaced.
    .text

    .global _start
_start:
    .define X 5; add r1, r2, X
    br end
end:
    add r3, r4, 1
