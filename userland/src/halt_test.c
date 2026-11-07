/*
 * halt_test [SECONDS]: wait, then ask the kernel to halt the machine (reboot(RB_HALT)), as launchd does at the end of a shutdown.
 * With "restart" as the first argument, reboot(0): a restart. For testing what the loader does with the kernel's halt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* (the SDK here has no <sys/reboot.h>: the BSD call and flag, as Darwin defines them) */
extern int reboot(int howto);
#define RB_HALT 0x08

int
main(int argc, char **argv)
{
	int restart = argc > 1 && strcmp(argv[1], "restart") == 0;
	int seconds = argc > 1 + restart ? atoi(argv[1 + restart]) : 0;

	printf("halt_test: %s in %d s\n", restart ? "restart" : "halt", seconds);
	fflush(stdout);
	sleep((unsigned)seconds);
	sync();
	reboot(restart ? 0 : RB_HALT);
	printf("halt_test: reboot() returned\n");
	return 1;
}
