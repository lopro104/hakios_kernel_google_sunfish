/*
 * bootd: the root daemon behind the Boot Switch app (devices/sunfish/apps/BootSwitch), and what puts our applications on the home screen.
 *
 * The app only posts Darwin notifications; this listens for them and asks the kernel:
 *
 *   com.hakios.reboot, com.hakios.restart   restart the machine (reboot(2) with 0)
 *   com.hakios.next-boot-android            the next start is Android's: sets kern.hakios.next_boot=1 (the kernel tells the loader, which
 *                                           leaves the marker U-Boot acts on) and restarts. Where the kernel has no such switch it says so
 *                                           and does nothing: a restart would only boot iOS again.
 * Any process can post these names; the worst that can happen is a restart.
 *
 * Applications of ours in /private/preboot/redwood/apps (build.sh EXTRA_APPS): the Darwin volume is read only, so each one is registered
 * with LaunchServices, as uicache -p does, from where its files REALLY are -- the boot volume at /private/preboot/redwood, not
 * /redwood, which is a bind mount of it: LaunchServices keeps an application by volume and file number and takes none from a file
 * system that does not claim persistent ids (docs/ru/notes.md, the package manager's prefix). Retried until LaunchServices answers: the
 * daemon starts long before SpringBoard.
 */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern int reboot(int howto);

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dictionary;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dictionary withObserverNotification:(BOOL)notify;
- (BOOL)applicationIsInstalled:(NSString *)identifier;
@end

/* said on the console and kept in /private/var/bootd.log: the kernel's log drops the middle of a run, where these lines come */
static void
blog(const char *format, ...) __printflike(1, 2);

static void
blog(const char *format, ...)
{
	char line[400];
	va_list list;
	FILE *file;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	fputs(line, stdout);
	fflush(stdout);
	if ((file = fopen("/private/var/bootd.log", "a")) != NULL) {
		fputs(line, file);
		fclose(file);
	}
}

static void
restart_now(const char *why)
{
	blog("bootd: %s: restarting\n", why);
	sync();
	reboot(0);
	blog("bootd: reboot() returned\n");
}

static void
listen_for(const char *name, void (^handler)(void))
{
	int token;

	if (notify_register_dispatch(name, &token, dispatch_get_main_queue(), ^(int t) { (void)t; handler(); }) != NOTIFY_STATUS_OK) {
		blog("bootd: cannot listen for %s\n", name);
	}
}

/* the pid of the process whose name (the kernel's p_comm, 16 characters at most) is NAME, 0 if none */
static pid_t
find_process(const char *name)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
	size_t size = 0;
	struct kinfo_proc *list;
	pid_t found = 0;

	if (sysctl(mib, 3, NULL, &size, NULL, 0) != 0 || size == 0) {
		return 0;
	}
	size += 32 * sizeof(struct kinfo_proc);
	if ((list = malloc(size)) == NULL) {
		return 0;
	}
	if (sysctl(mib, 3, list, &size, NULL, 0) == 0) {
		for (size_t i = 0; i < size / sizeof(struct kinfo_proc); i++) {
			if (strncmp(list[i].kp_proc.p_comm, name, 16) == 0) {
				found = list[i].kp_proc.p_pid;
			}
		}
	}
	free(list);
	return found;
}

static int registration_attempts;

/* the application at `path`, registered the way uicache -p does it */
static BOOL
register_application(NSString *path)
{
	NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
	NSString *identifier = info[@"CFBundleIdentifier"];
	NSMutableDictionary *dictionary;
	Class workspace = NSClassFromString(@"LSApplicationWorkspace");

	if (identifier == nil || workspace == Nil) {
		if (registration_attempts < 3) {
			blog("bootd: cannot register %s: %s%s\n", path.UTF8String, identifier == nil ? "no readable Info.plist with a CFBundleIdentifier " : "",
			    workspace == Nil ? "no LSApplicationWorkspace class" : "");
		}
		registration_attempts++;
		return NO;
	}
	dictionary = [NSMutableDictionary dictionaryWithDictionary:info];
	dictionary[@"ApplicationType"] = @"System";
	dictionary[@"CFBundleIdentifier"] = identifier;
	dictionary[@"CodeInfoIdentifier"] = identifier;
	dictionary[@"CompatibilityState"] = @0;
	dictionary[@"IsContainerized"] = @NO;
	dictionary[@"Path"] = path;
	dictionary[@"SignerOrganization"] = @"Apple Inc.";
	dictionary[@"SignatureVersion"] = @132352;
	dictionary[@"SignerIdentity"] = @"Apple iPhone OS Application Signing";
	LSApplicationWorkspace *space = [workspace defaultWorkspace];
	BOOL done = NO;

	if (registration_attempts < 4) {
		blog("bootd: attempt %d: calling LaunchServices for %s\n", registration_attempts + 1, path.UTF8String);
	}
	@try {
		if (registration_attempts % 2 == 0) {
			done = [space registerApplicationDictionary:dictionary];
		} else {
			done = [space registerApplicationDictionary:dictionary withObserverNotification:YES];
		}
		if (registration_attempts < 4 || registration_attempts % 12 == 0) {
			blog("bootd: attempt %d (%s): registration said %s, installed: %s\n", registration_attempts + 1,
			    registration_attempts % 2 == 0 ? "plain" : "with notification", done ? "yes" : "no",
			    [space applicationIsInstalled:identifier] ? "yes" : "no");
		}
	} @catch (NSException *exception) {
		blog("bootd: registration threw %s: %s\n", exception.name.UTF8String, exception.reason.UTF8String);
	}
	registration_attempts++;
	return done;
}

static void
register_our_applications(void)
{
	NSError *error = nil;
	NSArray *names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"/private/preboot/redwood/apps" error:&error];

	blog("bootd: /private/preboot/redwood/apps has %lu entries%s%s\n", (unsigned long)names.count, error ? ": " : "", error ? error.localizedDescription.UTF8String : "");

	for (NSString *name in names) {
		if (![name hasSuffix:@".app"]) {
			continue;
		}
		NSString *path = [@"/private/preboot/redwood/apps" stringByAppendingPathComponent:name];
		__block BOOL done = NO;

		/* LaunchServices is busy with the system's applications for the first half minute after SpringBoard starts */
		for (int waited = 0; find_process("SpringBoard") == 0 && waited < 600; waited += 2) {
			sleep(2);
		}
		blog("bootd: SpringBoard is up (pid %d): waiting 30 s before registering\n", (int)find_process("SpringBoard"));
		sleep(30);
		for (int attempt = 1; attempt <= 40 && !done; attempt++) {
			/* one call at a time, each on a thread of its own: a call that does not come back is given up on after 45 s */
			dispatch_semaphore_t finished = dispatch_semaphore_create(0);
			__block BOOL answer = NO;

			dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
				@autoreleasepool {
					answer = register_application(path);
				}
				dispatch_semaphore_signal(finished);
			});
			if (dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 45 * NSEC_PER_SEC)) != 0) {
				blog("bootd: attempt %d: LaunchServices did not answer in 45 s\n", attempt);
				continue;
			}
			done = answer;
			if (!done) {
				sleep(5);
			} else {
				blog("bootd: %s registered (attempt %d)\n", name.UTF8String, attempt);
			}
		}
		if (!done) {
			blog("bootd: %s could not be registered\n", name.UTF8String);
		}
		fflush(stdout);
	}
}

int
main(void)
{
	@autoreleasepool {
		listen_for("com.hakios.reboot", ^{ restart_now("com.hakios.reboot"); });
		listen_for("com.hakios.restart", ^{ restart_now("com.hakios.restart"); });
		listen_for("com.hakios.next-boot-android", ^{
			int one = 1;

			if (sysctlbyname("kern.hakios.next_boot", NULL, NULL, &one, sizeof(one)) != 0) {
				blog("bootd: the kernel has no kern.hakios.next_boot: not restarting (it would boot iOS again)\n");
				return;
			}
			restart_now("next boot is Android");
		});
		blog("bootd: listening\n");
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ register_our_applications(); });
	}
	dispatch_main();
}
