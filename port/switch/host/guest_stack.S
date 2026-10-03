/*
GUEST_STACK.S

Runs the guest on a stack below 4 GB.

The console will not give a thread a stack the C library did not allocate:
pthread_create with a stack mapped out of the sub-4 GB space is refused with
ENOMEM even though that stack is mapped and readable, and with a stack of
its own choosing it succeeds. The guest cannot use a stack above 4 GB, its
pointers are 32 bits and the game's data formats depend on it.

So the thread is created the ordinary way and this moves it onto the stack
the port mapped, immediately before the guest is entered, and never moves
back.

One way, deliberately. The old stack pointer is not saved, because there is
nothing to return to: the guest's entry point ends the process (host_exit),
and the one thread that does this is the game thread, which exists only to
run the game. If the guest ever did return, this stops rather than
returning onto a stack frame that belongs to a different address space -
the log is written first, so that case would be visible rather than a
fault.
*/

.text
.global guest_stack_enter

/* void guest_stack_enter(void *stack, void (*function)(void *), void *argument)
 *
 * x0 = the stack to move onto, x1 = the function, x2 = its argument.
 *
 * The argument has to be moved into x0 before the call: x0 is the register a
 * C function takes its first argument in, and by then x0 holds the stack
 * that was just installed. Leaving it there meant the guest was handed the
 * stack pointer instead of the boot structure, and read a zero for the
 * contiguous window out of stack memory - which is why the window could not
 * be reserved and the game reported no contiguous memory.
 */
guest_stack_enter:
	mov	sp, x0			/* onto the stack the port mapped, below 4 GB */
	mov	x0, x2			/* the argument, which x0 no longer is */
	blr	x1			/* into the guest */
	bl	guest_stack_returned	/* it should not come back; if it does, say so */
	b	.			/* and stop */

.global guest_stack_returned
guest_stack_returned:
	mov	x19, x0
	adrp	x0, message
	add	x0, x0, :lo12:message
	mov	x1, x19
	bl	host_logf
	mov	x0, #1
	b	host_exit

.section .rodata
message:
	.asciz "the guest's entry point returned (it should end the process); stopping"
