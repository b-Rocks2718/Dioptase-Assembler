    .kernel

    add r1, r1, 11
    add r1, r1, 0b11
    add r1, r1, 0o11
loop:
    add r1, r1, 0x11 
    add r1, r1, -11

    push r2
    
    mov r2, r3
    mov cr2, r3
    mov r2, cr3
    mov cr2, cr3

.global loop2
loop2:

.global loop3
    
    add r1, r20, -0b11
    add r1, r1, -0o11
    add r1, r1, -1
    add r1, r1, loop

    call loop
    call 13
    call 12312432178568888888888888

    movi r2, 0x12345

    movi r3, loop

    sys EXIT