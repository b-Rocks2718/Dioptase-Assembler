  .kernel
  # test all register aliases

_start:
  # user mode aliases
  and r0, r0, sp
  and r0, r0, bp
  and r0, r0, ra
  
  # control register aliases
  mov r0, psr
  mov r0, pid
  mov r0, isr
  mov r0, imr
  mov r0, epc
  mov r0, flg
  mov r0, cdv
  mov r0, tlb
  mov r0, ksp
