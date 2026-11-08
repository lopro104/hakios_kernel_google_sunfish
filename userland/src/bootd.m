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
 * Applications of ours in /redwood/apps (build.sh EXTRA_APPS): the Darwin volume is read only and a bind mount is passed over by the
 * registration of /Applications, so each one is registered with LaunchServices from where it lies, as uicache -p does (retried until
 * LaunchServices answers: the daemon starts long before SpringBoard).
 */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <stdio.h>
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

static void
restart_now(const char *why)
{
	printf("bootd: %s: restarting\n", why);
	fflush(stdout);
	sync();
	reboot(0);
	printf("bootd: reboot() returned\n");
}

static void
listen_for(const char *name, void (^handler)(void))
{
	int token;

	if (notify_register_dispatch(name, &token, dispatch_get_main_queue(), ^(int t) { (void)t; handler(); }) != NOTIFY_STATUS_OK) {
		printf("bootd: cannot listen for %s\n", name);
	}
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
			printf("bootd: cannot register %s: %s%s\n", path.UTF8String, identifier == nil ? "no readable Info.plist with a CFBundleIdentifier " : "",
			    workspace == Nil ? "no LSApplicationWorkspace class" : "");
			fflush(stdout);
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

	@try {
		if (registration_attempts % 2 == 0) {
			done = [space registerApplicationDictionary:dictionary];
		} else {
			done = [space registerApplicationDictionary:dictionary withObserverNotification:YES];
		}
		if (registration_attempts < 4 || registration_attempts % 12 == 0) {
			printf("bootd: attempt %d (%s): registration said %s, installed: %s\n", registration_attempts + 1,
			    registration_attempts % 2 == 0 ? "plain" : "with notification", done ? "yes" : "no",
			    [space applicationIsInstalled:identifier] ? "yes" : "no");
			fflush(stdout);
		}
	} @catch (NSException *exception) {
		printf("bootd: registration threw %s: %s\n", exception.name.UTF8String, exception.reason.UTF8String);
		fflush(stdout);
	}
	registration_attempts++;
	return done;
}

static void
register_our_applications(void)
{
	NSError *error = nil;
	NSArray *names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"/redwood/apps" error:&error];

	printf("bootd: /redwood/apps has %lu entries%s%s\n", (unsigned long)names.count, error ? ": " : "", error ? error.localizedDescription.UTF8String : "");
	fflush(stdout);

	for (NSString *name in names) {
		if (![name hasSuffix:@".app"]) {
			continue;
		}
		NSString *path = [@"/redwood/apps" stringByAppendingPathComponent:name];
		BOOL done = NO;

		for (int attempt = 1; attempt <= 90 && !done; attempt++) {
			@autoreleasepool {
				done = register_application(path);
			}
			if (!done) {
				sleep(5);
			} else {
				printf("bootd: %s registered (attempt %d)\n", name.UTF8String, attempt);
			}
		}
		if (!done) {
			printf("bootd: %s could not be registered\n", name.UTF8String);
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
				printf("bootd: the kernel has no kern.hakios.next_boot: not restarting (it would boot iOS again)\n");
				fflush(stdout);
				return;
			}
			restart_now("next boot is Android");
		});
		printf("bootd: listening\n");
		fflush(stdout);
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ register_our_applications(); });
	}
	dispatch_main();
}
