.text
.global _start
_start:
  add r1, r0, 1
  add r2, r0, 2

.rodata
ro_val:
  .fill 0x11223344
  .fill 0x55667788

.text
text_mid:
  add r3, r0, 3

.data
data_val:
  .fill 0xAABBCCDD
  .space 8

.text
text_end:
  add r4, r0, 4

.bss
bss_buf:
  .space 16

.text
done:
  add r5, r0, 5
