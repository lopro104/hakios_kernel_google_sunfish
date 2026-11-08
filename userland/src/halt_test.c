/*
 * halt_test [SECONDS]: wait, then ask the kernel to halt the machine (reboot(RB_HALT)), as launchd does at the end of a shutdown.
 * With "restart" as the first argument ("android": after setting kern.hakios_next_boot), reboot(0): a restart. For testing what the loader does with the kernel's halt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
/* (the SDK here has no <sys/reboot.h>: the BSD call and flag, as Darwin defines them) */
extern int reboot(int howto);
#define RB_HALT 0x08

int
main(int argc, char **argv)
{
	int android = argc > 1 && strcmp(argv[1], "android") == 0;
	int restart = android || (argc > 1 && strcmp(argv[1], "restart") == 0);
	int seconds = argc > 1 + restart ? atoi(argv[1 + restart]) : 0;

	printf("halt_test: %s in %d s\n", restart ? "restart" : "halt", seconds);
	fflush(stdout);
	sleep((unsigned)seconds);
	sync();
	if (android) {
		int one = 1;

		printf("halt_test: kern.hakios_next_boot=1: %d\n", sysctlbyname("kern.hakios_next_boot", NULL, NULL, &one, sizeof(one)));
		fflush(stdout);
	}
	reboot(restart ? 0 : RB_HALT);
	printf("halt_test: reboot() returned\n");
	return 1;
}
