/*
 * libhakios.ci.dylib: no QR-code scan of an image inside an application (see inject.c for how it gets there; build.sh's INJECT names
 * the programs).
 *
 * Holding an image down in Safari makes it look for a QR code in it (to offer the link in the menu): BarcodeSupport's
 * -[BCSQRCodeParser _qrCodeFeatureFromImage:] makes a Core Image barcode detector, which needs a CIContext. Without Metal in the
 * application Core Image makes that on its OpenGL ES path (CI::GLContext), which asks for OpenGLES.framework/GLEngine.bundle -- not on
 * this system -- and calls through a null pointer: Safari dies at once (systrace, 2026-11-07). The parser's answer for "no code in this
 * image" is nil, which is what it is made to say here. With Metal in the application this library is not needed.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <objc/message.h>
#include <objc/runtime.h>

static id
no_qr_code(id self, SEL command, id image)
{
	(void)self;
	(void)command;
	(void)image;
	return NULL;
}

static id
no_qr_action(id self, SEL command, id image, id user_info)
{
	(void)self;
	(void)command;
	(void)image;
	(void)user_info;
	return NULL;
}

/* the methods that lead to the detector, the one in Safari's own framework (loaded when it starts) first */
static struct hook {
	const char *class_name;
	const char *selector;
	IMP replacement;
	int done;
} hooks[] = {
	{ "_SFQRCodeDetectionController", "getActionForImageSynchronously:userInfo:", (IMP)no_qr_action, 0 },
	{ "BCSQRCodeParser", "_qrCodeFeatureFromImage:", (IMP)no_qr_code, 0 },
};

/* true when every hook is in */
static int
try_hooks(void)
{
	int all = 1;

	for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		Class cls;
		Method method;

		if (hooks[i].done) {
			continue;
		}
		if ((cls = objc_getClass(hooks[i].class_name)) != NULL &&
		    (method = class_getInstanceMethod(cls, sel_registerName(hooks[i].selector))) != NULL) {
			method_setImplementation(method, hooks[i].replacement);
			hooks[i].done = 1;
		} else {
			all = 0;
		}
	}
	return all;
}

/*
 * The class is in BarcodeSupport.framework, which the application may load at any time, and the Objective-C runtime registers a
 * library's classes only after the add-image callbacks have run -- so looking once, when an image is added, can miss it. A thread
 * looks every 20 ms (for ten minutes) until the classes are there. The barcode framework may be loaded only when an image is
 * held and used at once, so the first hook is in Safari\'s own controller, which is there from the start.
 */
static void *
wait_for_parser(void *unused)
{
	(void)unused;
	for (int i = 0; i < 30000 && !try_hooks(); i++) {
		usleep(20000);
	}
	return NULL;
}

__attribute__((constructor)) static void
inject_ci(void)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, wait_for_parser, NULL) == 0) {
		pthread_detach(thread);
	}
}
