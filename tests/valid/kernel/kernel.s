    # test all privileged instructions
    .global _start
_start:
    tlbr r1, r2
    tlbw r1, r2
    tlbc

    # crmv is tested by macros so we skip that

    mode run
    mode sleep
    mode halt

    rfe
    sys EXIT

EXIT:
    mode halt

INT_TIMER:
    mode sleep
    crmv r3, r31
    rfi
    tlbi r3
    ipi  r3, 2
    ipi  r3, all
