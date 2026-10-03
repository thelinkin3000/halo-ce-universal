/* The smallest thing that can say hello: it exists only to tell whether a
program built here runs at all on this console, before any of the memory
probe's own behaviour is in play. */
#include <switch.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("hello from the probe build\n");
	printf("pointer size %u bits\n", (unsigned)(sizeof(void *) * 8));
	return 0;
}
