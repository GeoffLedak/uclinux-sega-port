	.file	"initfini.c"
	
	.section .init
	.align	2
	.global	_init
	.type	_init, %function
	ldr	pc, [sp], #4
	.size	_init, .-_init
	
	.section .fini
	.align	2
	.global	_fini
	.type	_fini, %function
	ldr	pc, [sp], #4
	.size	_fini, .-_fini
	
	.ident	"GCC: (GNU) 3.3.2 20031005 (Debian prerelease)"
