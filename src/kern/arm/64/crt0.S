/* -*- c -*- */

#include "asm.h"
.section .text.init,"ax"
.type start,#function
ENTRY(_start_kernel)
ENTRY(start)
	ldr	x9, =_stack;
	mov	sp, x9
	bl	__main

     /* never returns */

.section ".init.bss", "w", %nobits
.p2align 4
.globl _sstack
_sstack:
	.space	4096
.globl _stack
.type _stack,#object
_stack:
