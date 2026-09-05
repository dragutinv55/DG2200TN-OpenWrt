/*
 * Bank-2 CFE autorun hook. This executes at the same point where an
 * interrupted CFE startup initializes its command and network subsystems,
 * before the run command shuts CFE services down for Linux.
 */
	.syntax unified
	.arm
	.section .text.start
	.global receiver

receiver:
	push	{r0-r12, lr}

	ldr	r12, =0x00f22018
	blx	r12
	ldr	r12, =0x00f18c24
	blx	r12
	ldr	r12, =0x00f2725c
	blx	r12

	ldr	r0, =0x80080038
	ldr	r1, [r0]
	orr	r1, r1, #1
	str	r1, [r0]

	ldr	r0, =0x00f6af44
	mov	r1, #1
	str	r1, [r0]

	pop	{r0-r12, lr}
	ldr	pc, =0x00f1f60c

	.ltorg
