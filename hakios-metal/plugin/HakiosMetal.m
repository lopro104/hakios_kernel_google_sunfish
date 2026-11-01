/*
 * HakiosMetal: the plug-in Metal loads when the kernel names it (boot-arg metal=N): a device, and the first objects it makes
 * that hold something (buffers, libraries, functions: "STEP TWO" below).
 *
 * What Metal does (docs/ru/metal.md section 1): it finds the IOKit service of class IOAcceleratorES, reads its
 * properties MetalPluginName ("HakiosMetal") and MetalPluginClassName ("HakiosMetalDevice"), opens
 * /System/Library/Extensions/HakiosMetal.bundle, takes that class, insists that it is a subclass of Metal's private
 * _MTLDevice, and sends it -initWithAcceleratorPort:. That object is the system's one device.
 *
 * What this is, and is not. HakiosMetalDevice is a subclass of _MTLDevice that answers -initWithAcceleratorPort: and -name,
 * makes buffers that hold memory and libraries read from a .metallib, and stand-ins for the rest (a queue that queues nothing,
 * pipeline and state objects that hold nothing). It cannot run anything on the GPU yet, and says so: a message it does not know
 * is logged ("HakiosMetal: asked and not implemented: -...") and answers nil/0/NO instead of crashing, so that the question
 * "what does Darwin ask a device first, and in what order?" can be read off the log rather than guessed. The compiler for the shaders is a separate piece and is largely done
 * (docs/air.md: Apple's shader bitcode into Mesa's NIR); the command queue and the kernel side are not started (docs/metal-plan.md).
 *
 * _MTLDevice is not in the SDK's Metal.tbd (it lives in the system's own copy of the framework), so the class is made at run
 * time, in the constructor that runs when the bundle is loaded, by objc_allocateClassPair on the _MTLDevice that is there.
 * If there is none, nothing is registered, and Metal drops this plug-in as it does any that is not a _MTLDevice.
 *
 * Status. Step one ran in QEMU (docs/metal-plan.md: what it found). Step two -- buffers, libraries, functions -- was built for
 * Darwin userland 16.5 and run on a Mac only, against the Mac's own Metal (tools/metal-plugin-test.sh, under the address sanitizer: every
 * buffer written and read back and freed, the system's five MTLB libraries read, all of their functions by name with their
 * bitcode); it has not run on Darwin yet: metal_test's "what the device makes" is the check for that (QEMU, metal=1).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static Class gBase;                     /* Metal's private _MTLDevice */
static unsigned int gPort;              /* the IOAcceleratorES service this device was made for */
static id gDevice;                      /* the device, once made: there is one */

static void
say(NSString * text)
{
	NSLog(@"HakiosMetal: %@", text);
}

/* -initWithAcceleratorPort: the fifth step of how Metal finds a device. */
static id
deviceInit(id self, SEL _cmd, unsigned int port)
{
	struct objc_super sup = { self, gBase };
	id (*send)(struct objc_super *, SEL) = (id (*)(struct objc_super *, SEL))objc_msgSendSuper;

	self = send(&sup, @selector(init));
	gPort = port;
	gDevice = self;                         /* what the objects it hands out answer to -device */
	say([NSString stringWithFormat:@"a device for the accelerator port %u (%@)", port, self ? @"made" : @"_MTLDevice's init gave nil"]);
	return self;
}

static id
deviceName(__unused id self, __unused SEL _cmd)
{
#if defined(HAKIOS_METAL_A618)
	return @"HakiOS Adreno 618";
#else
	return @"HakiOS Adreno 642L";
#endif
}

static uint64_t
deviceRegistryID(__unused id self, __unused SEL _cmd)
{
	return gPort;
}

/* the largest buffer: what the GPU memory could hold, not a promise (nothing allocates a buffer yet) */
static uint64_t
deviceMaxBufferLength(__unused id self, __unused SEL _cmd)
{
	return 256ull << 20;
}

/* the largest threadgroup: the Adreno 6xx's (a workgroup of 1024 invocations), as Apple's GPUs answer too */
static MTLSize
deviceMaxThreadsPerThreadgroup(__unused id self, __unused SEL _cmd)
{
	return MTLSizeMake(1024, 1024, 1024);
}

/*
 * The command queue: the first thing the window server wants after the device (the second run's log: "[Metal] Failed to create
 * MTLCommandQueue", then the process ends). Made as a subclass of Metal's private _MTLCommandQueue, at the same time as the device's
 * class, with the same stub for what it does not answer. It queues nothing and executes nothing: a command buffer asked of it is
 * nil, and the log says so, which is the next step's list.
 */
static Class gQueueClass;

#ifdef HAKIOS_METAL_GPU
/*
 * The GPU itself, through hakios-metal (github.com/oosexgang/hakios-metal, C++, its C interface hakios_metal.h): buffers in
 * the GPU's memory, command buffers, and blits done by the GPU's command processor. Without the kernel's GPU (no gpu=1,
 * QEMU) nothing of this is used and the device is what it was: buffers in plain memory, a queue that queues nothing.
 */
#include "hakios-metal/hakios_metal.h"
#include <IOSurface/IOSurfaceRef.h>

static void **stateOf(id self);

static hakios_metal_device *gGPU;               /* opened once; NULL: the kernel has no GPU to give */
static Class gCommandBufferClass, gBlitEncoderClass, gComputePipelineClass, gComputeEncoderClass;
static Class gTextureClass, gSamplerClass, gRenderPipelineClass, gRenderEncoderClass;

struct commandbuffer {
	hakios_metal_cb *cb;
	id queue;                               /* retained */
	NSMutableArray *handlers;               /* the completed handlers, copied */
	NSMutableArray *scheduled;              /* the scheduled handlers, copied */
	BOOL committed;
	id label;
};

struct blitencoder {
	id commandBuffer;                       /* retained */
	BOOL ended;
	id label;
};

static hakios_metal_device *
gpu(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
#if !TARGET_OS_IPHONE
		/* on a Mac, HAKIOS_METAL_FAKE=1: the core's fake GPU, which runs the command streams it is given, so that command
		 * buffers and blits are tried there too (plugin/tests/metal-plugin-test.sh, GPU=1) */
		gGPU = getenv("HAKIOS_METAL_FAKE") != NULL ? hakios_metal_open_fake() : hakios_metal_open();
#else
		gGPU = hakios_metal_open();
#endif
		say(gGPU != NULL ? [NSString stringWithFormat:@"the GPU: Adreno %#llx, through hakios-metal", hakios_metal_gpu_id(gGPU)]
		                 : [NSString stringWithFormat:@"no GPU (%s): buffers in plain memory, no command buffers", hakios_metal_error()]);
	});
	return gGPU;
}
#endif

static id
queueDevice(__unused id self, __unused SEL _cmd)
{
	return gDevice;
}

static id
queueLabel(__unused id self, __unused SEL _cmd)
{
	return @"HakiosMetalQueue";
}

static void
queueSetGPUPriority(__unused id self, __unused SEL _cmd, __unused unsigned long priority)
{
}

static id
deviceNewQueue(id self, __unused SEL _cmd)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id queue;

	gDevice = self;
	if (gQueueClass == Nil) {
		say(@"newCommandQueue: no queue class (this system has no _MTLCommandQueue): nil");
		return nil;
	}
	queue = plain(plain((id)gQueueClass, sel_registerName("alloc")), sel_registerName("init"));
#ifdef HAKIOS_METAL_GPU
	if (queue != nil && gpu() != NULL) {
		void **slot = stateOf(queue);
		hakios_metal_queue *q = hakios_metal_queue_new(gGPU);

		if (slot != NULL && q != NULL) {
			*slot = q;
			say(@"newCommandQueue: a queue of the GPU's");
			return queue;
		}
		say([NSString stringWithFormat:@"newCommandQueue: no GPU queue (%s)", hakios_metal_error()]);
	}
#endif
	say([NSString stringWithFormat:@"newCommandQueue: a queue that queues nothing (%@)", queue ? @"made" : @"_MTLCommandQueue's init gave nil"]);
	return queue;
}

static id
deviceNewQueueMax(id self, SEL _cmd, __unused unsigned long maxBuffers)
{
	return deviceNewQueue(self, _cmd);
}

/* the protocols the class declares (the ones Metal checks for), for the signatures below */
static Protocol * gProtocols[64];
static unsigned gProtocolCount;

/*
 * A message the class does not have, but a protocol it declares does: Metal sends the whole of MTLDevice and the private
 * MTLDeviceSPI, and a method that is missing would end the process ("unrecognized selector": -featureProfile was the first).
 * The type of the answer is read from the protocol's own description of the method, so that zero comes back in the right
 * size, and the message is logged once. What a method should answer is learned one message at a time from this log.
 */
static id
deviceSignature(id self, SEL _cmd, SEL selector)
{
	for (unsigned i = 0; i < gProtocolCount; i++) {
		for (int required = 1; required >= 0; required--) {
			struct objc_method_description d = protocol_getMethodDescription(gProtocols[i], selector, required, YES);

			if (d.name != NULL && d.types != NULL) {
				id (*make)(Class, SEL, const char *) = (id (*)(Class, SEL, const char *))objc_msgSend;

				return make(objc_getClass("NSMethodSignature"), sel_registerName("signatureWithObjCTypes:"), d.types);
			}
		}
	}
	struct objc_super sup = { self, gBase };
	id (*send)(struct objc_super *, SEL, SEL) = (id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper;

	(void)_cmd;
	return send(&sup, sel_registerName("methodSignatureForSelector:"), selector);
}

static void
deviceForward(id self, SEL _cmd, id invocation)
{
	static NSMutableSet * seen;
	SEL selector = ((SEL (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("selector"));
	id signature = ((id (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("methodSignature"));
	NSUInteger length = ((NSUInteger (*)(id, SEL))objc_msgSend)(signature, sel_registerName("methodReturnLength"));
	NSString * name = NSStringFromSelector(selector);

	(void)self;
	(void)_cmd;
	@synchronized(gBase) {
		if (seen == nil) {
			seen = [NSMutableSet new];
		}
		if (![seen containsObject:name]) {
			[seen addObject:name];
			const char * encoding = ((const char * (*)(id, SEL))objc_msgSend)(signature, sel_registerName("methodReturnType"));

			say([NSString stringWithFormat:@"asked and not implemented: -%@ (returns %lu bytes, type %s; answered with zero)", name,
			    (unsigned long)length, encoding ? encoding : "?"]);
		}
	}
	if (length != 0) {
		void * zero = calloc(1, length);

		((void (*)(id, SEL, void *))objc_msgSend)(invocation, sel_registerName("setReturnValue:"), zero);
		free(zero);
	}
}

/*
 * The objects a device hands out. Every one is a class of ours made at load time, a subclass of Metal's private base where this
 * system has one (and of NSObject where it does not), declaring the protocol Metal checks and any private protocol of the same
 * name family, answering the rest through the same logged zero stub as the device. None of them does anything: a state object
 * holds nothing, a buffer has memory and a length and nothing else. They exist so that the next question reaches the log, which is
 * the work list of this plug-in (docs/metal-plan.md).
 */
static const struct stubSpec {
	const char * selector;          /* the device's method that makes it */
	const char * cls;               /* our class */
	const char * base;              /* Metal's private base class, if the system has it */
	const char * protocol;          /* the public protocol, and the family of private ones beginning with it */
} gStubSpecs[] = {
	{ "newDepthStencilStateWithDescriptor:", "HakiosMetalDepthStencilState", "_MTLDepthStencilState", "MTLDepthStencilState" },
	{ "newEventWithOptions:", "HakiosMetalEvent", "_MTLEvent", "MTLEvent" },
	{ "newFence", "HakiosMetalFence", "_MTLFence", "MTLFence" },
};
#define STUB_COUNT (sizeof(gStubSpecs) / sizeof(gStubSpecs[0]))
static Class gStubClass[STUB_COUNT];

/* a device method that makes one of the objects above: the class is found by the selector it was called by */
static id
stubFactory(__unused id self, SEL _cmd)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char * called = sel_getName(_cmd);

	for (unsigned i = 0; i < STUB_COUNT; i++) {
		if (strcmp(gStubSpecs[i].selector, called) == 0) {
			id object;

			if (gStubClass[i] == Nil) {
				return nil;
			}
			object = plain(plain((id)gStubClass[i], sel_registerName("alloc")), sel_registerName("init"));
			say([NSString stringWithFormat:@"%s: a %s that holds nothing (%@)", called, gStubSpecs[i].cls, object ? @"made" : @"init gave nil"]);
			return object;
		}
	}
	return nil;
}

/*
 * STEP TWO (docs/metal-plan.md, steps 3 and 4 as far as they go without the GPU): objects that hold something.
 *
 *   buffers     -newBufferWithLength:options:, ...WithBytes:..., ...WithBytesNoCopy:...: memory a program writes and reads back
 *               through -contents. The Adreno shares the system's memory, so a buffer in shared storage is the same bytes
 *               for the CPU and, later, the GPU; until the GPU is used (hakios.gpu.cpp's buffers, mapped into its address
 *               space) it is ordinary memory of this process, zeroed, and -gpuAddress is 0.
 *   libraries   -newLibraryWithData:error:, -newLibraryWithURL:error:, -newLibraryWithFile:error:, -newDefaultLibrary...:
 *               the .metallib read (hakios_metallib.c, every offset checked), its functions listed by name and type, and
 *               -newFunctionWithName: giving a function that knows its name, its type and its module (the AIR bitcode the
 *               shader compiler of docs/air.md takes). Nothing is compiled yet: a pipeline made of such a function is still
 *               the empty stub above, and the log says so.
 *
 * Each is a class of ours made at load time, as the others are (a subclass of Metal's private _MTLResource, _MTLLibrary and
 * _MTLFunction where this system has them), with one instance variable, _hakios, pointing at its state in C.
 */
#include "hakios_metallib.h"
#include <dispatch/dispatch.h>
#include <sys/mman.h>

static Class gBufferClass, gLibraryClass, gFunctionClass;

struct buffer {
	void *memory;
	uint64_t length;
	unsigned long options;                  /* MTLResourceOptions: cache mode, storage mode, hazard tracking */
	void (^deallocator)(void *, NSUInteger); /* for ...NoCopy: the caller's, called with its memory when the buffer goes */
	BOOL mapped;                            /* ours, from mmap */
	id label;
	void *gpu;                              /* (HAKIOS_METAL_GPU) GPU memory of hakios-metal's, when the kernel has a GPU */
};

struct library {
	NSData *data;                           /* the file's bytes, kept for as long as a function of it may want its module */
	struct hakios_metallib info;
	struct hakios_metallib_function *functions;
	id label;
	id names;                               /* NSArray of the functions' names, made once */
};

struct function {
	id library;                             /* retained: the module is in its bytes */
	struct hakios_metallib_function f;
	id name;
	id label;
};

static struct library *libraryOf(id self);


static void **
stateOf(id self)
{
	Ivar ivar = class_getInstanceVariable(object_getClass(self), "_hakios");

	return ivar != NULL ? (void **)((char *)(__bridge void *)self + ivar_getOffset(ivar)) : NULL;
}

/* -dealloc: the state freed by FREE, then the superclass's own */
static void
deallocWith(id self, SEL _cmd, Class cls, void (*free_state)(void *))
{
	void **slot = stateOf(self);
	struct objc_super sup = { self, class_getSuperclass(cls) };

	if (slot != NULL && *slot != NULL) {
		free_state(*slot);
		*slot = NULL;
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}

/*
 * -dealloc of the queue (and of what is made like it): the state freed by FREE (NULL: none), then NSObject's own, not Metal's.
 * The queue is made with -init, not with Metal's initializer (-initWithDevice:), and _MTLCommandQueue's -dealloc lets go of what
 * that makes: its dispatch queue, NULL here. dispatch_release(NULL) faulted on the phone as metal_test's blits ended, and does
 * in plugin/tests/metal_plugin_test.m on a Mac (GPU=1).
 */
static void
deallocPlain(id self, SEL _cmd, void (*free_state)(void *))
{
	void **slot = stateOf(self);
	struct objc_super sup = { self, objc_getClass("NSObject") };

	if (free_state != NULL && slot != NULL && *slot != NULL) {
		free_state(*slot);
		*slot = NULL;
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}

static void
queueDealloc(id self, SEL _cmd)
{
#ifdef HAKIOS_METAL_GPU
	/* (no command buffer is in flight: each holds its queue) */
	deallocPlain(self, _cmd, (void (*)(void *))hakios_metal_queue_free);
#else
	deallocPlain(self, _cmd, NULL);
#endif
}

static id
labelGet(id self, __unused SEL _cmd)
{
	id *label = NULL;
	void **slot = stateOf(self);

	if (slot == NULL || *slot == NULL) {
		return nil;
	}
	if (object_getClass(self) == gBufferClass) {
		label = &((struct buffer *)*slot)->label;
	} else if (object_getClass(self) == gLibraryClass) {
		label = &((struct library *)*slot)->label;
#ifdef HAKIOS_METAL_GPU
	} else if (object_getClass(self) == gCommandBufferClass) {
		label = &((struct commandbuffer *)*slot)->label;
	} else if (object_getClass(self) == gBlitEncoderClass) {
		label = &((struct blitencoder *)*slot)->label;
	} else if (object_getClass(self) == gComputePipelineClass) {
		label = (id *)((char *)*slot + 16);
	} else if (object_getClass(self) == gComputeEncoderClass) {
		label = (id *)((char *)*slot + 824);
#endif
	} else {
		label = &((struct function *)*slot)->label;
	}
	return *label;
}

static void
labelSet(id self, __unused SEL _cmd, id text)
{
	void **slot = stateOf(self);
	id *label;

	if (slot == NULL || *slot == NULL) {
		return;
	}
	if (object_getClass(self) == gBufferClass) {
		label = &((struct buffer *)*slot)->label;
	} else if (object_getClass(self) == gLibraryClass) {
		label = &((struct library *)*slot)->label;
#ifdef HAKIOS_METAL_GPU
	} else if (object_getClass(self) == gCommandBufferClass) {
		label = &((struct commandbuffer *)*slot)->label;
	} else if (object_getClass(self) == gBlitEncoderClass) {
		label = &((struct blitencoder *)*slot)->label;
	} else if (object_getClass(self) == gComputePipelineClass) {
		label = (id *)((char *)*slot + 16);
	} else if (object_getClass(self) == gComputeEncoderClass) {
		label = (id *)((char *)*slot + 824);
#endif
	} else {
		label = &((struct function *)*slot)->label;
	}
	[*label release];
	*label = [text copy];
}

static id
objectDevice(__unused id self, __unused SEL _cmd)
{
	return gDevice;
}

/* ---- buffers ---- */

static void
bufferFree(void *state)
{
	struct buffer *b = state;

	if (b->mapped) {
		munmap(b->memory, b->length ? b->length : 1);
#ifdef HAKIOS_METAL_GPU
	} else if (b->gpu != NULL) {
		hakios_metal_buffer_free(b->gpu);       /* (a command buffer still using it keeps the memory until it is done) */
#endif
	} else if (b->deallocator != nil) {
		b->deallocator(b->memory, (NSUInteger)b->length);
	}
	[b->deallocator release];
	[b->label release];
	free(b);
}

static void
bufferDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gBufferClass, bufferFree);
}

static struct buffer *
bufferOf(id self)
{
	void **slot = stateOf(self);

	return slot != NULL ? *slot : NULL;
}

static void *
bufferContents(id self, __unused SEL _cmd)
{
	struct buffer *b = bufferOf(self);

	return b != NULL ? b->memory : NULL;
}

static unsigned long
bufferLength(id self, __unused SEL _cmd)
{
	struct buffer *b = bufferOf(self);

	return b != NULL ? (unsigned long)b->length : 0;
}

static unsigned long
bufferOptions(id self, __unused SEL _cmd)
{
	struct buffer *b = bufferOf(self);

	return b != NULL ? b->options : 0;
}

static unsigned long
bufferStorageMode(id self, SEL _cmd)
{
	return bufferOptions(self, _cmd) >> MTLResourceStorageModeShift & 0xf;
}

static unsigned long
bufferCacheMode(id self, SEL _cmd)
{
	return bufferOptions(self, _cmd) >> MTLResourceCPUCacheModeShift & 0xf;
}

static unsigned long
bufferHazardMode(id self, SEL _cmd)
{
	return bufferOptions(self, _cmd) >> MTLResourceHazardTrackingModeShift & 0x3;
}

/* the memory a buffer takes: its pages */
static unsigned long
bufferAllocatedSize(id self, SEL _cmd)
{
	unsigned long page = (unsigned long)getpagesize();

	return (bufferLength(self, _cmd) + page - 1) / page * page;
}

/* shared memory: what the CPU wrote is what there is; nothing to tell anyone */
static void
bufferDidModify(__unused id self, __unused SEL _cmd, __unused NSRange range)
{
}

/* no GPU address until the GPU's own buffers are used (hakios.gpu.cpp) */
static uint64_t
bufferGPUAddress(__unused id self, __unused SEL _cmd)
{
#ifdef HAKIOS_METAL_GPU
	struct buffer *b = bufferOf(self);

	if (b != NULL && b->gpu != NULL) {
		return hakios_metal_buffer_gpu_address(b->gpu);
	}
#endif
	return 0;
}

static unsigned long
bufferPurgeable(__unused id self, __unused SEL _cmd, __unused unsigned long state)
{
	return MTLPurgeableStateNonVolatile;
}

static id
bufferMake(void *memory, uint64_t length, unsigned long options, BOOL mapped, void (^deallocator)(void *, NSUInteger))
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	struct buffer *b;
	id buffer;
	void **slot;

	if (gBufferClass == Nil || (buffer = plain(plain((id)gBufferClass, sel_registerName("alloc")), sel_registerName("init"))) == nil) {
		say(@"a buffer: the class could not make one (its base's -init gave nil)");
		return nil;
	}
	b = calloc(1, sizeof(*b));
	slot = stateOf(buffer);
	if (b == NULL || slot == NULL) {
		free(b);
		[buffer release];
		return nil;
	}
	b->memory = memory;
	b->length = length;
	b->options = options;
	b->mapped = mapped;
	b->deallocator = [deallocator copy];
	*slot = b;
	return buffer;
}

static void *
pages(uint64_t length)
{
	void *memory = mmap(NULL, length ? length : 1, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

	return memory == MAP_FAILED ? NULL : memory;
}

static id
deviceNewBuffer(__unused id self, __unused SEL _cmd, unsigned long length, unsigned long options)
{
	void *memory;
	id buffer;

#ifdef HAKIOS_METAL_GPU
	if (gpu() != NULL && length <= deviceMaxBufferLength(nil, NULL)) {
		hakios_metal_buffer *g = hakios_metal_buffer_new(gGPU, length);

		if (g != NULL) {
			buffer = bufferMake(hakios_metal_buffer_contents(g), length, options, NO, nil);
			if (buffer == nil) {
				hakios_metal_buffer_free(g);
				return nil;
			}
			bufferOf(buffer)->gpu = g;
			return buffer;
		}
		say([NSString stringWithFormat:@"newBufferWithLength:%lu: no GPU memory (%s): plain memory instead", length, hakios_metal_error()]);
	}
#endif
	if (length > deviceMaxBufferLength(nil, NULL) || (memory = pages(length)) == NULL) {
		say([NSString stringWithFormat:@"newBufferWithLength:%lu: no (more than maxBufferLength, or no memory): nil", length]);
		return nil;
	}
	buffer = bufferMake(memory, length, options, YES, nil);
	if (buffer == nil) {
		munmap(memory, length ? length : 1);
	}
	return buffer;
}

static id
deviceNewBufferBytes(id self, SEL _cmd, const void *bytes, unsigned long length, unsigned long options)
{
	id buffer = deviceNewBuffer(self, _cmd, length, options);

	if (buffer != nil && bytes != NULL && length != 0) {
		memcpy(bufferContents(buffer, NULL), bytes, length);
	}
	return buffer;
}

static id
deviceNewBufferNoCopy(__unused id self, __unused SEL _cmd, void *bytes, unsigned long length, unsigned long options,
    void (^deallocator)(void *, NSUInteger))
{
	if (bytes == NULL || ((uintptr_t)bytes & (uintptr_t)(getpagesize() - 1)) != 0) {
		say(@"newBufferWithBytesNoCopy: the memory must start on a page (as Metal says): nil");
		return nil;
	}
	return bufferMake(bytes, length, options, NO, deallocator);
}

/* ---- functions ---- */

static void
functionFree(void *state)
{
	struct function *f = state;

	[f->library release];
	[f->name release];
	[f->label release];
	free(f);
}

static void
functionDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gFunctionClass, functionFree);
}

static struct function *
functionOf(id self)
{
	void **slot = stateOf(self);

	return slot != NULL ? *slot : NULL;
}

static id
functionName(id self, __unused SEL _cmd)
{
	struct function *f = functionOf(self);

	return f != NULL ? f->name : nil;
}

/* MTLFunctionType: the metallib's TYPE as Metal numbers it */
static unsigned long
functionType(id self, __unused SEL _cmd)
{
	struct function *f = functionOf(self);

	switch (f != NULL ? f->f.type : HAKIOS_METALLIB_NO_TYPE) {
	case HAKIOS_METALLIB_VERTEX:
		return MTLFunctionTypeVertex;
	case HAKIOS_METALLIB_FRAGMENT:
		return MTLFunctionTypeFragment;
	case HAKIOS_METALLIB_KERNEL:
		return MTLFunctionTypeKernel;
	case HAKIOS_METALLIB_INTERSECTION:
		return MTLFunctionTypeIntersection;
	default:
		return MTLFunctionTypeVisible;
	}
}

static id
functionNoAttributes(__unused id self, __unused SEL _cmd)
{
	return [NSArray array];
}

static id
functionNoConstants(__unused id self, __unused SEL _cmd)
{
	return [NSDictionary dictionary];
}

/* not Metal's: the function's AIR module, for the shader compiler (docs/air.md) */
static id
functionBitcode(id self, __unused SEL _cmd)
{
	struct function *f = functionOf(self);
	struct library *l;
	const uint8_t *bitcode;
	uint64_t length;

	if (f == NULL || (l = libraryOf(f->library)) == NULL ||
	    hakios_metallib_bitcode([l->data bytes], [l->data length], &f->f, &bitcode, &length) != 0) {
		return nil;
	}
	return [NSData dataWithBytesNoCopy:(void *)bitcode length:(NSUInteger)length freeWhenDone:NO];
}

/* ---- libraries ---- */

static void
libraryFree(void *state)
{
	struct library *l = state;

	[l->data release];
	[l->names release];
	[l->label release];
	free(l->functions);
	free(l);
}

static void
libraryDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gLibraryClass, libraryFree);
}

static struct library *
libraryOf(id self)
{
	void **slot = stateOf(self);

	return slot != NULL ? *slot : NULL;
}

static id
libraryFunctionNames(id self, __unused SEL _cmd)
{
	struct library *l = libraryOf(self);

	return l != NULL ? l->names : nil;
}

static id
libraryNewFunction(id self, __unused SEL _cmd, id name)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	struct library *l = libraryOf(self);
	const char *wanted = [name UTF8String];
	size_t length = wanted != NULL ? strlen(wanted) : 0;

	if (l == NULL || wanted == NULL) {
		return nil;
	}
	for (uint32_t i = 0; i < l->info.count; i++) {
		struct hakios_metallib_function *f = &l->functions[i];

		if (f->name_length == length && memcmp(f->name, wanted, length) == 0) {
			struct function *state = calloc(1, sizeof(*state));
			id function = gFunctionClass != Nil ? plain(plain((id)gFunctionClass, sel_registerName("alloc")), sel_registerName("init")) : nil;
			void **slot = function != nil ? stateOf(function) : NULL;

			if (state == NULL || slot == NULL) {
				free(state);
				[function release];
				say([NSString stringWithFormat:@"newFunctionWithName:%@: the function class could not make one: nil", name]);
				return nil;
			}
			state->library = [self retain];
			state->f = *f;
			state->name = [name copy];
			*slot = state;
			return function;
		}
	}
	return nil;
}

/* the same, with function constants: there is no compiler yet to specialise with them, so they are only said */
static id
libraryNewFunctionConstants(id self, SEL _cmd, id name, id constants, NSError **error)
{
	id function = libraryNewFunction(self, _cmd, name);

	say([NSString stringWithFormat:@"newFunctionWithName:%@ constantValues: the constants are not applied (no compiler yet)", name]);
	(void)constants;
	if (function == nil && error != NULL) {
		*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorFunctionNotFound
		    userInfo:@{ NSLocalizedDescriptionKey: [NSString stringWithFormat:@"no function %@ in the library", name] }];
	}
	return function;
}

static unsigned long
libraryType(__unused id self, __unused SEL _cmd)
{
	return MTLLibraryTypeExecutable;
}

static id
libraryError(MTLLibraryError code, NSString *text, NSError **error)
{
	say(text);
	if (error != NULL) {
		*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:code userInfo:@{ NSLocalizedDescriptionKey: text }];
	}
	return nil;
}

/* a library of the bytes of a .metallib (DATA retained by the library); WHERE names them in the log */
static id
libraryMake(NSData *data, NSString *where, NSError **error)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	struct library *l;
	const char *why = NULL;
	id library;
	void **slot;
	unsigned kinds[8] = { 0 };

	if (gLibraryClass == Nil) {
		return libraryError(MTLLibraryErrorInternal, @"a library: this system has no class for one", error);
	}
	/* a universal file (AIR64 beside code for Apple's GPUs): its AIR64 slice is the library, as Metal itself takes it */
	const uint8_t *thin;
	size_t thin_size;
	if (hakios_metallib_thin([data bytes], [data length], &thin, &thin_size, &why) != 0) {
		return libraryError(MTLLibraryErrorInternal, [NSString stringWithFormat:@"a library from %@: %s", where, why], error);
	}
	if (thin != [data bytes]) {
		data = [data subdataWithRange:NSMakeRange((NSUInteger)(thin - (const uint8_t *)[data bytes]), thin_size)];
		say([NSString stringWithFormat:@"a library from %@: a universal file, its AIR64 slice taken (%zu bytes)", where, thin_size]);
	}
	l = calloc(1, sizeof(*l));
	if (l == NULL) {
		return libraryError(MTLLibraryErrorInternal, @"a library: no memory", error);
	}
	if (hakios_metallib_parse([data bytes], [data length], &l->info, NULL, 0, &why) != 0 ||
	    (l->functions = calloc(l->info.count ? l->info.count : 1, sizeof(*l->functions))) == NULL ||
	    hakios_metallib_parse([data bytes], [data length], &l->info, l->functions, l->info.count, &why) != 0) {
		free(l->functions);
		free(l);
		return libraryError(MTLLibraryErrorInternal, [NSString stringWithFormat:@"a library from %@: %s", where,
		    why ? why : "no memory"], error);
	}
	NSMutableArray *names = [NSMutableArray arrayWithCapacity:l->info.count];
	for (uint32_t i = 0; i < l->info.count; i++) {
		NSString *name = [[NSString alloc] initWithBytes:l->functions[i].name length:l->functions[i].name_length encoding:NSUTF8StringEncoding];

		if (name != nil) {
			[names addObject:name];
			[name release];
		}
		kinds[l->functions[i].type < 7 ? l->functions[i].type : 7]++;
	}
	library = plain(plain((id)gLibraryClass, sel_registerName("alloc")), sel_registerName("init"));
	slot = library != nil ? stateOf(library) : NULL;
	if (slot == NULL) {
		free(l->functions);
		free(l);
		[library release];
		return libraryError(MTLLibraryErrorInternal, @"a library: the class could not make one (its base's -init gave nil)", error);
	}
	l->data = [data retain];
	l->names = [names copy];
	*slot = l;
	say([NSString stringWithFormat:@"a library from %@: %u functions (%u vertex, %u fragment, %u kernel, %u other), container %u.%u",
	    where, l->info.count, kinds[0], kinds[1], kinds[2], l->info.count - kinds[0] - kinds[1] - kinds[2],
	    l->info.version[0], l->info.version[1]]);
	return library;
}

static id
deviceNewLibraryURL(__unused id self, __unused SEL _cmd, NSURL *url, NSError **error)
{
	NSData *data = url != nil ? [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:NULL] : nil;

	if (data == nil) {
		return libraryError(MTLLibraryErrorFileNotFound, [NSString stringWithFormat:@"a library from %@: no such file", url], error);
	}
	return libraryMake(data, [url path], error);
}

static id
deviceNewLibraryFile(id self, SEL _cmd, NSString *path, NSError **error)
{
	return deviceNewLibraryURL(self, _cmd, path != nil ? [NSURL fileURLWithPath:path] : nil, error);
}

static id
deviceNewLibraryData(__unused id self, __unused SEL _cmd, dispatch_data_t data, NSError **error)
{
	const void *bytes = NULL;
	size_t size = 0;
	dispatch_data_t map;
	NSData *copy;

	if (data == nil) {
		return libraryError(MTLLibraryErrorFileNotFound, @"a library from data: no data", error);
	}
	map = dispatch_data_create_map(data, &bytes, &size);
	copy = [NSData dataWithBytes:bytes length:size];        /* the caller may let go of its data; the library keeps its own */
	dispatch_release(map);
	return libraryMake(copy, @"data", error);
}

/* a bundle's default.metallib: the main bundle's for -newDefaultLibrary */
static id
deviceNewDefaultLibraryBundle(id self, SEL _cmd, NSBundle *bundle, NSError **error)
{
	NSURL *url = [bundle URLForResource:@"default" withExtension:@"metallib"];

	if (url == nil) {
		return libraryError(MTLLibraryErrorFileNotFound, [NSString stringWithFormat:@"the default library of %@: no default.metallib",
		    [bundle bundlePath]], error);
	}
	return deviceNewLibraryURL(self, _cmd, url, error);
}

static id
deviceNewDefaultLibrary(id self, SEL _cmd)
{
	return deviceNewDefaultLibraryBundle(self, _cmd, [NSBundle mainBundle], NULL);
}

static BOOL
deviceUnifiedMemory(__unused id self, __unused SEL _cmd)
{
	return YES;                             /* the Adreno has no memory of its own */
}

/*
 * What CoreImage's CIMetalDeviceGetLimits reads of a device and _MTLDevice does not answer (Apple's own GPU devices do): without
 * them every CIContext creation throws, and a process drawing symbols through CoreImage (iconservicesagent) ends. UniXNU's
 * AdrenoMetal found this on iPadOS 17; the answers are the same here: no memory of its own, the system's shared.
 */
static uint64_t
deviceNoDedicatedMemory(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static uint64_t
deviceSharedMemory(__unused id self, __unused SEL _cmd)
{
	return [[NSProcessInfo processInfo] physicalMemory];
}

static BOOL
deviceNo(__unused id self, __unused SEL _cmd)
{
	return NO;
}

/* no legacy feature set is claimed: _MTLDevice's own answer reads tables of Apple's GPUs */
static BOOL
deviceNoFeatureSet(__unused id self, __unused SEL _cmd, __unused unsigned long set)
{
	return NO;
}

/*
 * The public protocol and Metal's private one of the same name (MTLBufferSPI ...), where this system has it: not every protocol
 * whose name begins the same, as for the stub objects, because MTLFunction begins a dozen others (MTLFunctionStitchingNode ...)
 * that a function is not.
 */
static void
declareProtocols(Class cls, const char *const *names)
{
	char spi[96];

	for (; *names != NULL; names++) {
		for (int private = 0; private < 2; private++) {
			Protocol *protocol;

			snprintf(spi, sizeof(spi), "%s%s", *names, private ? "SPI" : "");
			if ((protocol = objc_getProtocol(spi)) == NULL) {
				continue;
			}
			class_addProtocol(cls, protocol);
			if (gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
				gProtocols[gProtocolCount++] = protocol;
			}
		}
	}
}

/* a class of ours with the _hakios slot, on Metal's private BASE where there is one */
static Class
objectClass(const char *name, const char *base, const char *const *protocols, IMP dealloc)
{
	Class super = objc_getClass(base);
	Class cls = objc_allocateClassPair(super != Nil ? super : objc_getClass("NSObject"), name, 0);

	if (cls == Nil) {
		say([NSString stringWithFormat:@"could not make the class %s", name]);
		return Nil;
	}
	class_addIvar(cls, "_hakios", sizeof(void *), sizeof(void *) == 8 ? 3 : 2, "^v");
	declareProtocols(cls, protocols);
	class_addMethod(cls, sel_registerName("dealloc"), dealloc, "v@:");
	class_addMethod(cls, sel_registerName("device"), (IMP)objectDevice, "@@:");
	class_addMethod(cls, sel_registerName("label"), (IMP)labelGet, "@@:");
	class_addMethod(cls, sel_registerName("setLabel:"), (IMP)labelSet, "v@:@");
	class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP)deviceSignature, "@@::");
	class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP)deviceForward, "v@:@");
	say([NSString stringWithFormat:@"%s is registered (a subclass of %s)", name, super != Nil ? base : "NSObject: this system has no such class"]);
	return cls;
}

#ifdef HAKIOS_METAL_GPU
static id deviceNewComputePipeline(id self, SEL _cmd, id function, NSError **error);
static void deviceNewComputePipelineAsync(id self, SEL _cmd, id function, void (^completion)(id, NSError *));
static id bufferNewTexture(id self, SEL _cmd, id desc, unsigned long offset, unsigned long bytesPerRow);
static void registerTextureAndSamplerClasses(Class device);
#endif

static void
registerObjectClasses(Class device)
{
	static const char *const buffer[] = { "MTLBuffer", "MTLResource", NULL }, *const function[] = { "MTLFunction", NULL },
	    *const library[] = { "MTLLibrary", NULL };

	if ((gBufferClass = objectClass("HakiosMetalBuffer", "_MTLResource", buffer, (IMP)bufferDealloc)) != Nil) {
		class_addMethod(gBufferClass, sel_registerName("contents"), (IMP)bufferContents, "^v@:");
		class_addMethod(gBufferClass, sel_registerName("length"), (IMP)bufferLength, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("resourceOptions"), (IMP)bufferOptions, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("storageMode"), (IMP)bufferStorageMode, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("cpuCacheMode"), (IMP)bufferCacheMode, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("hazardTrackingMode"), (IMP)bufferHazardMode, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("allocatedSize"), (IMP)bufferAllocatedSize, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("didModifyRange:"), (IMP)bufferDidModify, "v@:{_NSRange=QQ}");
		class_addMethod(gBufferClass, sel_registerName("gpuAddress"), (IMP)bufferGPUAddress, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("setPurgeableState:"), (IMP)bufferPurgeable, "Q@:Q");
#ifdef HAKIOS_METAL_GPU
		class_addMethod(gBufferClass, sel_registerName("newTextureWithDescriptor:offset:bytesPerRow:"), (IMP)bufferNewTexture, "@@:@QQ");
#endif
		objc_registerClassPair(gBufferClass);
		class_addMethod(device, sel_registerName("newBufferWithLength:options:"), (IMP)deviceNewBuffer, "@@:QQ");
		class_addMethod(device, sel_registerName("newBufferWithBytes:length:options:"), (IMP)deviceNewBufferBytes, "@@:r^vQQ");
		class_addMethod(device, sel_registerName("newBufferWithBytesNoCopy:length:options:deallocator:"), (IMP)deviceNewBufferNoCopy,
		    "@@:^vQQ@?");
	}
	if ((gFunctionClass = objectClass("HakiosMetalFunction", "_MTLFunction", function, (IMP)functionDealloc)) != Nil) {
		class_addMethod(gFunctionClass, sel_registerName("name"), (IMP)functionName, "@@:");
		class_addMethod(gFunctionClass, sel_registerName("functionType"), (IMP)functionType, "Q@:");
		class_addMethod(gFunctionClass, sel_registerName("vertexAttributes"), (IMP)functionNoAttributes, "@@:");
		class_addMethod(gFunctionClass, sel_registerName("stageInputAttributes"), (IMP)functionNoAttributes, "@@:");
		class_addMethod(gFunctionClass, sel_registerName("functionConstantsDictionary"), (IMP)functionNoConstants, "@@:");
		class_addMethod(gFunctionClass, sel_registerName("hakiosBitcode"), (IMP)functionBitcode, "@@:");
		objc_registerClassPair(gFunctionClass);
	}
	if ((gLibraryClass = objectClass("HakiosMetalLibrary", "_MTLLibrary", library, (IMP)libraryDealloc)) != Nil) {
		class_addMethod(gLibraryClass, sel_registerName("functionNames"), (IMP)libraryFunctionNames, "@@:");
		class_addMethod(gLibraryClass, sel_registerName("newFunctionWithName:"), (IMP)libraryNewFunction, "@@:@");
		class_addMethod(gLibraryClass, sel_registerName("newFunctionWithName:constantValues:error:"), (IMP)libraryNewFunctionConstants,
		    "@@:@@^@");
		class_addMethod(gLibraryClass, sel_registerName("type"), (IMP)libraryType, "Q@:");
		objc_registerClassPair(gLibraryClass);
		class_addMethod(device, sel_registerName("newLibraryWithURL:error:"), (IMP)deviceNewLibraryURL, "@@:@^@");
		class_addMethod(device, sel_registerName("newLibraryWithFile:error:"), (IMP)deviceNewLibraryFile, "@@:@^@");
		class_addMethod(device, sel_registerName("newLibraryWithData:error:"), (IMP)deviceNewLibraryData, "@@:@^@");
		class_addMethod(device, sel_registerName("newDefaultLibrary"), (IMP)deviceNewDefaultLibrary, "@@:");
		class_addMethod(device, sel_registerName("newDefaultLibraryWithBundle:error:"), (IMP)deviceNewDefaultLibraryBundle, "@@:@^@");
	}
	class_addMethod(device, sel_registerName("hasUnifiedMemory"), (IMP)deviceUnifiedMemory, "B@:");
	class_addMethod(device, sel_registerName("dedicatedMemorySize"), (IMP)deviceNoDedicatedMemory, "Q@:");
	class_addMethod(device, sel_registerName("sharedMemorySize"), (IMP)deviceSharedMemory, "Q@:");
	class_addMethod(device, sel_registerName("isLowPower"), (IMP)deviceNo, "B@:");
	class_addMethod(device, sel_registerName("isRemovable"), (IMP)deviceNo, "B@:");
	class_addMethod(device, sel_registerName("isHeadless"), (IMP)deviceNo, "B@:");
	class_addMethod(device, sel_registerName("supportsFeatureSet:"), (IMP)deviceNoFeatureSet, "B@:Q");
#ifdef HAKIOS_METAL_GPU
	class_addMethod(device, sel_registerName("newComputePipelineStateWithFunction:error:"), (IMP)deviceNewComputePipeline, "@@:@^@");
	class_addMethod(device, sel_registerName("newComputePipelineStateWithFunction:completionHandler:"), (IMP)deviceNewComputePipelineAsync, "v@:@@?");
	registerTextureAndSamplerClasses(device);
#endif
}

/* Declares `protocol` and every private protocol whose name begins with it on `cls`, and keeps them for the stub's signatures. */
static void
declareProtocolFamily(Class cls, const char * protocolName)
{
	unsigned int count = 0;
	Protocol * __unsafe_unretained * all = objc_copyProtocolList(&count);

	for (unsigned int i = 0; i < count; i++) {
		if (strncmp(protocol_getName(all[i]), protocolName, strlen(protocolName)) == 0) {
			class_addProtocol(cls, all[i]);
			if (gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
				gProtocols[gProtocolCount++] = all[i];
			}
		}
	}
	free(all);
}

static void
registerStubClasses(Class device)
{
	for (unsigned i = 0; i < STUB_COUNT; i++) {
		Class base = objc_getClass(gStubSpecs[i].base);
		Class cls = objc_allocateClassPair(base != Nil ? base : objc_getClass("NSObject"), gStubSpecs[i].cls, 0);

		if (cls == Nil) {
			say([NSString stringWithFormat:@"could not make the class %s", gStubSpecs[i].cls]);
			continue;
		}
		declareProtocolFamily(cls, gStubSpecs[i].protocol);
		class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP)deviceSignature, "@@::");
		class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP)deviceForward, "v@:@");
		objc_registerClassPair(cls);
		gStubClass[i] = cls;
		class_addMethod(device, sel_registerName(gStubSpecs[i].selector), (IMP)stubFactory, "@@:@");
		say([NSString stringWithFormat:@"%s is a %s (a subclass of %s)", gStubSpecs[i].selector, gStubSpecs[i].cls,
		    base != Nil ? gStubSpecs[i].base : "NSObject: this system has no such class"]);
	}
}

#ifdef HAKIOS_METAL_GPU
/* ---- command buffers and blits, on the GPU (hakios-metal) ---- */

static void *
slotState(id self)
{
	void **slot = stateOf(self);

	return slot != NULL ? *slot : NULL;
}

static void
commandBufferFree(void *state)
{
	struct commandbuffer *c = state;

	hakios_metal_cb_free(c->cb);
	[c->handlers release];
	[c->scheduled release];
	[c->queue release];
	[c->label release];
	free(c);
}

static void
commandBufferDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gCommandBufferClass, commandBufferFree);
}

static id
queueCommandBuffer(id self, __unused SEL _cmd)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	hakios_metal_queue *q = slotState(self);
	struct commandbuffer *c;
	void **slot;
	id cb;

	if (q == NULL || gCommandBufferClass == Nil) {
		say(@"commandBuffer: the queue has no GPU queue: nil");
		return nil;
	}
	if ((cb = plain(plain((id)gCommandBufferClass, sel_registerName("alloc")), sel_registerName("init"))) == nil) {
		return nil;
	}
	c = calloc(1, sizeof(*c));
	slot = stateOf(cb);
	if (c == NULL || slot == NULL || (c->cb = hakios_metal_cb_new(q)) == NULL) {
		free(c);
		[cb release];
		return nil;
	}
	c->queue = [self retain];
	c->handlers = [[NSMutableArray alloc] init];
	c->scheduled = [[NSMutableArray alloc] init];
	*slot = c;
	return [cb autorelease];
}

/* on the queue's thread, once the GPU is done with it (or it ended in error): its handlers, then the reference commit took */
static void
commandBufferDone(__unused hakios_metal_cb *cb, void *context)
{
	id self = (id)context;
	struct commandbuffer *c = slotState(self);

	@autoreleasepool {
		for (void (^handler)(id) in c->handlers) {
			handler(self);
		}
	}
	[self release];
}

static void
commandBufferCommit(id self, __unused SEL _cmd)
{
	struct commandbuffer *c = slotState(self);

	if (c == NULL || c->committed) {
		return;
	}
	c->committed = YES;
	for (void (^handler)(id) in c->scheduled) {
		handler(self);
	}
	hakios_metal_cb_commit(c->cb, commandBufferDone, [self retain]);
}

static void
commandBufferEnqueue(__unused id self, __unused SEL _cmd)
{
}

static void
commandBufferWait(id self, __unused SEL _cmd)
{
	struct commandbuffer *c = slotState(self);

	if (c != NULL && c->committed) {
		hakios_metal_cb_wait(c->cb);    /* (its completed handlers have run when this returns) */
	}
}

static void
commandBufferWaitScheduled(__unused id self, __unused SEL _cmd)
{
}

static void
commandBufferAddHandler(id self, __unused SEL _cmd, void (^handler)(id))
{
	struct commandbuffer *c = slotState(self);

	if (c != NULL && !c->committed && handler != nil) {
		[c->handlers addObject:[[handler copy] autorelease]];
	}
}

static void
commandBufferAddScheduled(id self, __unused SEL _cmd, void (^handler)(id))
{
	struct commandbuffer *c = slotState(self);

	if (c != NULL && !c->committed && handler != nil) {
		[c->scheduled addObject:[[handler copy] autorelease]];
	}
}

static unsigned long
commandBufferStatus(id self, __unused SEL _cmd)
{
	struct commandbuffer *c = slotState(self);

	return c == NULL || !c->committed ? MTLCommandBufferStatusNotEnqueued : (unsigned long)hakios_metal_cb_status(c->cb);
}

static id
commandBufferError(id self, __unused SEL _cmd)
{
	struct commandbuffer *c = slotState(self);

	if (c == NULL || !c->committed || hakios_metal_cb_status(c->cb) != HAKIOS_METAL_ERROR) {
		return nil;
	}
	return [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
	    userInfo:@{ NSLocalizedDescriptionKey: [NSString stringWithUTF8String:hakios_metal_cb_error(c->cb)] }];
}

static id
commandBufferQueue(id self, __unused SEL _cmd)
{
	struct commandbuffer *c = slotState(self);

	return c != NULL ? c->queue : nil;
}

static double
commandBufferNoTime(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static void
blitEncoderFree(void *state)
{
	struct blitencoder *e = state;

	[e->commandBuffer release];
	[e->label release];
	free(e);
}

static void
blitEncoderDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gBlitEncoderClass, blitEncoderFree);
}

static id
commandBufferBlitEncoder(id self, __unused SEL _cmd)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	struct commandbuffer *c = slotState(self);
	struct blitencoder *e;
	void **slot;
	id encoder;

	if (c == NULL || c->committed || gBlitEncoderClass == Nil ||
	    (encoder = plain(plain((id)gBlitEncoderClass, sel_registerName("alloc")), sel_registerName("init"))) == nil) {
		return nil;
	}
	e = calloc(1, sizeof(*e));
	slot = stateOf(encoder);
	if (e == NULL || slot == NULL) {
		free(e);
		[encoder release];
		return nil;
	}
	e->commandBuffer = [self retain];
	*slot = e;
	return [encoder autorelease];
}

/* the command buffer an encoder records into, while it may */
static struct commandbuffer *
recording(id encoder)
{
	struct blitencoder *e = slotState(encoder);
	struct commandbuffer *c = e != NULL && !e->ended ? slotState(e->commandBuffer) : NULL;

	return c != NULL && !c->committed ? c : NULL;
}

/* a buffer of the GPU's, or NULL (made from the caller's memory, or not ours at all) */
static hakios_metal_buffer *
gpuBufferOf(id buffer)
{
	struct buffer *b = object_getClass(buffer) == gBufferClass ? bufferOf(buffer) : NULL;

	return b != NULL ? b->gpu : NULL;
}

static void
blitCopy(id self, __unused SEL _cmd, id from, unsigned long fromOffset, id to, unsigned long toOffset, unsigned long size)
{
	struct commandbuffer *c = recording(self);
	hakios_metal_buffer *a = gpuBufferOf(from), *b = gpuBufferOf(to);

	if (c == NULL) {
		return;
	}
	if (a == NULL || b == NULL) {
		hakios_metal_cb_fail(c->cb, "copyFromBuffer: a buffer that is not in the GPU's memory (made with ...BytesNoCopy?)");
	} else if (hakios_metal_cb_copy(c->cb, a, fromOffset, b, toOffset, size) != 0) {
		say([NSString stringWithFormat:@"copyFromBuffer: %s", hakios_metal_error()]);
	}
}

static void
blitFill(id self, __unused SEL _cmd, id buffer, NSRange range, unsigned char value)
{
	struct commandbuffer *c = recording(self);
	hakios_metal_buffer *b = gpuBufferOf(buffer);

	if (c == NULL) {
		return;
	}
	if (b == NULL) {
		hakios_metal_cb_fail(c->cb, "fillBuffer: a buffer that is not in the GPU's memory");
	} else if (hakios_metal_cb_fill(c->cb, b, range.location, range.length, value) != 0) {
		say([NSString stringWithFormat:@"fillBuffer: %s", hakios_metal_error()]);
	}
}

static void
blitEnd(id self, __unused SEL _cmd)
{
	struct blitencoder *e = slotState(self);

	if (e != NULL) {
		e->ended = YES;
	}
}

static void
blitNothing(__unused id self, __unused SEL _cmd, __unused id resource)
{
	/* synchronizeResource: the memory is the CPU's and the GPU's both (write-combined): there is nothing to copy back */
}

struct compute_pipeline {
	hakios_metal_kernel *kernel;
	id function;
	id label;
};

struct compute_encoder {
	id commandBuffer;
	id pipeline;
	hakios_metal_kernel *kernel;
	hakios_metal_buffer *buffers[31];
	uint64_t offsets[31];
	unsigned maxBuffer;
	hakios_metal_texture *textures[31];
	unsigned maxTexture;
	hakios_metal_sampler *samplers[16];
	unsigned maxSampler;
	BOOL ended;
	id label;
};

static void
computePipelineFree(void *state)
{
	struct compute_pipeline *cp = (struct compute_pipeline *)state;

	if (cp->kernel != NULL) {
		hakios_metal_kernel_free(cp->kernel);
	}
	[cp->function release];
	[cp->label release];
	free(cp);
}

static void
computePipelineDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gComputePipelineClass, computePipelineFree);
}

static unsigned long
computePipelineMaxThreads(id self, __unused SEL _cmd)
{
	struct compute_pipeline *cp = slotState(self);
	unsigned m = cp && cp->kernel ? hakios_metal_kernel_max_threads(cp->kernel) : 1024;
	return m ? m : 1024;
}

static unsigned long
computePipelineExecutionWidth(id self, __unused SEL _cmd)
{
	struct compute_pipeline *cp = slotState(self);
	unsigned w = cp && cp->kernel ? hakios_metal_kernel_execution_width(cp->kernel) : 64;
	return w ? w : 64;
}

static unsigned long
computePipelineThreadgroupMemory(id self, __unused SEL _cmd)
{
	struct compute_pipeline *cp = slotState(self);
	return cp && cp->kernel ? hakios_metal_kernel_threadgroup_memory(cp->kernel) : 0;
}

static void
computeEncoderFree(void *state)
{
	struct compute_encoder *e = (struct compute_encoder *)state;

	[e->commandBuffer release];
	[e->pipeline release];
	[e->label release];
	free(e);
}

static void
computeEncoderDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gComputeEncoderClass, computeEncoderFree);
}

static void
computeSetPipeline(id self, __unused SEL _cmd, id pipeline)
{
	struct compute_encoder *e = slotState(self);
	if (e == NULL || e->ended) return;
	[e->pipeline release];
	e->pipeline = [pipeline retain];
	void **slot = stateOf(pipeline);
	struct compute_pipeline *cp = slot != NULL ? (struct compute_pipeline *)*slot : NULL;
	e->kernel = cp != NULL ? cp->kernel : NULL;
}

static void
computeSetBuffer(id self, __unused SEL _cmd, id buffer, unsigned long offset, unsigned long index)
{
	struct compute_encoder *e = slotState(self);
	if (e == NULL || e->ended || index >= 31) return;
	e->buffers[index] = gpuBufferOf(buffer);
	e->offsets[index] = offset;
	if (index + 1 > e->maxBuffer) e->maxBuffer = (unsigned)(index + 1);
}

static void
computeSetBuffers(id self, __unused SEL _cmd, const id *buffers, const unsigned long *offsets, NSRange range)
{
	for (NSUInteger k = 0; k < range.length; k++) {
		computeSetBuffer(self, _cmd, buffers[k], offsets[k], range.location + k);
	}
}

static void
computeDispatch(id self, __unused SEL _cmd, MTLSize groups, MTLSize threads)
{
	struct compute_encoder *e = slotState(self);
	struct commandbuffer *c = e != NULL && !e->ended ? slotState(e->commandBuffer) : NULL;
	if (c == NULL || e->kernel == NULL) {
		say(@"dispatchThreadgroups: no command buffer or no compute pipeline set");
		return;
	}
	unsigned g[3] = { (unsigned)groups.width, (unsigned)groups.height, (unsigned)groups.depth };
	unsigned t[3] = { (unsigned)threads.width, (unsigned)threads.height, (unsigned)threads.depth };
	if (hakios_metal_cb_dispatch2(c->cb, e->kernel, g, t, e->buffers, e->offsets, e->maxBuffer,
	                             e->textures, e->maxTexture, e->samplers, e->maxSampler) != 0) {
		say([NSString stringWithFormat:@"dispatchThreadgroups: %s", hakios_metal_error()]);
	}
}

static void
computeDispatchThreads(id self, SEL _cmd, MTLSize threads, MTLSize threadsPerGroup)
{
	if (threadsPerGroup.width == 0 || threadsPerGroup.height == 0 || threadsPerGroup.depth == 0) return;
	MTLSize groups;
	groups.width = (threads.width + threadsPerGroup.width - 1) / threadsPerGroup.width;
	groups.height = (threads.height + threadsPerGroup.height - 1) / threadsPerGroup.height;
	groups.depth = (threads.depth + threadsPerGroup.depth - 1) / threadsPerGroup.depth;
	computeDispatch(self, _cmd, groups, threadsPerGroup);
}

static void
computeEnd(id self, __unused SEL _cmd)
{
	struct compute_encoder *e = slotState(self);
	if (e != NULL) {
		e->ended = YES;
	}
}

static hakios_metal_texture *gpuTextureOf(id texture);
static hakios_metal_sampler *gpuSamplerOf(id sampler);

static void
computeSetTexture(id self, __unused SEL _cmd, id texture, unsigned long index)
{
	struct compute_encoder *e = slotState(self);
	if (e == NULL || e->ended || index >= 31) return;
	e->textures[index] = gpuTextureOf(texture);
	if (index + 1 > e->maxTexture) e->maxTexture = (unsigned)(index + 1);
}

static void
computeSetTextures(id self, SEL _cmd, const id *textures, NSRange range)
{
	for (NSUInteger k = 0; k < range.length; k++) {
		computeSetTexture(self, _cmd, textures[k], range.location + k);
	}
}

static void
computeSetSampler(id self, __unused SEL _cmd, id sampler, unsigned long index)
{
	struct compute_encoder *e = slotState(self);
	if (e == NULL || e->ended || index >= 16) return;
	e->samplers[index] = gpuSamplerOf(sampler);
	if (index + 1 > e->maxSampler) e->maxSampler = (unsigned)(index + 1);
}

static void
computeSetSamplers(id self, SEL _cmd, const id *samplers, NSRange range)
{
	for (NSUInteger k = 0; k < range.length; k++) {
		computeSetSampler(self, _cmd, samplers[k], range.location + k);
	}
}

struct texture_data {
	hakios_metal_texture *tex;
	id buffer;        /* MTLBuffer, if created over buffer */
	id iosurface;     /* IOSurface, if created over IOSurface */
	id parentTexture; /* MTLTexture, if created as view */
	id label;
};

static void
textureFree(void *state)
{
	struct texture_data *t = (struct texture_data *)state;
	if (t->tex != NULL) {
		hakios_metal_texture_free(t->tex);
	}
	[t->buffer release];
	[t->iosurface release];
	[t->parentTexture release];
	[t->label release];
	free(t);
}

static void
textureDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gTextureClass, textureFree);
}

static hakios_metal_texture *
gpuTextureOf(id texture)
{
	if (texture == nil) return NULL;
	void **slot = stateOf(texture);
	if (slot == NULL || *slot == NULL) return NULL;
	struct texture_data *t = (struct texture_data *)*slot;
	return t->tex;
}

static unsigned long
textureWidth(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_width(t) : 0;
}

static unsigned long
textureHeight(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_height(t) : 0;
}

static unsigned long
textureDepth(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_depth(t) : 1;
}

static unsigned long
textureMipmapLevels(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_levels(t) : 1;
}

static unsigned long
textureType(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_type(t) : 2 /* MTLTextureType2D */;
}

static unsigned long
texturePixelFormat(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_format(t) : 0;
}

static unsigned long
textureUsage(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_usage(t) : 0;
}

static unsigned long
textureSamples(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_samples(t) : 1;
}

static unsigned long
textureArrayLength(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	if (t == NULL) return 1;
	unsigned type = hakios_metal_texture_type(t);
	return (type == 3 || type == 6) ? hakios_metal_texture_depth(t) : 1;
}

static unsigned long
textureAllocatedSize(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_allocated_bytes(t) : 0;
}

static uint64_t
textureGpuAddress(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_gpu_address(t) : 0;
}

static uint64_t
textureBytesPerRow(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_bytes_per_row(t) : 0;
}

static id
textureBuffer(id self, __unused SEL _cmd)
{
	void **slot = stateOf(self);
	if (slot == NULL || *slot == NULL) return nil;
	struct texture_data *t = (struct texture_data *)*slot;
	return t->buffer;
}

static unsigned long
textureBufferOffset(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static id
textureIOSurface(id self, __unused SEL _cmd)
{
	void **slot = stateOf(self);
	if (slot == NULL || *slot == NULL) return nil;
	struct texture_data *t = (struct texture_data *)*slot;
	return t->iosurface;
}

static unsigned long
textureIOSurfacePlane(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static uint64_t
textureGpuResourceID(id self, __unused SEL _cmd)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	return t ? hakios_metal_texture_resource_id(t) : 0;
}

static void
textureReplaceRegion(id self, __unused SEL _cmd, MTLRegion region, unsigned long level, const void *bytes, unsigned long bytesPerRow)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	if (t == NULL || bytes == NULL) return;
	hakios_metal_texture_replace_level(t, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y,
	                                   (unsigned)region.size.width, (unsigned)region.size.height, bytes, bytesPerRow);
}

static void
textureReplaceRegion3D(id self, __unused SEL _cmd, MTLRegion region, unsigned long level, unsigned long slice,
                       const void *bytes, unsigned long bytesPerRow, unsigned long bytesPerImage)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	if (t == NULL || bytes == NULL) return;
	hakios_metal_texture_replace_3d(t, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y,
	                                (unsigned)(region.origin.z + slice), (unsigned)region.size.width, (unsigned)region.size.height,
	                                (unsigned)region.size.depth, bytes, bytesPerRow, bytesPerImage);
}

static void
textureGetBytes(id self, __unused SEL _cmd, void *bytes, unsigned long bytesPerRow, MTLRegion region, unsigned long level)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	if (t == NULL || bytes == NULL) return;
	hakios_metal_texture_read_level(t, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y,
	                                (unsigned)region.size.width, (unsigned)region.size.height, bytes, bytesPerRow);
}

static void
textureGetBytes3D(id self, __unused SEL _cmd, void *bytes, unsigned long bytesPerRow, unsigned long bytesPerImage,
                  MTLRegion region, unsigned long level, unsigned long slice)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	if (t == NULL || bytes == NULL) return;
	hakios_metal_texture_read_3d(t, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y,
	                             (unsigned)(region.origin.z + slice), (unsigned)region.size.width, (unsigned)region.size.height,
	                             (unsigned)region.size.depth, bytes, bytesPerRow, bytesPerImage);
}

static id
textureWrap(hakios_metal_texture *t, id buffer, id iosurface, id parent)
{
	if (t == NULL) return nil;
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id obj = plain(plain((id)gTextureClass, sel_registerName("alloc")), sel_registerName("init"));
	if (obj == nil) {
		hakios_metal_texture_free(t);
		return nil;
	}
	void **slot = stateOf(obj);
	struct texture_data *td = calloc(1, sizeof(*td));
	if (td == NULL || slot == NULL) {
		free(td);
		[obj release];
		hakios_metal_texture_free(t);
		return nil;
	}
	td->tex = t;
	td->buffer = [buffer retain];
	td->iosurface = [iosurface retain];
	td->parentTexture = [parent retain];
	*slot = td;
	return obj;
}

static id
deviceNewTexture(__unused id self, __unused SEL _cmd, id desc)
{
	if (desc == nil || gGPU == NULL) return nil;
	unsigned long type = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("textureType"));
	unsigned long format = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("pixelFormat"));
	unsigned long width = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("width"));
	unsigned long height = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("height"));
	unsigned long depth = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("depth"));
	unsigned long levels = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("mipmapLevelCount"));
	unsigned long samples = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("sampleCount"));
	unsigned long usage = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("usage"));
	unsigned long arrayLength = 1;
	if ([desc respondsToSelector:sel_registerName("arrayLength")]) {
		arrayLength = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("arrayLength"));
	}

	hakios_metal_texture *t = NULL;
	if (samples > 1) {
		t = hakios_metal_texture_new_multisample(gGPU, (unsigned)format, (unsigned)width, (unsigned)height, (unsigned)samples, (unsigned)usage);
	} else if (type == 3 || type == 6 || type == 1) {
		unsigned layers = (unsigned)(arrayLength > 1 ? arrayLength : (depth > 1 ? depth : 1));
		if (levels > 1) {
			t = hakios_metal_texture_new_layered(gGPU, (unsigned)type, (unsigned)format, (unsigned)width, (unsigned)height, layers, (unsigned)levels, (unsigned)usage);
		} else {
			t = hakios_metal_texture_new_layers(gGPU, (unsigned)type, (unsigned)format, (unsigned)width, (unsigned)height, layers, (unsigned)usage);
		}
	} else if (type == 5) {
		t = hakios_metal_texture_new_cube(gGPU, (unsigned)format, (unsigned)width, (unsigned)usage);
	} else if (type == 7) {
		t = hakios_metal_texture_new_3d(gGPU, (unsigned)format, (unsigned)width, (unsigned)height, (unsigned)(depth ? depth : 1), (unsigned)usage);
	} else if (levels > 1) {
		t = hakios_metal_texture_new_levels(gGPU, (unsigned)type, (unsigned)format, (unsigned)width, (unsigned)height, (unsigned)levels, (unsigned)usage);
	} else {
		t = hakios_metal_texture_new(gGPU, (unsigned)type, (unsigned)format, (unsigned)width, (unsigned)height, (unsigned)usage);
	}
	if (t == NULL) {
		say([NSString stringWithFormat:@"newTextureWithDescriptor: failed (%s)", hakios_metal_error()]);
		return nil;
	}
	return textureWrap(t, nil, nil, nil);
}

static id
deviceNewTextureIOSurface(__unused id self, __unused SEL _cmd, id desc, id iosurface, unsigned long plane)
{
	if (desc == nil || iosurface == nil || gGPU == NULL) return nil;
	unsigned long format = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("pixelFormat"));
	unsigned long width = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("width"));
	unsigned long height = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("height"));
	unsigned long usage = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("usage"));

	uint32_t surfaceId = 0;
	if ([iosurface respondsToSelector:sel_registerName("surfaceID")]) {
		surfaceId = ((uint32_t (*)(id, SEL))objc_msgSend)(iosurface, sel_registerName("surfaceID"));
	} else {
		surfaceId = IOSurfaceGetID((IOSurfaceRef)iosurface);
	}

	hakios_metal_texture *t = hakios_metal_texture_new_iosurface(gGPU, surfaceId, (unsigned)plane, (unsigned)format, (unsigned)width, (unsigned)height, (unsigned)usage);
	if (t == NULL) {
		say([NSString stringWithFormat:@"newTextureWithDescriptor:iosurface: failed (%s)", hakios_metal_error()]);
		return nil;
	}
	return textureWrap(t, nil, iosurface, nil);
}

static id
bufferNewTexture(id self, __unused SEL _cmd, id desc, unsigned long offset, unsigned long bytesPerRow)
{
	if (desc == nil) return nil;
	hakios_metal_buffer *b = gpuBufferOf(self);
	if (b == NULL) return nil;
	unsigned long type = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("textureType"));
	unsigned long format = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("pixelFormat"));
	unsigned long width = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("width"));
	unsigned long height = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("height"));
	unsigned long usage = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("usage"));

	hakios_metal_texture *t = hakios_metal_texture_new_buffer(b, (unsigned)type, (unsigned)format, (unsigned)width, (unsigned)height, (unsigned)usage, offset, bytesPerRow);
	if (t == NULL) {
		say([NSString stringWithFormat:@"newTextureWithDescriptor:offset:bytesPerRow: failed (%s)", hakios_metal_error()]);
		return nil;
	}
	return textureWrap(t, self, nil, nil);
}

static id
textureNewView(id self, __unused SEL _cmd, unsigned long pixelFormat, unsigned long type, NSRange levels, NSRange slices)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	if (t == NULL) return nil;
	hakios_metal_texture *view = hakios_metal_texture_new_view(t, (unsigned)pixelFormat, (unsigned)type,
	                                                          (unsigned)levels.location, (unsigned)levels.length,
	                                                          (unsigned)slices.location, (unsigned)slices.length);
	if (view == NULL) {
		say([NSString stringWithFormat:@"newTextureViewWithPixelFormat:... failed (%s)", hakios_metal_error()]);
		return nil;
	}
	return textureWrap(view, nil, nil, self);
}

static id
textureNewViewFormat(id self, SEL _cmd, unsigned long pixelFormat)
{
	hakios_metal_texture *t = gpuTextureOf(self);
	if (t == NULL) return nil;
	unsigned type = hakios_metal_texture_type(t);
	unsigned levels = hakios_metal_texture_levels(t);
	unsigned depth = hakios_metal_texture_depth(t);
	return textureNewView(self, _cmd, pixelFormat, type, NSMakeRange(0, levels), NSMakeRange(0, depth));
}

struct sampler_data {
	hakios_metal_sampler *sampler;
	id label;
};

static void
samplerFree(void *state)
{
	struct sampler_data *s = (struct sampler_data *)state;
	if (s->sampler != NULL) {
		hakios_metal_sampler_free(s->sampler);
	}
	[s->label release];
	free(s);
}

static void
samplerDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gSamplerClass, samplerFree);
}

static hakios_metal_sampler *
gpuSamplerOf(id sampler)
{
	if (sampler == nil) return NULL;
	void **slot = stateOf(sampler);
	if (slot == NULL || *slot == NULL) return NULL;
	struct sampler_data *s = (struct sampler_data *)*slot;
	return s->sampler;
}

static uint64_t
samplerGpuResourceID(id self, __unused SEL _cmd)
{
	hakios_metal_sampler *s = gpuSamplerOf(self);
	return s ? hakios_metal_sampler_resource_id(s) : 0;
}

static id
deviceNewSamplerState(__unused id self, __unused SEL _cmd, id desc)
{
	if (desc == nil || gGPU == NULL) return nil;
	struct hakios_metal_sampler_desc sd;
	memset(&sd, 0, sizeof(sd));

	sd.min_filter = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("minFilter"));
	sd.mag_filter = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("magFilter"));
	sd.mip_filter = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("mipFilter"));
	sd.s_address = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("sAddressMode"));
	sd.t_address = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("tAddressMode"));
	sd.r_address = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("rAddressMode"));
	sd.normalized_coordinates = ((BOOL (*)(id, SEL))objc_msgSend)(desc, sel_registerName("normalizedCoordinates"));
	sd.lod_min_clamp = ((float (*)(id, SEL))objc_msgSend)(desc, sel_registerName("lodMinClamp"));
	sd.lod_max_clamp = ((float (*)(id, SEL))objc_msgSend)(desc, sel_registerName("lodMaxClamp"));
	sd.max_anisotropy = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("maxAnisotropy"));
	sd.compare_function = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("compareFunction"));
	sd.border_color = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("borderColor"));

	hakios_metal_sampler *s = hakios_metal_sampler_new(gGPU, &sd);
	if (s == NULL) {
		say([NSString stringWithFormat:@"newSamplerStateWithDescriptor: failed (%s)", hakios_metal_error()]);
		return nil;
	}
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id obj = plain(plain((id)gSamplerClass, sel_registerName("alloc")), sel_registerName("init"));
	if (obj == nil) {
		hakios_metal_sampler_free(s);
		return nil;
	}
	void **slot = stateOf(obj);
	struct sampler_data *sd_alloc = calloc(1, sizeof(*sd_alloc));
	if (sd_alloc == NULL || slot == NULL) {
		free(sd_alloc);
		[obj release];
		hakios_metal_sampler_free(s);
		return nil;
	}
	sd_alloc->sampler = s;
	*slot = sd_alloc;
	return obj;
}

struct render_pipeline {
	hakios_metal_program *program;
	id vertexFunction;
	id fragmentFunction;
	id label;
};

static void
renderPipelineFree(void *state)
{
	struct render_pipeline *rp = (struct render_pipeline *)state;
	if (rp->program != NULL) {
		hakios_metal_program_free(rp->program);
	}
	[rp->vertexFunction release];
	[rp->fragmentFunction release];
	[rp->label release];
	free(rp);
}

static void
renderPipelineDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gRenderPipelineClass, renderPipelineFree);
}

static hakios_metal_program *
gpuProgramOf(id obj)
{
	if (obj == nil) return NULL;
	void **slot = stateOf(obj);
	if (slot == NULL || *slot == NULL) return NULL;
	struct render_pipeline *rp = (struct render_pipeline *)*slot;
	return rp->program;
}

static unsigned long
renderPipelineMaxThreads(__unused id self, __unused SEL _cmd)
{
	return 1024;
}

static id
deviceNewRenderPipeline(__unused id self, __unused SEL _cmd, id desc, NSError **error)
{
	if (desc == nil || gGPU == NULL) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal userInfo:@{NSLocalizedDescriptionKey: @"nil descriptor or no GPU"}];
		}
		return nil;
	}
	id vs = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("vertexFunction"));
	id fs = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("fragmentFunction"));
	if (vs == nil) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal userInfo:@{NSLocalizedDescriptionKey: @"vertexFunction is required"}];
		}
		return nil;
	}
	NSData *vsBitcode = nil, *fsBitcode = nil;
	if ([vs respondsToSelector:sel_registerName("hakiosBitcode")]) {
		vsBitcode = ((id (*)(id, SEL))objc_msgSend)(vs, sel_registerName("hakiosBitcode"));
	} else if ([vs respondsToSelector:sel_registerName("bitcodeData")]) {
		vsBitcode = ((id (*)(id, SEL))objc_msgSend)(vs, sel_registerName("bitcodeData"));
	}
	if (fs != nil) {
		if ([fs respondsToSelector:sel_registerName("hakiosBitcode")]) {
			fsBitcode = ((id (*)(id, SEL))objc_msgSend)(fs, sel_registerName("hakiosBitcode"));
		} else if ([fs respondsToSelector:sel_registerName("bitcodeData")]) {
			fsBitcode = ((id (*)(id, SEL))objc_msgSend)(fs, sel_registerName("bitcodeData"));
		}
	}
	if (vsBitcode == nil || [vsBitcode length] == 0) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal userInfo:@{NSLocalizedDescriptionKey: @"vertex function has no bitcode"}];
		}
		return nil;
	}

	unsigned colorPixelFormat = 70; /* RGBA8Unorm default */
	id colorAttachments = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("colorAttachments"));
	id ca0 = colorAttachments != nil ? ((id (*)(id, SEL, unsigned long))objc_msgSend)(colorAttachments, sel_registerName("objectAtIndexedSubscript:"), 0) : nil;
	if (ca0 != nil) {
		unsigned long fmt = ((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("pixelFormat"));
		if (fmt != 0) colorPixelFormat = (unsigned)fmt;
	}

	hakios_metal_program *prog = hakios_metal_program_new_bitcode([vsBitcode bytes], [vsBitcode length],
	                                                              fsBitcode ? [fsBitcode bytes] : NULL,
	                                                              fsBitcode ? [fsBitcode length] : 0,
	                                                              colorPixelFormat);
	if (prog == NULL) {
		const char *err = hakios_metal_error();
		say([NSString stringWithFormat:@"newRenderPipelineState: compile failed (%s)", err ? err : "unknown"]);
		if (error != NULL) {
			*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:err ? err : "compile failed"]}];
		}
		return nil;
	}

	id vdesc = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("vertexDescriptor"));
	if (vdesc != nil) {
		id attrs = ((id (*)(id, SEL))objc_msgSend)(vdesc, sel_registerName("attributes"));
		id layouts = ((id (*)(id, SEL))objc_msgSend)(vdesc, sel_registerName("layouts"));
		if (attrs != nil) {
			for (unsigned i = 0; i < 31; i++) {
				id a = ((id (*)(id, SEL, unsigned long))objc_msgSend)(attrs, sel_registerName("objectAtIndexedSubscript:"), i);
				if (a != nil) {
					unsigned long format = ((unsigned long (*)(id, SEL))objc_msgSend)(a, sel_registerName("format"));
					if (format != 0) {
						unsigned long offset = ((unsigned long (*)(id, SEL))objc_msgSend)(a, sel_registerName("offset"));
						unsigned long bufIdx = ((unsigned long (*)(id, SEL))objc_msgSend)(a, sel_registerName("bufferIndex"));
						hakios_metal_program_set_vertex_attribute(prog, i, (unsigned)format, (unsigned)offset, (unsigned)bufIdx);
					}
				}
			}
		}
		if (layouts != nil) {
			for (unsigned i = 0; i < 31; i++) {
				id l = ((id (*)(id, SEL, unsigned long))objc_msgSend)(layouts, sel_registerName("objectAtIndexedSubscript:"), i);
				if (l != nil) {
					unsigned long stride = ((unsigned long (*)(id, SEL))objc_msgSend)(l, sel_registerName("stride"));
					if (stride != 0) {
						unsigned long stepFunc = ((unsigned long (*)(id, SEL))objc_msgSend)(l, sel_registerName("stepFunction"));
						unsigned long stepRate = ((unsigned long (*)(id, SEL))objc_msgSend)(l, sel_registerName("stepRate"));
						hakios_metal_program_set_vertex_layout(prog, i, (unsigned)stride, (unsigned)stepFunc, (unsigned)stepRate);
					}
				}
			}
		}
	}

	if (ca0 != nil) {
		BOOL blendEnabled = ((BOOL (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("isBlendingEnabled"));
		unsigned rgbOp = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("rgbBlendOperation"));
		unsigned aOp = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("alphaBlendOperation"));
		unsigned sRgb = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("sourceRGBBlendFactor"));
		unsigned dRgb = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("destinationRGBBlendFactor"));
		unsigned sA = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("sourceAlphaBlendFactor"));
		unsigned dA = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("destinationAlphaBlendFactor"));
		unsigned mask = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("writeMask"));
		if (mask == 0) mask = 0xf;
		hakios_metal_program_set_blend(prog, blendEnabled ? 1 : 0, rgbOp, aOp, sRgb, dRgb, sA, dA, mask);
	}

	unsigned long samples = ((unsigned long (*)(id, SEL))objc_msgSend)(desc, sel_registerName("rasterSampleCount"));
	if (samples > 1) {
		hakios_metal_program_set_samples(prog, (unsigned)samples);
	}

	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id obj = plain(plain((id)gRenderPipelineClass, sel_registerName("alloc")), sel_registerName("init"));
	if (obj == nil) {
		hakios_metal_program_free(prog);
		return nil;
	}
	void **slot = stateOf(obj);
	struct render_pipeline *rp = calloc(1, sizeof(*rp));
	if (rp == NULL || slot == NULL) {
		free(rp);
		[obj release];
		hakios_metal_program_free(prog);
		return nil;
	}
	rp->program = prog;
	rp->vertexFunction = [vs retain];
	rp->fragmentFunction = [fs retain];
	*slot = rp;
	say([NSString stringWithFormat:@"newRenderPipelineStateWithDescriptor: compiled (vs: %@, fs: %@, format: %u)",
	     [vs name], fs ? [fs name] : @"(none)", colorPixelFormat]);
	return obj;
}

static void
deviceNewRenderPipelineAsync(id self, SEL _cmd, id desc, void (^completion)(id, NSError *))
{
	NSError *err = nil;
	id pipe = deviceNewRenderPipeline(self, _cmd, desc, &err);
	if (completion != nil) {
		completion(pipe, err);
	}
}

struct render_encoder {
	id commandBuffer;
	id pipeline;
	hakios_metal_program *program;
	struct hakios_metal_render_pass pass;
	struct hakios_metal_viewport viewport;
	struct hakios_metal_scissor scissor;
	BOOL hasViewport;
	BOOL hasScissor;
	unsigned drawsCount;
	hakios_metal_buffer *vsBuffers[31];
	uint64_t vsOffsets[31];
	unsigned maxVsBuffer;
	hakios_metal_buffer *fsBuffers[31];
	uint64_t fsOffsets[31];
	unsigned maxFsBuffer;
	hakios_metal_texture *vsTextures[31];
	unsigned maxVsTexture;
	hakios_metal_sampler *vsSamplers[16];
	unsigned maxVsSampler;
	hakios_metal_texture *fsTextures[31];
	unsigned maxFsTexture;
	hakios_metal_sampler *fsSamplers[16];
	unsigned maxFsSampler;
	BOOL ended;
	id label;
};

static void
renderEncoderFree(void *state)
{
	struct render_encoder *e = (struct render_encoder *)state;
	[e->commandBuffer release];
	[e->pipeline release];
	[e->label release];
	free(e);
}

static void
renderEncoderDealloc(id self, SEL _cmd)
{
	deallocWith(self, _cmd, gRenderEncoderClass, renderEncoderFree);
}

static void
renderSetPipeline(id self, __unused SEL _cmd, id pipeline)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended) return;
	[e->pipeline release];
	e->pipeline = [pipeline retain];
	e->program = gpuProgramOf(pipeline);
}

static void
renderSetViewport(id self, __unused SEL _cmd, MTLViewport vp)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended) return;
	e->viewport.x = vp.originX;
	e->viewport.y = vp.originY;
	e->viewport.width = vp.width;
	e->viewport.height = vp.height;
	e->viewport.znear = vp.znear;
	e->viewport.zfar = vp.zfar;
	e->hasViewport = YES;
}

static void
renderSetScissor(id self, __unused SEL _cmd, MTLScissorRect sc)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended) return;
	e->scissor.x = (unsigned)sc.x;
	e->scissor.y = (unsigned)sc.y;
	e->scissor.width = (unsigned)sc.width;
	e->scissor.height = (unsigned)sc.height;
	e->hasScissor = YES;
}

static void
renderSetVertexBuffer(id self, __unused SEL _cmd, id buffer, unsigned long offset, unsigned long index)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended || index >= 31) return;
	e->vsBuffers[index] = gpuBufferOf(buffer);
	e->vsOffsets[index] = offset;
	if (index + 1 > e->maxVsBuffer) e->maxVsBuffer = (unsigned)(index + 1);
}

static void
renderSetVertexBuffers(id self, SEL _cmd, const id *buffers, const unsigned long *offsets, NSRange range)
{
	for (NSUInteger k = 0; k < range.length; k++) {
		renderSetVertexBuffer(self, _cmd, buffers[k], offsets[k], range.location + k);
	}
}

static void
renderSetFragmentBuffer(id self, __unused SEL _cmd, id buffer, unsigned long offset, unsigned long index)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended || index >= 31) return;
	e->fsBuffers[index] = gpuBufferOf(buffer);
	e->fsOffsets[index] = offset;
	if (index + 1 > e->maxFsBuffer) e->maxFsBuffer = (unsigned)(index + 1);
}

static void
renderSetFragmentBuffers(id self, SEL _cmd, const id *buffers, const unsigned long *offsets, NSRange range)
{
	for (NSUInteger k = 0; k < range.length; k++) {
		renderSetFragmentBuffer(self, _cmd, buffers[k], offsets[k], range.location + k);
	}
}

static void
renderSetVertexTexture(id self, __unused SEL _cmd, id texture, unsigned long index)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended || index >= 31) return;
	e->vsTextures[index] = gpuTextureOf(texture);
	if (index + 1 > e->maxVsTexture) e->maxVsTexture = (unsigned)(index + 1);
}

static void
renderSetVertexTextures(id self, SEL _cmd, const id *textures, NSRange range)
{
	for (NSUInteger k = 0; k < range.length; k++) {
		renderSetVertexTexture(self, _cmd, textures[k], range.location + k);
	}
}

static void
renderSetFragmentTexture(id self, __unused SEL _cmd, id texture, unsigned long index)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended || index >= 31) return;
	e->fsTextures[index] = gpuTextureOf(texture);
	if (index + 1 > e->maxFsTexture) e->maxFsTexture = (unsigned)(index + 1);
}

static void
renderSetFragmentTextures(id self, SEL _cmd, const id *textures, NSRange range)
{
	for (NSUInteger k = 0; k < range.length; k++) {
		renderSetFragmentTexture(self, _cmd, textures[k], range.location + k);
	}
}

static void
renderSetVertexSampler(id self, __unused SEL _cmd, id sampler, unsigned long index)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended || index >= 16) return;
	e->vsSamplers[index] = gpuSamplerOf(sampler);
	if (index + 1 > e->maxVsSampler) e->maxVsSampler = (unsigned)(index + 1);
}

static void
renderSetFragmentSampler(id self, __unused SEL _cmd, id sampler, unsigned long index)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended || index >= 16) return;
	e->fsSamplers[index] = gpuSamplerOf(sampler);
	if (index + 1 > e->maxFsSampler) e->maxFsSampler = (unsigned)(index + 1);
}

static void
renderSetCullMode(id self, __unused SEL _cmd, unsigned long cullMode)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended) return;
	e->pass.cull_mode = (unsigned)cullMode;
}

static void
renderSetWinding(id self, __unused SEL _cmd, unsigned long winding)
{
	struct render_encoder *e = slotState(self);
	if (e == NULL || e->ended) return;
	e->pass.winding = (unsigned)winding;
}

static void
renderDrawPrimitives(id self, __unused SEL _cmd, unsigned long primitive, unsigned long vertexStart, unsigned long vertexCount, unsigned long instanceCount)
{
	struct render_encoder *e = slotState(self);
	struct commandbuffer *c = e != NULL && !e->ended ? slotState(e->commandBuffer) : NULL;

	if (c == NULL || e->program == NULL) {
		say(@"drawPrimitives: no command buffer or no render pipeline set");
		return;
	}
	hakios_metal_cb_set_vertex_textures(c->cb, e->vsTextures, e->maxVsTexture, e->vsSamplers, e->maxVsSampler);
	int rc = hakios_metal_cb_draw2(c->cb, e->program, &e->pass,
	                               e->hasViewport ? &e->viewport : NULL,
	                               e->hasScissor ? &e->scissor : NULL,
	                               (unsigned)primitive, (unsigned)vertexStart, (unsigned)vertexCount,
	                               (unsigned)(instanceCount > 0 ? instanceCount : 1),
	                               e->vsBuffers, e->vsOffsets, e->maxVsBuffer,
	                               e->fsBuffers, e->fsOffsets, e->maxFsBuffer,
	                               e->fsTextures, e->maxFsTexture,
	                               e->fsSamplers, e->maxFsSampler);
	if (rc != 0) {
		say([NSString stringWithFormat:@"drawPrimitives: failed (%s)", hakios_metal_error()]);
	}
	e->drawsCount++;
	if (e->pass.load_action == 2 /* Clear */) {
		e->pass.load_action = 1;
	}
}

static void
renderDrawPrimitivesSimple(id self, SEL _cmd, unsigned long primitive, unsigned long vertexStart, unsigned long vertexCount)
{
	renderDrawPrimitives(self, _cmd, primitive, vertexStart, vertexCount, 1);
}

static void
renderDrawIndexed(id self, __unused SEL _cmd, unsigned long primitive, unsigned long indexCount, unsigned long indexType,
                  id indexBuffer, unsigned long indexOffset, unsigned long instanceCount)
{
	struct render_encoder *e = slotState(self);
	struct commandbuffer *c = e != NULL && !e->ended ? slotState(e->commandBuffer) : NULL;
	hakios_metal_buffer *ib = gpuBufferOf(indexBuffer);

	if (c == NULL || e->program == NULL || ib == NULL) {
		say(@"drawIndexedPrimitives: no command buffer, pipeline or index buffer");
		return;
	}
	hakios_metal_cb_set_vertex_textures(c->cb, e->vsTextures, e->maxVsTexture, e->vsSamplers, e->maxVsSampler);
	int rc = hakios_metal_cb_draw_indexed(c->cb, e->program, &e->pass,
	                                      e->hasViewport ? &e->viewport : NULL,
	                                      e->hasScissor ? &e->scissor : NULL,
	                                      (unsigned)primitive, (unsigned)indexCount, (unsigned)indexType,
	                                      ib, indexOffset, (unsigned)(instanceCount > 0 ? instanceCount : 1),
	                                      0, 0,
	                                      e->vsBuffers, e->vsOffsets, e->maxVsBuffer,
	                                      e->fsBuffers, e->fsOffsets, e->maxFsBuffer,
	                                      e->fsTextures, e->maxFsTexture,
	                                      e->fsSamplers, e->maxFsSampler);
	if (rc != 0) {
		say([NSString stringWithFormat:@"drawIndexedPrimitives: failed (%s)", hakios_metal_error()]);
	}
	e->drawsCount++;
	if (e->pass.load_action == 2 /* Clear */) {
		e->pass.load_action = 1;
	}
}

static void
renderDrawIndexedSimple(id self, SEL _cmd, unsigned long primitive, unsigned long indexCount, unsigned long indexType,
                        id indexBuffer, unsigned long indexOffset)
{
	renderDrawIndexed(self, _cmd, primitive, indexCount, indexType, indexBuffer, indexOffset, 1);
}

static void
renderEnd(id self, __unused SEL _cmd)
{
	struct render_encoder *e = slotState(self);
	struct commandbuffer *c = e != NULL && !e->ended ? slotState(e->commandBuffer) : NULL;

	if (e != NULL && !e->ended) {
		if (e->drawsCount == 0 && c != NULL && e->pass.load_action == 2 /* Clear */) {
			if (hakios_metal_cb_clear(c->cb, &e->pass) != 0) {
				say([NSString stringWithFormat:@"renderEnd: cb_clear failed: %s", hakios_metal_error()]);
			}
		}
		e->ended = YES;
	}
}

static id
commandBufferRenderEncoder(id self, __unused SEL _cmd, id desc)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	struct commandbuffer *c = slotState(self);
	struct render_encoder *e;
	void **slot;
	id encoder;

	if (c == NULL || c->committed || gRenderEncoderClass == Nil || desc == nil ||
	    (encoder = plain(plain((id)gRenderEncoderClass, sel_registerName("alloc")), sel_registerName("init"))) == nil) {
		return nil;
	}
	e = calloc(1, sizeof(*e));
	slot = stateOf(encoder);
	if (e == NULL || slot == NULL) {
		free(e);
		[encoder release];
		return nil;
	}
	e->commandBuffer = [self retain];

	id colorAttachments = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("colorAttachments"));
	if (colorAttachments != nil) {
		id ca0 = ((id (*)(id, SEL, unsigned long))objc_msgSend)(colorAttachments, sel_registerName("objectAtIndexedSubscript:"), 0);
		if (ca0 != nil) {
			id tex = ((id (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("texture"));
			if (tex != nil) {
				e->pass.color0 = gpuTextureOf(tex);
				e->pass.load_action = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("loadAction"));
				e->pass.store_action = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("storeAction"));
				e->pass.color0_level = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("level"));
				e->pass.color0_slice = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("slice"));
				MTLClearColor cc = ((MTLClearColor (*)(id, SEL))objc_msgSend)(ca0, sel_registerName("clearColor"));
				e->pass.clear_color[0] = cc.red;
				e->pass.clear_color[1] = cc.green;
				e->pass.clear_color[2] = cc.blue;
				e->pass.clear_color[3] = cc.alpha;
			}
		}
		for (unsigned i = 1; i <= 7; i++) {
			id ca = ((id (*)(id, SEL, unsigned long))objc_msgSend)(colorAttachments, sel_registerName("objectAtIndexedSubscript:"), i);
			if (ca != nil) {
				id mtex = ((id (*)(id, SEL))objc_msgSend)(ca, sel_registerName("texture"));
				if (mtex != nil) {
					e->pass.more[i - 1].texture = gpuTextureOf(mtex);
					e->pass.more[i - 1].load_action = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca, sel_registerName("loadAction"));
					e->pass.more[i - 1].store_action = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca, sel_registerName("storeAction"));
					e->pass.more[i - 1].level = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca, sel_registerName("level"));
					e->pass.more[i - 1].slice = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(ca, sel_registerName("slice"));
					MTLClearColor mcc = ((MTLClearColor (*)(id, SEL))objc_msgSend)(ca, sel_registerName("clearColor"));
					e->pass.more[i - 1].clear_color[0] = mcc.red;
					e->pass.more[i - 1].clear_color[1] = mcc.green;
					e->pass.more[i - 1].clear_color[2] = mcc.blue;
					e->pass.more[i - 1].clear_color[3] = mcc.alpha;
				}
			}
		}
	}

	id depthAttachment = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("depthAttachment"));
	if (depthAttachment != nil) {
		id dtex = ((id (*)(id, SEL))objc_msgSend)(depthAttachment, sel_registerName("texture"));
		if (dtex != nil) {
			e->pass.depth = gpuTextureOf(dtex);
			e->pass.depth_load_action = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(depthAttachment, sel_registerName("loadAction"));
			e->pass.depth_store_action = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(depthAttachment, sel_registerName("storeAction"));
			e->pass.clear_depth = ((double (*)(id, SEL))objc_msgSend)(depthAttachment, sel_registerName("clearDepth"));
			e->pass.depth_level = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(depthAttachment, sel_registerName("level"));
			e->pass.depth_slice = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(depthAttachment, sel_registerName("slice"));
		}
	}

	id stencilAttachment = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("stencilAttachment"));
	if (stencilAttachment != nil) {
		id stex = ((id (*)(id, SEL))objc_msgSend)(stencilAttachment, sel_registerName("texture"));
		if (stex != nil) {
			e->pass.stencil = gpuTextureOf(stex);
			e->pass.stencil_load_action = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(stencilAttachment, sel_registerName("loadAction"));
			e->pass.stencil_store_action = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(stencilAttachment, sel_registerName("storeAction"));
			e->pass.clear_stencil = (unsigned)((unsigned long (*)(id, SEL))objc_msgSend)(stencilAttachment, sel_registerName("clearStencil"));
		}
	}

	*slot = e;
	return [encoder autorelease];
}

static void
registerTextureAndSamplerClasses(Class device)
{
	static const char *const tex[] = { "MTLTexture", "MTLResource", NULL };
	static const char *const samp[] = { "MTLSamplerState", NULL };
	static const char *const renderPipe[] = { "MTLRenderPipelineState", NULL };

	if ((gTextureClass = objectClass("HakiosMetalTexture", "_MTLTexture", tex, (IMP)textureDealloc)) != Nil) {
		class_addMethod(gTextureClass, sel_registerName("width"), (IMP)textureWidth, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("height"), (IMP)textureHeight, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("depth"), (IMP)textureDepth, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("mipmapLevelCount"), (IMP)textureMipmapLevels, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("textureType"), (IMP)textureType, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("pixelFormat"), (IMP)texturePixelFormat, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("usage"), (IMP)textureUsage, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("sampleCount"), (IMP)textureSamples, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("arrayLength"), (IMP)textureArrayLength, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("allocatedSize"), (IMP)textureAllocatedSize, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("gpuAddress"), (IMP)textureGpuAddress, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("bufferBytesPerRow"), (IMP)textureBytesPerRow, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("buffer"), (IMP)textureBuffer, "@@:");
		class_addMethod(gTextureClass, sel_registerName("bufferOffset"), (IMP)textureBufferOffset, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("iosurface"), (IMP)textureIOSurface, "@@:");
		class_addMethod(gTextureClass, sel_registerName("iosurfacePlane"), (IMP)textureIOSurfacePlane, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("gpuResourceID"), (IMP)textureGpuResourceID, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"), (IMP)textureReplaceRegion, "v@:{?=QQQQQQ}Qr^vQ");
		class_addMethod(gTextureClass, sel_registerName("replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:"), (IMP)textureReplaceRegion3D, "v@:{?=QQQQQQ}QQr^vQQ");
		class_addMethod(gTextureClass, sel_registerName("getBytes:bytesPerRow:fromRegion:mipmapLevel:"), (IMP)textureGetBytes, "v@:^vQ{?=QQQQQQ}Q");
		class_addMethod(gTextureClass, sel_registerName("getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:"), (IMP)textureGetBytes3D, "v@:^vQQ{?=QQQQQQ}QQ");
		class_addMethod(gTextureClass, sel_registerName("newTextureViewWithPixelFormat:textureType:levels:slices:"), (IMP)textureNewView, "@@:QQ{_NSRange=QQ}{_NSRange=QQ}");
		class_addMethod(gTextureClass, sel_registerName("newTextureViewWithPixelFormat:"), (IMP)textureNewViewFormat, "@@:Q");
		objc_registerClassPair(gTextureClass);
		class_addMethod(device, sel_registerName("newTextureWithDescriptor:"), (IMP)deviceNewTexture, "@@:@");
		class_addMethod(device, sel_registerName("newTextureWithDescriptor:iosurface:plane:"), (IMP)deviceNewTextureIOSurface, "@@:@@Q");
	}

	if ((gSamplerClass = objectClass("HakiosMetalSamplerState", "_MTLSamplerState", samp, (IMP)samplerDealloc)) != Nil) {
		class_addMethod(gSamplerClass, sel_registerName("gpuResourceID"), (IMP)samplerGpuResourceID, "Q@:");
		objc_registerClassPair(gSamplerClass);
		class_addMethod(device, sel_registerName("newSamplerStateWithDescriptor:"), (IMP)deviceNewSamplerState, "@@:@");
	}

	if ((gRenderPipelineClass = objectClass("HakiosMetalRenderPipelineState", "_MTLRenderPipelineState", renderPipe, (IMP)renderPipelineDealloc)) != Nil) {
		class_addMethod(gRenderPipelineClass, sel_registerName("maxTotalThreadsPerThreadgroup"), (IMP)renderPipelineMaxThreads, "Q@:");
		objc_registerClassPair(gRenderPipelineClass);
		class_addMethod(device, sel_registerName("newRenderPipelineStateWithDescriptor:error:"), (IMP)deviceNewRenderPipeline, "@@:@^@");
		class_addMethod(device, sel_registerName("newRenderPipelineStateWithDescriptor:completionHandler:"), (IMP)deviceNewRenderPipelineAsync, "v@:@@?");
	}
}

static id
commandBufferComputeEncoder(id self, __unused SEL _cmd)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	struct commandbuffer *c = slotState(self);
	struct compute_encoder *e;
	void **slot;
	id encoder;

	if (c == NULL || c->committed || gComputeEncoderClass == Nil ||
	    (encoder = plain(plain((id)gComputeEncoderClass, sel_registerName("alloc")), sel_registerName("init"))) == nil) {
		return nil;
	}
	e = calloc(1, sizeof(*e));
	slot = stateOf(encoder);
	if (e == NULL || slot == NULL) {
		free(e);
		[encoder release];
		return nil;
	}
	e->commandBuffer = [self retain];
	*slot = e;
	return [encoder autorelease];
}

static id
deviceNewComputePipeline(__unused id self, __unused SEL _cmd, id function, NSError **error)
{
	if (function == nil) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal
			    userInfo:@{ NSLocalizedDescriptionKey: @"nil function" }];
		}
		return nil;
	}
	NSData *bitcode = nil;
	if ([function respondsToSelector:sel_registerName("hakiosBitcode")]) {
		bitcode = ((id (*)(id, SEL))objc_msgSend)(function, sel_registerName("hakiosBitcode"));
	} else if ([function respondsToSelector:sel_registerName("bitcodeData")]) {
		bitcode = ((id (*)(id, SEL))objc_msgSend)(function, sel_registerName("bitcodeData"));
	}
	if (bitcode == nil || [bitcode length] == 0) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal
			    userInfo:@{ NSLocalizedDescriptionKey: @"function has no bitcode" }];
		}
		return nil;
	}
	hakios_metal_kernel *k = hakios_metal_kernel_compile_bitcode([bitcode bytes], [bitcode length]);
	if (k == NULL) {
		const char *err = hakios_metal_error();
		say([NSString stringWithFormat:@"newComputePipelineState: compile failed: %s", err ? err : "unknown"]);
		if (error != NULL) {
			*error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal
			    userInfo:@{ NSLocalizedDescriptionKey: [NSString stringWithUTF8String:err ? err : "compile failed"] }];
		}
		return nil;
	}
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id pipe = plain(plain((id)gComputePipelineClass, sel_registerName("alloc")), sel_registerName("init"));
	void **slot = stateOf(pipe);
	struct compute_pipeline *cp = calloc(1, sizeof(*cp));
	if (pipe == nil || slot == NULL || cp == NULL) {
		free(cp);
		[pipe release];
		hakios_metal_kernel_free(k);
		return nil;
	}
	cp->kernel = k;
	cp->function = [function retain];
	*slot = cp;
	say([NSString stringWithFormat:@"newComputePipelineStateWithFunction:%@: compiled (%u max threads, execution width %u)",
	    [function name], hakios_metal_kernel_max_threads(k), hakios_metal_kernel_execution_width(k)]);
	return pipe;
}

static void
deviceNewComputePipelineAsync(id self, SEL _cmd, id function, void (^completion)(id, NSError *))
{
	NSError *err = nil;
	id pipe = deviceNewComputePipeline(self, _cmd, function, &err);
	if (completion != nil) {
		completion(pipe, err);
	}
}

static void
registerCommandClasses(void)
{
	static const char *const cb[] = { "MTLCommandBuffer", NULL }, *const blit[] = { "MTLBlitCommandEncoder", "MTLCommandEncoder", NULL },
	    *const computeEnc[] = { "MTLComputeCommandEncoder", "MTLCommandEncoder", NULL },
	    *const computePipe[] = { "MTLComputePipelineState", NULL };

	if ((gComputePipelineClass = objectClass("HakiosMetalComputePipelineState", "_MTLComputePipelineState", computePipe, (IMP)computePipelineDealloc)) != Nil) {
		class_addMethod(gComputePipelineClass, sel_registerName("maxTotalThreadsPerThreadgroup"), (IMP)computePipelineMaxThreads, "Q@:");
		class_addMethod(gComputePipelineClass, sel_registerName("threadExecutionWidth"), (IMP)computePipelineExecutionWidth, "Q@:");
		class_addMethod(gComputePipelineClass, sel_registerName("staticThreadgroupMemoryLength"), (IMP)computePipelineThreadgroupMemory, "Q@:");
		objc_registerClassPair(gComputePipelineClass);
	}
	if ((gCommandBufferClass = objectClass("HakiosMetalCommandBuffer", "_MTLCommandBuffer", cb, (IMP)commandBufferDealloc)) != Nil) {
		class_addMethod(gCommandBufferClass, sel_registerName("commit"), (IMP)commandBufferCommit, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("enqueue"), (IMP)commandBufferEnqueue, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("waitUntilCompleted"), (IMP)commandBufferWait, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("waitUntilScheduled"), (IMP)commandBufferWaitScheduled, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("addCompletedHandler:"), (IMP)commandBufferAddHandler, "v@:@?");
		class_addMethod(gCommandBufferClass, sel_registerName("addScheduledHandler:"), (IMP)commandBufferAddScheduled, "v@:@?");
		class_addMethod(gCommandBufferClass, sel_registerName("status"), (IMP)commandBufferStatus, "Q@:");
		class_addMethod(gCommandBufferClass, sel_registerName("error"), (IMP)commandBufferError, "@@:");
		class_addMethod(gCommandBufferClass, sel_registerName("commandQueue"), (IMP)commandBufferQueue, "@@:");
		class_addMethod(gCommandBufferClass, sel_registerName("blitCommandEncoder"), (IMP)commandBufferBlitEncoder, "@@:");
		class_addMethod(gCommandBufferClass, sel_registerName("computeCommandEncoder"), (IMP)commandBufferComputeEncoder, "@@:");
		class_addMethod(gCommandBufferClass, sel_registerName("computeCommandEncoderWithDispatchType:"), (IMP)commandBufferComputeEncoder, "@@:Q");
		class_addMethod(gCommandBufferClass, sel_registerName("renderCommandEncoderWithDescriptor:"), (IMP)commandBufferRenderEncoder, "@@:@");
		class_addMethod(gCommandBufferClass, sel_registerName("GPUStartTime"), (IMP)commandBufferNoTime, "d@:");
		class_addMethod(gCommandBufferClass, sel_registerName("GPUEndTime"), (IMP)commandBufferNoTime, "d@:");
		objc_registerClassPair(gCommandBufferClass);
	}
	if ((gBlitEncoderClass = objectClass("HakiosMetalBlitEncoder", "_MTLCommandEncoder", blit, (IMP)blitEncoderDealloc)) != Nil) {
		class_addMethod(gBlitEncoderClass, sel_registerName("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"),
		    (IMP)blitCopy, "v@:@Q@QQ");
		class_addMethod(gBlitEncoderClass, sel_registerName("fillBuffer:range:value:"), (IMP)blitFill, "v@:@{_NSRange=QQ}C");
		class_addMethod(gBlitEncoderClass, sel_registerName("endEncoding"), (IMP)blitEnd, "v@:");
		class_addMethod(gBlitEncoderClass, sel_registerName("synchronizeResource:"), (IMP)blitNothing, "v@:@");
		objc_registerClassPair(gBlitEncoderClass);
	}
	if ((gComputeEncoderClass = objectClass("HakiosMetalComputeEncoder", "_MTLCommandEncoder", computeEnc, (IMP)computeEncoderDealloc)) != Nil) {
		class_addMethod(gComputeEncoderClass, sel_registerName("setComputePipelineState:"), (IMP)computeSetPipeline, "v@:@");
		class_addMethod(gComputeEncoderClass, sel_registerName("setBuffer:offset:atIndex:"), (IMP)computeSetBuffer, "v@:@QQ");
		class_addMethod(gComputeEncoderClass, sel_registerName("setBuffers:offsets:withRange:"), (IMP)computeSetBuffers, "v@:r^@r^Q{_NSRange=QQ}");
		class_addMethod(gComputeEncoderClass, sel_registerName("setTexture:atIndex:"), (IMP)computeSetTexture, "v@:@Q");
		class_addMethod(gComputeEncoderClass, sel_registerName("setTextures:withRange:"), (IMP)computeSetTextures, "v@:r^@{_NSRange=QQ}");
		class_addMethod(gComputeEncoderClass, sel_registerName("setSamplerState:atIndex:"), (IMP)computeSetSampler, "v@:@Q");
		class_addMethod(gComputeEncoderClass, sel_registerName("setSamplerStates:withRange:"), (IMP)computeSetSamplers, "v@:r^@{_NSRange=QQ}");
		class_addMethod(gComputeEncoderClass, sel_registerName("dispatchThreadgroups:threadsPerThreadgroup:"), (IMP)computeDispatch, "v@:{?=QQQ}{?=QQQ}");
		class_addMethod(gComputeEncoderClass, sel_registerName("dispatchThreads:threadsPerThreadgroup:"), (IMP)computeDispatchThreads, "v@:{?=QQQ}{?=QQQ}");
		class_addMethod(gComputeEncoderClass, sel_registerName("endEncoding"), (IMP)computeEnd, "v@:");
		objc_registerClassPair(gComputeEncoderClass);
	}
	static const char *const renderEnc[] = { "MTLRenderCommandEncoder", "MTLCommandEncoder", NULL };
	if ((gRenderEncoderClass = objectClass("HakiosMetalRenderEncoder", "_MTLCommandEncoder", renderEnc, (IMP)renderEncoderDealloc)) != Nil) {
		class_addMethod(gRenderEncoderClass, sel_registerName("setRenderPipelineState:"), (IMP)renderSetPipeline, "v@:@");
		class_addMethod(gRenderEncoderClass, sel_registerName("setViewport:"), (IMP)renderSetViewport, "v@:{?=dddddd}");
		class_addMethod(gRenderEncoderClass, sel_registerName("setScissorRect:"), (IMP)renderSetScissor, "v@:{?=QQQQ}");
		class_addMethod(gRenderEncoderClass, sel_registerName("setVertexBuffer:offset:atIndex:"), (IMP)renderSetVertexBuffer, "v@:@QQ");
		class_addMethod(gRenderEncoderClass, sel_registerName("setVertexBuffers:offsets:withRange:"), (IMP)renderSetVertexBuffers, "v@:r^@r^Q{_NSRange=QQ}");
		class_addMethod(gRenderEncoderClass, sel_registerName("setFragmentBuffer:offset:atIndex:"), (IMP)renderSetFragmentBuffer, "v@:@QQ");
		class_addMethod(gRenderEncoderClass, sel_registerName("setFragmentBuffers:offsets:withRange:"), (IMP)renderSetFragmentBuffers, "v@:r^@r^Q{_NSRange=QQ}");
		class_addMethod(gRenderEncoderClass, sel_registerName("setVertexTexture:atIndex:"), (IMP)renderSetVertexTexture, "v@:@Q");
		class_addMethod(gRenderEncoderClass, sel_registerName("setVertexTextures:withRange:"), (IMP)renderSetVertexTextures, "v@:r^@{_NSRange=QQ}");
		class_addMethod(gRenderEncoderClass, sel_registerName("setFragmentTexture:atIndex:"), (IMP)renderSetFragmentTexture, "v@:@Q");
		class_addMethod(gRenderEncoderClass, sel_registerName("setFragmentTextures:withRange:"), (IMP)renderSetFragmentTextures, "v@:r^@{_NSRange=QQ}");
		class_addMethod(gRenderEncoderClass, sel_registerName("setVertexSamplerState:atIndex:"), (IMP)renderSetVertexSampler, "v@:@Q");
		class_addMethod(gRenderEncoderClass, sel_registerName("setFragmentSamplerState:atIndex:"), (IMP)renderSetFragmentSampler, "v@:@Q");
		class_addMethod(gRenderEncoderClass, sel_registerName("setCullMode:"), (IMP)renderSetCullMode, "v@:Q");
		class_addMethod(gRenderEncoderClass, sel_registerName("setFrontFacingWinding:"), (IMP)renderSetWinding, "v@:Q");
		class_addMethod(gRenderEncoderClass, sel_registerName("drawPrimitives:vertexStart:vertexCount:instanceCount:"), (IMP)renderDrawPrimitives, "v@:QQQQ");
		class_addMethod(gRenderEncoderClass, sel_registerName("drawPrimitives:vertexStart:vertexCount:"), (IMP)renderDrawPrimitivesSimple, "v@:QQQ");
		class_addMethod(gRenderEncoderClass, sel_registerName("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:"), (IMP)renderDrawIndexed, "v@:QQ@QQ");
		class_addMethod(gRenderEncoderClass, sel_registerName("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"), (IMP)renderDrawIndexedSimple, "v@:QQ@Q");
		class_addMethod(gRenderEncoderClass, sel_registerName("endEncoding"), (IMP)renderEnd, "v@:");
		objc_registerClassPair(gRenderEncoderClass);
	}
}
#endif

static void
registerQueueClass(void)
{
	Class base = objc_getClass("_MTLCommandQueue");
	Protocol * protocol = @protocol(MTLCommandQueue);

	if (base == Nil) {
		say(@"this system has no _MTLCommandQueue: no queue class is registered");
		return;
	}
	gQueueClass = objc_allocateClassPair(base, "HakiosMetalQueue", 0);
	if (gQueueClass == Nil) {
		say(@"could not make the class HakiosMetalQueue");
		return;
	}
	class_addProtocol(gQueueClass, protocol);
	if (gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
		gProtocols[gProtocolCount++] = protocol;
	}
	/*
	 * The third run: -setCompletionQueue: is not in MTLCommandQueue but in a private protocol of Metal's own (as MTLDeviceSPI is
	 * for the device), and the window server sends it. Every protocol of this process whose name begins with MTLCommandQueue is
	 * taken, so that the stub answers what any of them declares; their names go to the log, which is how the next one is found.
	 */
	unsigned int count = 0;
	Protocol * __unsafe_unretained * all = objc_copyProtocolList(&count);

	for (unsigned int i = 0; i < count; i++) {
		const char * name = protocol_getName(all[i]);

		if (strncmp(name, "MTLCommandQueue", 15) == 0 && all[i] != protocol && gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
			class_addProtocol(gQueueClass, all[i]);
			gProtocols[gProtocolCount++] = all[i];
			say([NSString stringWithFormat:@"the queue also declares the protocol %s", name]);
		}
	}
	free(all);
	class_addMethod(gQueueClass, sel_registerName("device"), (IMP)queueDevice, "@@:");
	class_addMethod(gQueueClass, sel_registerName("label"), (IMP)queueLabel, "@@:");
	class_addMethod(gQueueClass, sel_registerName("setGPUPriority:"), (IMP)queueSetGPUPriority, "v@:Q");
	class_addMethod(gQueueClass, sel_registerName("methodSignatureForSelector:"), (IMP)deviceSignature, "@@::");
	class_addMethod(gQueueClass, sel_registerName("forwardInvocation:"), (IMP)deviceForward, "v@:@");
	class_addMethod(gQueueClass, sel_registerName("dealloc"), (IMP)queueDealloc, "v@:");
#ifdef HAKIOS_METAL_GPU
	class_addIvar(gQueueClass, "_hakios", sizeof(void *), sizeof(void *) == 8 ? 3 : 2, "^v");
	class_addMethod(gQueueClass, sel_registerName("commandBuffer"), (IMP)queueCommandBuffer, "@@:");
	class_addMethod(gQueueClass, sel_registerName("commandBufferWithUnretainedReferences"), (IMP)queueCommandBuffer, "@@:");
#endif
	objc_registerClassPair(gQueueClass);
	say(@"the class HakiosMetalQueue is registered (a subclass of _MTLCommandQueue)");
#ifdef HAKIOS_METAL_GPU
	registerCommandClasses();
#endif
}

/*
 * Whether this process may be shown the device, by the level of boot-arg metal (read from sysctl kern.bootargs):
 *
 *   metal=1  only the program metal_test (userland/src/metal_test.m): the safe level, for seeing what Metal does with the device
 *   metal=2  every process but backboardd
 *   metal=3  every process, backboardd too, to find out what the window server asks
 *
 * Why not simply everyone. CoreAnimation's window server tries Metal FIRST and goes to its OpenGL ES path (which works on this port)
 * only when no Metal device exists; with ours it takes Metal and stops on "metallib_url != nil": its own shader library,
 * QuartzCore.framework/default.metallib, is not in this system image (the framework has only a default.pipelinelib that names
 * it), so the window server cannot run on Metal here at all. And mediaserverd, given a device that cannot do anything, ends and
 * is started again. A device that is shown to a process that cannot use it costs that process, so it is shown to as few as the
 * experiment needs.
 *
 * Besides the level, a file HakiosMetal.clients beside the bundle (the way UniXNU's AdrenoMetal chooses its clients) names more
 * processes, one per line, "*" for every one: an image can show the device to the one program being worked on without a boot-arg.
 */
static BOOL
listedBesideTheBundle(const char *name)
{
	Dl_info info;
	char path[1024], line[256];
	char *slash;
	FILE *file;
	BOOL listed = NO;

	if (dladdr((const void *)&listedBesideTheBundle, &info) == 0 || info.dli_fname == NULL) {
		return NO;
	}
	strlcpy(path, info.dli_fname, sizeof(path));            /* <Extensions>/HakiosMetal.bundle/HakiosMetal */
	if ((slash = strrchr(path, '/')) == NULL) {
		return NO;
	}
	*slash = '\0';
	if ((slash = strrchr(path, '/')) == NULL) {
		return NO;
	}
	strlcpy(slash + 1, "HakiosMetal.clients", sizeof(path) - (size_t)(slash + 1 - path));
	if ((file = fopen(path, "r")) == NULL) {
		return NO;
	}
	while (!listed && fgets(line, sizeof(line), file) != NULL) {
		line[strcspn(line, " \t\r\n#")] = '\0';
		listed = strcmp(line, "*") == 0 || (line[0] != '\0' && strcmp(line, name) == 0);
	}
	fclose(file);
	return listed;
}

static BOOL
shownToThisProcess(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char * name = getprogname();
	int level = 1;

	if (name != NULL && (strcmp(name, "metal_test") == 0 || strcmp(name, "hakios-test") == 0 || strcmp(name, "mtldisc") == 0)) {
		return YES;
	}
	if (getenv("HAKIOS_METAL_ENABLE") != NULL || getenv("USE_HAKIOS") != NULL) {
		return YES;
	}
	FILE *cf = fopen("/var/jb/etc/hakiosmetal.clients", "r");
	if (cf == NULL) cf = fopen("/var/jb/etc/sweetmetal.clients", "r");
	if (cf != NULL) {
		char line[256];
		BOOL ok = NO;
		while (!ok && fgets(line, sizeof(line), cf) != NULL) {
			line[strcspn(line, " \t\r\n#")] = '\0';
			ok = strcmp(line, "*") == 0 || (line[0] != '\0' && name != NULL && strcmp(line, name) == 0);
		}
		fclose(cf);
		if (ok) return YES;
	}

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		const char * at = strstr(args, "metal=");

		if (at != NULL) {
			level = atoi(at + 6);
		}
	}
	if (name == NULL) {
		return NO;
	}
	if (level >= 3 || (level == 2 && strcmp(name, "backboardd") != 0) || (level == 1 && strcmp(name, "metal_test") == 0)) {
		return YES;
	}
	if (listedBesideTheBundle(name)) {
		say([NSString stringWithFormat:@"%s is shown the device: HakiosMetal.clients beside the bundle names it", name]);
		return YES;
	}
	say([NSString stringWithFormat:@"%s is not shown the device (metal=%d; 1 is metal_test only, 2 all but backboardd, 3 all)", name, level]);
	return NO;
}

__attribute__((constructor)) static void
registerDevice(void)
{
	Class device;

	if (!shownToThisProcess()) {
		return;
	}

	gBase = objc_getClass("_MTLDevice");
	if (gBase == Nil) {
		say(@"this system has no _MTLDevice: no device class is registered");
		return;
	}
	if (objc_getClass("HakiosMetalDevice") != Nil) {
		return;
	}
	device = objc_allocateClassPair(gBase, "HakiosMetalDevice", 0);
	if (device == Nil) {
		say(@"could not make the class HakiosMetalDevice");
		return;
	}
	/*
	 * The first run said it: MTLAddDevice asserts [newDevice conformsToProtocol:@protocol(MTLDevice)] and aborts the process
	 * (backboardd) when the class does not declare it. _MTLDevice itself does not, so the class does.
	 */
	class_addProtocol(device, @protocol(MTLDevice));
	gProtocols[gProtocolCount++] = @protocol(MTLDevice);
	/*
	 * ... and the second run said that MTLAddDevice asserts the private protocol MTLDeviceSPI as well (line 835). Private
	 * protocols are not in the SDK, so they are taken by name from the system's own Metal; each next assertion of this kind
	 * that a run shows is a name for this list.
	 */
	static const char * const privateProtocols[] = { "MTLDeviceSPI", NULL };
	for (const char * const * name = privateProtocols; *name != NULL; name++) {
		Protocol * protocol = objc_getProtocol(*name);

		if (protocol != NULL && gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
			class_addProtocol(device, protocol);
			gProtocols[gProtocolCount++] = protocol;
		} else {
			say([NSString stringWithFormat:@"the protocol %s is not in this system's Metal", *name]);
		}
	}
	class_addMethod(device, @selector(initWithAcceleratorPort:), (IMP)deviceInit, "@@:I");
	class_addMethod(device, @selector(name), (IMP)deviceName, "@@:");
	class_addMethod(device, @selector(registryID), (IMP)deviceRegistryID, "Q@:");
	class_addMethod(device, sel_registerName("maxBufferLength"), (IMP)deviceMaxBufferLength, "Q@:");
	class_addMethod(device, sel_registerName("maxThreadsPerThreadgroup"), (IMP)deviceMaxThreadsPerThreadgroup, "{?=QQQ}@:");
	class_addMethod(device, sel_registerName("newCommandQueue"), (IMP)deviceNewQueue, "@@:");
	class_addMethod(device, sel_registerName("newCommandQueueWithMaxCommandBufferCount:"), (IMP)deviceNewQueueMax, "@@:Q");
	class_addMethod(device, sel_registerName("methodSignatureForSelector:"), (IMP)deviceSignature, "@@::");
	class_addMethod(device, sel_registerName("forwardInvocation:"), (IMP)deviceForward, "v@:@");
	objc_registerClassPair(device);
	say(@"the class HakiosMetalDevice is registered (a subclass of _MTLDevice)");
	if (objc_getClass("SweetMetalDevice") == Nil) {
		Class sweet = objc_allocateClassPair(device, "SweetMetalDevice", 0);
		if (sweet != Nil) {
			objc_registerClassPair(sweet);
			say(@"the class SweetMetalDevice is registered (a subclass of HakiosMetalDevice)");
		}
	}
	registerQueueClass();
	registerStubClasses(device);
	registerObjectClasses(device);
}

#ifdef __cplusplus
extern "C" {
#endif
__attribute__((visibility("default"))) id
hakios_metal_open_device(unsigned int port)
{
	Class cls = objc_getClass("HakiosMetalDevice");
	if (cls == Nil) cls = objc_getClass("SweetMetalDevice");
	if (cls == Nil) return nil;
	return ((id (*)(id, SEL, unsigned int))objc_msgSend)([cls alloc], sel_registerName("initWithAcceleratorPort:"), port);
}
#ifdef __cplusplus
}
#endif
