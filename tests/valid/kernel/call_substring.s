  .global _start
_start:
  call test_syscall
  nop

  .global test_syscall
test_syscall:
  ret
