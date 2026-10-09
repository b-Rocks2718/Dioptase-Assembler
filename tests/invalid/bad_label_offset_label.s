  .text

  .global _start
_start:
  adpc r1, data + other
data:
  nop
other:
  nop
