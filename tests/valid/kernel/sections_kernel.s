# Kernel section layout test with implicit + explicit sections.
.global _start
_start:
  .fill 0x11111111

.text
text_label:
  add r0, r0, r0

.rodata
ro_label:
  .fill 0x33333333

.data
data_label:
  .fill 0x44444444

.bss
bss_label:
  .space 4
