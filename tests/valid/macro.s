# TODO: make this .ok file
.kernel

_start:
  nop
  push r2
  pop r2
label:
  mov r2, r3
  mov cr2, r3
  mov r2, cr3
  mov cr2, cr3
  ret
  movi r1, 0xAAAA5555
  movi r2, 1
  call label
