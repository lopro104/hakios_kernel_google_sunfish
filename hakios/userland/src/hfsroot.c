/*
 * /sbin/launchd of a small RAM disk: the phone's HFS Plus root in `userdata`, read only (the old tree's iOS root, on
 * the Pixel 4a, which has no `darwin` partition). Needs ufs=2, whose block devices refuse every write.
 *
 * It mounts /dev/ufs_userdata read only on /flash, says what is there, and stops (exit panics the kernel, ending the
 * run). hfsroot_wait=N: stay N seconds first.
 *
 * hfsroot_launchd=1 goes on: a memory filesystem on /private/var, since the volume's own cannot be written, the
 * volume as the root (the RAM disk on hfsroot_out, default cores, which has to be a directory on the volume), and its
 * /sbin/launchd as process 1.
 */
#include "mini.h"

/* the value of NAME=VALUE on the kernel's command line, or NULL */
static const char *
boot_arg(const char *name)
{
	static char args[1024];
	size_t len = sizeof(args) - 1, n = strlen(name);

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return NULL;
	}
	args[len] = 0;
	for (char *c = args; *c; c++) {
		if ((c == args || c[-1] == ' ') && strncmp(c, name, n) == 0 && c[n] == '=') {
			char *v = c + n + 1, *end = strchr(v, ' ');

			if (end) {
				*end = 0;
			}
			return v;
		}
	}
	return NULL;
}

static long
parse(const char *text)
{
	long n = 0;

	while (*text >= '0' && *text <= '9') {
		n = n * 10 + (*text++ - '0');
	}
	return n;
}

static void
show(const char *path)
{
	struct stat st;
	unsigned magic = 0;
	int fd;

	if (stat(path, &st) != 0) {
		printf("hfsroot: %s: %s\n", path, strerror(errno));
		return;
	}
	if ((st.st_mode & 0170000) == 0100000 && (fd = open(path, O_RDONLY, 0)) >= 0) {
		if (read(fd, &magic, sizeof(magic)) != (ssize_t)sizeof(magic)) {
			magic = 0;
		}
		close(fd);
	}
	printf("hfsroot: %s: mode %x, %lld bytes, begins %x\n", path, (unsigned)st.st_mode, (long long)st.st_size, magic);
}

static void
list(const char *path)
{
	static char buf[8192];
	off_t pos = 0;
	ssize_t n;
	int fd = open(path, O_RDONLY | O_DIRECTORY, 0);

	if (fd < 0) {
		printf("hfsroot: %s: %s\n", path, strerror(errno));
		return;
	}
	printf("hfsroot: in %s:", path);
	while ((n = getdirentries(fd, buf, sizeof(buf), &pos)) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct dirent *de = (struct dirent *)(void *)p;

			if (de->d_reclen == 0) {
				break;
			}
			printf(" %s", de->d_name);
			p += de->d_reclen;
		}
	}
	printf("\n");
	close(fd);
}

int
main(int argc, char **argv, char **envp)
{
	/* struct hfs_mount_args (bsd/hfs/hfs_mount.h) */
	struct {
		const char *fspec;
		uint32_t uid, gid;
		uint16_t mask, pad;
		uint32_t encoding;
		int tz[2];
		int flags, journal_tbuffer_size, journal_flags, journal_disable;
	} args;
	static const char *const files[] = {
		"/flash/sbin/launchd", "/flash/usr/lib/dyld", "/flash/private/var", "/flash/private/preboot/Cryptexes/OS",
		"/flash/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld", "/flash/System/Library/LaunchDaemons",
		"/flash/cores", "/flash/Developer", "/flash/Applications",
	};
	const char *wait = boot_arg("hfsroot_wait"), *go = boot_arg("hfsroot_launchd"), *out = boot_arg("hfsroot_out");
	int fd = open("/dev/console", O_RDWR, 0), tries;

	(void)argc;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) {
		close(fd);
	}
	memset(&args, 0, sizeof(args));
	args.fspec = "/dev/ufs_userdata";
	args.journal_disable = 1;
	/* the kernel's storage driver may still be making its nodes */
	for (tries = 0; tries < 30; tries++) {
		struct stat st;

		if (stat(args.fspec, &st) == 0) {
			break;
		}
		usleep(1000000);
	}
	show(args.fspec);
	if (mount("hfs", "/flash", MNT_RDONLY, &args) != 0) {
		printf("hfsroot: mount %s read only: %s\n", args.fspec, strerror(errno));
	} else {
		printf("hfsroot: %s is mounted read only on /flash\n", args.fspec);
		list("/flash");
		for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
			show(files[i]);
		}
		if (go && go[0] == '1') {
			static char target[128];
			char *av[] = { "/sbin/launchd", NULL };

			if (mount("ramfs", "/flash/private/var", 0, NULL) != 0) {
				printf("hfsroot: memory filesystem on /private/var: %s\n", strerror(errno));
			} else {
				strcpy(target, "/flash ");
				strcpy(target + 7, out ? out : "cores");
				if (sysctlbyname("kern.hakios_switch_root", NULL, NULL, target, strlen(target)) != 0) {
					printf("hfsroot: switching the root: %s\n", strerror(errno));
				} else {
					chdir("/");
					printf("hfsroot: the volume is the root; launchd next\n");
					execve(av[0], av, envp);
					printf("hfsroot: execve %s: %s\n", av[0], strerror(errno));
				}
			}
		}
	}
	if (wait) {
		for (long n = parse(wait); n > 0; n--) {
			usleep(1000000);
		}
	}
	printf("hfsroot: done\n");
	return 0;
}
