    
    add r1, r1, 11
    add r1, r1, 0b11
    add r1, r1, 0o11
loop:
    add r1, r1, 0x11
    add r1, r1, -11


    add r1, r1, -0b11
    add r1, r1, -0o11
    add r1, r1, -1
    add r1, r1, loop
    sys EXIT
