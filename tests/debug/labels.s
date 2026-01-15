.text
.global _start
_start:
  add r0, r0, r0
  br mid_text

text_loop:
  add r1, r1, 1

mid_text:
  add r2, r2, 2

.rodata
const_a:
  .fill 0x11111111
const_b:
  .fill 0x22222222

.text
after_ro:
  add r3, r3, 3

.data
data_a:
  .fill 0x33333333
data_b:
  .space 8

.text
after_data:
  add r4, r4, 4

.bss
bss_a:
  .space 16

.text
done:
  add r5, r5, 5
