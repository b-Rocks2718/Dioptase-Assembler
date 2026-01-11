
	.text
# add, div, and mod can probably be more efficient using shifts
# shl and shr should check if second parameter is >16 and return 0 if so
# but im lazy and this works for now
	
# args passed in r1 and r2
# result returned in r1
	.global smul
smul:
	# check sign of inputs, store results in r4
	# if inputs are negative, negate them
	lui r3 0x8000
	mov r4 r0
	and r0 r1 r3
	bz  smul_check_r2
	add r4 r4 1
	sub r1 r0 r1
smul_check_r2:
	and r0 r2 r3
	bz  smul_pos
	add r4 r4 1
	sub r2 r0 r2
smul_pos:
	mov r3 r0
smul_loop: # repeated addition
	cmp r1 r0
	bz  smul_end
	add r1 r1 -1
	add r3 r3 r2
	jmp smul_loop
smul_end:
	add r4 r4 -1
	bnz smul_skip_negate
	sub r3 r0 r3 # fix sign of result
smul_skip_negate:
	mov r1 r3
	ret 

	.global sdiv
sdiv:
	# check sign of inputs, store results in r6
	lui r3 0x8000
	mov r4 r0
	and r0 r1 r3
	bz  sdiv_check_r2
	add r4 r4 1
	sub r1 r0 r1
sdiv_check_r2:
	and r0 r2 r3
	bz  sdiv_pos
	add r4 r4 1
	sub r2 r0 r2
sdiv_pos:
	mov r3 r0
sdiv_loop: # repeated subtraction
	cmp r1 r2
	bs  sdiv_end
	add r3 r3 1
	sub r1 r1 r2
	jmp sdiv_loop
sdiv_end:
	add r6 r6 -1
	bnz sdiv_skip_negate
	sub r3 r0 r3
sdiv_skip_negate:
	mov r1 r3
	ret

	.global smod
smod:
	# check sign of inputs, store results in r6
	lui r3 0x8000
	add r4 r0 r0
	and r0 r1 r3
	bz  smod_check_r2
	add r4 r4 1
	sub r1 r0 r1
smod_check_r2:
	and r0 r2 r3
	bz  smod_pos
	add r4 r4 1
	sub r2 r0 r2
smod_pos:
	mov r3 r0
smod_loop: # repeated subtraction
	cmp r1 r2
	bs  smod_end
	sub r1 r1 r2
	jmp smod_loop
smod_end:
	add r4 r4 -1
	bnz smod_skip_negate
	sub r1 r2 r1 # ensure result is between 0 and r2
smod_skip_negate:
	ret

	.global umul
umul:
	mov r3 r0
umul_loop: # repeated addition
	cmp r1 r0
	bz umul_end
	add r1 r1 -1
	add r3 r3 r2
	jmp umul_loop
umul_end:
	mov r1 r3
	ret

	.global udiv
udiv:
	mov r3 r0
udiv_loop: # repeated subtraction
	cmp r1 r2
	bb  udiv_end
	add r3 r3 1
	sub r1 r1 r2
	jmp udiv_loop
udiv_end:
	mov r1 r3
	ret

	.global umod
umod:
	mov r3 r0
umod_loop: # repeated subtraction
	cmp r1 r2
	bb  umod_end
	sub r1 r1 r2
	jmp umod_loop
umod_end:
	ret
	
	.global sleft_shift
sleft_shift:
	# check sign of r2
	# if negative, do right shift instead
	lui r3 0x8000
	and r0 r3 r2
	bz  sls_loop
	sub r2 r0 r2
	jmp srs_loop
sls_loop: # repeated shift
	cmp r2 r0
	bz  sls_end
	add r2 r2 -1
	lsl r1 r1 1
	jmp sls_loop
sls_end:
	ret
	
	.global sright_shift
sright_shift:
	# check sign of r2
	# if negative, do left shift instead
	lui r3 0x8000
	and r0 r3 r2
	bz  srs_loop
	sub r2 r0 r2
	jmp sls_loop
srs_loop: # repeated shift
	cmp r2 r0
	bz  srs_end
	add r2 r2 -1
	asr r1 r1 1
	jmp srs_loop
srs_end:
	ret

	.global uleft_shift
uleft_shift:
	# check sign of r2
	# if negative, do right shift instead
	lui r3 0x8000
	and r0 r3 r2
	bz  uls_loop
	sub r2 r0 r2
	jmp urs_loop
uls_loop: # repeated shift
	cmp r2 r0
	bz  uls_end
	add r2 r2 -1
	lsl r1 r1 1
	jmp uls_loop
uls_end:
	ret
	
	.global uright_shift
uright_shift:
	# check sign of r2
	# if negative, do left shift instead
	lui r3 0x8000
	and r0 r3 r2
	bz  urs_loop
	sub r2 r0 r2
	jmp uls_loop
urs_loop: # repeated shift
	cmp r2 r0
	bz  urs_end
	add r2 r2 -1
	lsr r1 r1 1
	jmp urs_loop
urs_end:
	ret