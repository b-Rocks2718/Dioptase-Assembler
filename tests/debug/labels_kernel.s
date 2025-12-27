.global _start
_start:
  add r0, r0, r0

k_loop:
  add r1, r1, 1
  br k_end

k_mid:
  add r2, r2, 2

.origin 0x600
origin_label:
  add r3, r3, 3

k_end:
  add r4, r4, 4
