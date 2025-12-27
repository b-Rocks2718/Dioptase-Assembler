
EXIT:
  mode halt

  .origin 0x500
  .global _start
_start:
  movi r3, 21
  sys EXIT