/*
 * /sbin/launchd of a small RAM disk: the phone's HFS Plus root in `userdata`, read only (the old tree's iOS root, on
 * the Pixel 4a, which has no `darwin` partition). Needs ufs=2, whose block devices refuse every write.
 *
 * It mounts /dev/ufs_userdata read only on /flash, says what is there, and stops (exit panics the kernel, ending the
 * run). hfsroot_wait=N: stay N seconds first. hfsroot_top=N: see top_watch.
 *
 * hfsroot_launchd=1 goes on: a memory filesystem on /private/var, since the volume's own cannot be written, with a
 * copy of the volume's directories, links and small files (var_copy), the
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

/*
 * /private/var of the volume, into the memory filesystem that covers it: directories, links, and the small files that
 * hold what the system remembers (preferences and the like). Nothing goes back to the volume: this is gone at reboot.
 */
#define VAR_FILE_MAX    (64 * 1024)     /* a bigger file is left out */
#define VAR_BYTES_MAX   (150 << 20)
#define VAR_FILES_MAX   40000
#define VAR_DEPTH_MAX   24

/* left empty: relative to /private/var */
static const char *const var_empty[] = {
	"tmp", "run", "vm", "logs", "jb", "db/diagnostics", "db/uuidtext", "mobile/Library/Logs", "mobile/Library/Caches",
	"mobile/Media", "MobileAsset", "MobileSoftwareUpdate", "staged_system_apps", "root/Library/Caches",
};
/* not made at all: a core dump in memory would cost more than it says */
static const char *const var_none[] = { "cores" };

static long var_dirs, var_links, var_files, var_bytes, var_skipped, var_failed;

static int
var_listed(const char *rel, const char *const *names, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		if (strcmp(rel, names[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

static void
var_copy_file(const char *from, const char *to, const struct stat *st)
{
	static char data[VAR_FILE_MAX];
	ssize_t n;
	int in, out;

	if (st->st_size > VAR_FILE_MAX || var_files >= VAR_FILES_MAX || var_bytes + st->st_size > VAR_BYTES_MAX) {
		var_skipped++;
		return;
	}
	in = open(from, O_RDONLY, 0);
	if (in < 0) {
		var_failed++;
		return;
	}
	out = open(to, O_WRONLY | O_CREAT, st->st_mode & 07777);
	if (out < 0) {
		var_failed++;
		close(in);
		return;
	}
	while ((n = read(in, data, sizeof(data))) > 0) {
		if (write(out, data, (size_t)n) != n) {
			var_failed++;
			break;
		}
		var_bytes += n;
	}
	close(out);
	close(in);
	chown(to, (int)st->st_uid, (int)st->st_gid);
	chmod(to, st->st_mode & 07777);
	var_files++;
}

/* from, to: absolute; rel: the same place from /private/var ("" at the top) */
static void
var_copy(const char *from, const char *to, const char *rel, int depth)
{
	static char buffers[VAR_DEPTH_MAX][4096];
	char *buf = buffers[depth];
	off_t pos = 0;
	ssize_t n;
	int fd = open(from, O_RDONLY | O_DIRECTORY, 0);

	if (fd < 0) {
		var_failed++;
		return;
	}
	while ((n = getdirentries(fd, buf, 4096, &pos)) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct dirent *de = (struct dirent *)(void *)p;
			static char link[1024];
			char fromx[900], tox[900], relx[900];
			struct stat st;
			size_t lf, lt, lr, ln = strlen(de->d_name);

			if (de->d_reclen == 0) {
				break;
			}
			p += de->d_reclen;
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}
			lf = strlen(from);
			lt = strlen(to);
			lr = strlen(rel);
			if (lf + ln + 2 > sizeof(fromx) || lt + ln + 2 > sizeof(tox) || lr + ln + 2 > sizeof(relx)) {
				var_skipped++;
				continue;
			}
			strcpy(fromx, from);
			fromx[lf] = '/';
			strcpy(fromx + lf + 1, de->d_name);
			strcpy(tox, to);
			tox[lt] = '/';
			strcpy(tox + lt + 1, de->d_name);
			strcpy(relx, rel);
			if (lr) {
				relx[lr++] = '/';
			}
			strcpy(relx + lr, de->d_name);
			if (var_listed(relx, var_none, sizeof(var_none) / sizeof(var_none[0])) || lstat(fromx, &st) != 0) {
				continue;
			}
			switch (st.st_mode & 0170000) {
			case 0040000:
				if (mkdir(tox, st.st_mode & 07777) != 0) {
					var_failed++;
					break;
				}
				chown(tox, (int)st.st_uid, (int)st.st_gid);
				chmod(tox, st.st_mode & 07777);
				var_dirs++;
				if (depth + 1 < VAR_DEPTH_MAX && !var_listed(relx, var_empty, sizeof(var_empty) / sizeof(var_empty[0]))) {
					var_copy(fromx, tox, relx, depth + 1);
				}
				break;
			case 0120000: {
				ssize_t len = readlink(fromx, link, sizeof(link) - 1);

				if (len >= 0) {
					link[len] = 0;
					symlink(link, tox);
					var_links++;
				}
				break;
			}
			case 0100000:
				var_copy_file(fromx, tox, &st);
				break;
			default:
				var_skipped++;
			}
		}
	}
	close(fd);
}

/*
 * hfsroot_top=N: a process beside launchd that says every N seconds who has had the processors, and how SpringBoard
 * and backboardd are doing (no system log can be read in this setup). cputop.c, with the raw system call.
 */
#define SYS_proc_info           336
#define TOP_MAX                 1024
struct top_task {
	uint64_t vsize, rsize, user, system, tuser, tsystem;
	int32_t policy, faults, pageins, cow, sent, received, mach, unix_calls, csw, threads, running, priority;
};
static struct top_sample { int pid; uint64_t time; } top_last[TOP_MAX], top_now[TOP_MAX];
static int top_pids[TOP_MAX];

static int
top_task_of(int pid, struct top_task *t)
{
	return mini_syscall(SYS_proc_info, 2, pid, 4 /* PROC_PIDTASKINFO */, 0, (long)t, sizeof(*t)) == sizeof(*t);
}

static void
top_name(int pid, char *name)
{
	static char info[136];		/* struct proc_bsdinfo: the short name at 48, the long one at 64 */

	strcpy(name, "?");
	if (mini_syscall(SYS_proc_info, 2, pid, 3 /* PROC_PIDTBSDINFO */, 0, (long)info, sizeof(info)) == sizeof(info)) {
		info[64 + 31] = 0;
		strcpy(name, info[64] ? info + 64 : info + 48);
	}
}

static uint64_t
top_clock(void)
{
	uint64_t v;

	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static void
top_watch(long seconds)
{
	int last_count = 0;
	uint64_t before = top_clock(), start = before;

	for (;;) {
		long bytes, n = 0;
		uint64_t at, span;

		for (long i = 0; i < seconds; i++) {
			usleep(1000000);
		}
		bytes = mini_syscall(SYS_proc_info, 1, 1 /* PROC_ALL_PIDS */, 0, 0, (long)top_pids, sizeof(top_pids));
		at = top_clock();
		span = at - before;
		printf("hfstop: %ld s:", (long)((at - start) / 19200000));
		for (long i = 0; i < bytes / (long)sizeof(int) && n < TOP_MAX; i++) {
			struct top_task t;
			uint64_t then = 0;
			long percent;
			char name[40];

			if (top_pids[i] <= 0 || !top_task_of(top_pids[i], &t)) {
				continue;
			}
			top_now[n].pid = top_pids[i];
			top_now[n].time = t.user + t.system;
			for (int j = 0; j < last_count; j++) {
				if (top_last[j].pid == top_pids[i]) {
					then = top_last[j].time;
					break;
				}
			}
			percent = span ? (long)((top_now[n].time - then) * 100 / span) : 0;
			n++;
			top_name(top_pids[i], name);
			if (percent >= 3) {
				printf(" %s[%d] %ld%%", name, top_pids[i], percent);
			}
		}
		printf("\n");
		for (long i = 0; i < n; i++) {
			struct top_task t;
			char name[40];

			top_name(top_now[i].pid, name);
			if ((!strcmp(name, "SpringBoard") || !strcmp(name, "backboardd")) && top_task_of(top_now[i].pid, &t)) {
				printf("hfstop: %s[%d]: %ld ms of processor, %d threads, %d running, %d mach msgs out %d in, "
				    "%d mach calls, %d unix calls, %ld MB\n", name, top_now[i].pid,
				    (long)((t.user + t.system) / 19200), t.threads, t.running, t.sent, t.received, t.mach,
				    t.unix_calls, (long)(t.rsize >> 20));
			}
		}
		memcpy(top_last, top_now, sizeof(top_now[0]) * (size_t)n);
		last_count = (int)n;
		before = at;
	}
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
	/* the kernel's 1024 vnodes are soon gone with the whole job list running: the rule of init.c, configure_vnodes */
	{
		unsigned long long memsize = 0;
		size_t len = sizeof(memsize);
		int vnodes = 32768;

		if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0 && memsize / 65536 + 1024 < 32768) {
			vnodes = (int)(memsize / 65536) + 1024;
		}
		if (sysctlbyname("kern.maxvnodes", NULL, NULL, &vnodes, sizeof(vnodes)) != 0) {
			printf("hfsroot: kern.maxvnodes: %s\n", strerror(errno));
		} else {
			printf("hfsroot: up to %d vnodes\n", vnodes);
		}
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

			/* a second way to the volume's /private/var, for the copy, before the memory filesystem covers it */
			int bound = mount("bindfs", "/oldvar", MNT_RDONLY, (void *)"/flash/private/var") == 0;

			if (!bound) {
				printf("hfsroot: bindfs of /private/var: %s\n", strerror(errno));
			}
			if (mount("ramfs", "/flash/private/var", 0, NULL) != 0) {
				printf("hfsroot: memory filesystem on /private/var: %s\n", strerror(errno));
			} else {
				if (bound) {
					var_copy("/oldvar", "/flash/private/var", "", 0);
					printf("hfsroot: /private/var in memory: %ld directories, %ld links, %ld files, %ld bytes; "
					    "%ld left out, %ld failed\n", var_dirs, var_links, var_files, var_bytes, var_skipped, var_failed);
				}
				strcpy(target, "/flash ");
				strcpy(target + 7, out ? out : "cores");
				if (sysctlbyname("kern.hakios_switch_root", NULL, NULL, target, strlen(target)) != 0) {
					printf("hfsroot: switching the root: %s\n", strerror(errno));
				} else {
					chdir("/");
					printf("hfsroot: the volume is the root; launchd next\n");
					if (boot_arg("hfsroot_top") != NULL && fork() == 0) {
						top_watch(parse(boot_arg("hfsroot_top")));
					}
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
