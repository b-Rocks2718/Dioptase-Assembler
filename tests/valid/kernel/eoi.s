  # Test summary:
  # - verifies kernel-mode assembly of `eoi n` and `eoi all`
  # - covers low, mid, and high ISR bit indices
  .global _start
_start:
  eoi 0
  eoi 6
  eoi 15
  eoi all
