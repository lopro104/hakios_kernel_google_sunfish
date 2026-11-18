/*
 * mg_island: the Dynamic Island the way the 16.x tweaks (Nugget) do it -- one answer changed in MobileGestalt's cache,
 * ArtworkDeviceSubType (inside CacheExtra's "oPeik/9e8lQWMszEjbPzng", ArtworkTraits) to the iPhone 14 Pro's 2556, the
 * rest of the iPhone X's answers (its screen class above all) left as they are. Changing the device tree's subtype
 * instead (xnuloader island=1) leaves MobileGestalt no screen class for the screen and SpringBoard falls back to the
 * iPhone 8's dock without home gestures (18 November). Takes effect for programs started after it (SpringBoard is
 * restarted by the job that runs it).   usage: mg_island [SUBTYPE]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
/* (libproc.h is not in the SDK here) */
extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);

#define CACHE "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/" \
	"com.apple.MobileGestalt.plist"

int
main(int argc, char **argv)
{
	int subtype = argc > 1 ? (strchr(argv[1], '=') != NULL ? 0 : atoi(argv[1])) : 2556;
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)CACHE, sizeof(CACHE) - 1, false);
	CFReadStreamRef in = CFReadStreamCreateWithFile(NULL, url);
	CFPropertyListRef cache = NULL;

	for (int tries = 0; tries < 60 && cache == NULL; tries++) {
		if (access(CACHE, R_OK) == 0 && CFReadStreamOpen(in)) {
			cache = CFPropertyListCreateWithStream(NULL, in, 0, kCFPropertyListMutableContainersAndLeaves, NULL, NULL);
			CFReadStreamClose(in);
			CFRelease(in);
			in = CFReadStreamCreateWithFile(NULL, url);
		}
		if (cache == NULL) {
			sleep(1);
		}
	}
	if (cache == NULL || CFGetTypeID(cache) != CFDictionaryGetTypeID()) {
		printf("mg_island: no MobileGestalt cache to change\n");
		return 1;
	}
	CFMutableDictionaryRef extra = (CFMutableDictionaryRef)CFDictionaryGetValue(cache, CFSTR("CacheExtra"));
	/* KEY=N arguments: CacheExtra's KEY (an obfuscated MobileGestalt key) set to N (0 and 1 as booleans) */
	int keys = 0;

	for (int i = 1; i < argc && extra != NULL; i++) {
		char *equals = strchr(argv[i], '=');

		if (equals == NULL) {
			continue;
		}
		*equals = 0;
		CFStringRef key = CFStringCreateWithCString(NULL, argv[i], kCFStringEncodingUTF8);
		int value = atoi(equals + 1);
		CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &value);

		CFDictionarySetValue(extra, key, value == 0 || value == 1 ? (CFTypeRef)(value ? kCFBooleanTrue : kCFBooleanFalse) :
		    (CFTypeRef)number);
		printf("mg_island: %s = %d in MobileGestalt's cache\n", argv[i], value);
		CFRelease(number);
		CFRelease(key);
		keys++;
	}
	CFMutableDictionaryRef traits = extra != NULL ? (CFMutableDictionaryRef)CFDictionaryGetValue(extra,
	    CFSTR("oPeik/9e8lQWMszEjbPzng")) : NULL;

	if (traits == NULL || CFGetTypeID(traits) != CFDictionaryGetTypeID()) {
		printf("mg_island: the cache has no ArtworkTraits (%s)\n", extra != NULL ? "CacheExtra there" : "no CacheExtra");
		return 1;
	}
	CFNumberRef was = CFDictionaryGetValue(traits, CFSTR("ArtworkDeviceSubType"));
	int old = 0;

	if (was != NULL) {
		CFNumberGetValue(was, kCFNumberIntType, &old);
	}
	if (subtype != 0) {
		CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &subtype);

		CFDictionarySetValue(traits, CFSTR("ArtworkDeviceSubType"), number);
		CFRelease(number);
	}
	CFErrorRef error = NULL;
	CFDataRef data = CFPropertyListCreateData(NULL, cache, kCFPropertyListBinaryFormat_v1_0, 0, &error);
	FILE *out = data != NULL ? fopen(CACHE ".new", "wb") : NULL;

	if (out == NULL || fwrite(CFDataGetBytePtr(data), 1, CFDataGetLength(data), out) != (size_t)CFDataGetLength(data) ||
	    fclose(out) != 0 || rename(CACHE ".new", CACHE) != 0) {
		printf("mg_island: could not write the cache\n");
		return 1;
	}
	if (subtype != 0) {
		printf("mg_island: ArtworkDeviceSubType %d -> %d in MobileGestalt's cache\n", old, subtype);
	}
	(void)keys;
	/* SpringBoard read the old answer when it started: it is stopped, and launchd starts it again */
	int pids[2048], count = proc_listallpids(pids, sizeof(pids));

	for (int i = 0; i < count; i++) {
		char name[64] = "";

		if (proc_name(pids[i], name, sizeof(name)) > 0 && strcmp(name, "SpringBoard") == 0) {
			printf("mg_island: SpringBoard (%d) restarted\n", pids[i]);
			kill(pids[i], SIGKILL);
		}
	}
	return 0;
}
