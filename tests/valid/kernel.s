    # test all privileged instructions
    .kernel

_start:
    tlbr r1, r2
    tlbw r1, r2
    tlbc

    # crmv is tested by macros so we skip that

    mode run
    mode sleep
    mode halt

    rfe r29, r30
    sys EXIT

EXIT:
    mode halt

INT_0:
    mode sleep

    rfi r0, r0