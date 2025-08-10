    # test absolute and immediate branches
_start:
    # immediate branches

    # test immediate encoding
    br 0
    br 1
    br 0xFFFF
    br -1
    br _start

    # test branch codes
    br 0
    bz 0
    bnz 0
    bs 0
    bns 0
    bc 0
    bnc 0
    bo 0
    bno 0
    bp 0
    bnp 0
    bg 0
    bge 0
    bl 0
    ble 0
    ba 0
    bae 0
    bb 0
    bbe 0

    # absolute register branches

    # test register encoding
    bra r12, r27
    bza r0,  r2

    # test branch codes
    bra r1
    bza r0
    bnza r0
    bsa r0
    bnsa r0
    bca r0
    bnca r0
    boa r0
    bnoa r0
    bpa r0
    bnpa r0
    bga r0
    bgea r0
    bla r0
    blea r0
    baa r0
    baea r0
    bba r0
    bbea r0

    # pc-relative register branches
    br r1, r2
    bz r3, r4

    br r1
    bz r0
    bnz r0
    bs r0
    bns r0
    bc r0
    bnc r0
    bo r0
    bno r0
    bp r0
    bnp r0
    bg r0
    bge r0
    bl r0
    ble r0
    ba r0
    bae r0
    bb r0
    bbe r0