
  .define LOCAL 0x67

  .global _start
_start:
  nop
  push r2
  pop  r2
  pshw r2
  popw r2
  pshd r2
  popd r2
  pshb r2
  popb r2
label:
  mov r2, r3
  mov cr2, r3
  mov r2, cr3
  mov cr2, cr3
  ret
  movi r1, BIG
  movi r2, ONE
  movi r2, LOCAL
  call label
  jmp  label
  jmp  r2
