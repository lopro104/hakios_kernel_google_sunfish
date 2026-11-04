/*
 * MiniIOWatchdog: a stand-in for Apple's IOWatchdog, as Darwin's watchdogd sees it. Boot-arg iowatchdog=1.
 *
 * watchdogd is a launchd job with _PanicOnCrash. It opens the service IOWatchdog with type 1 and stops when that is
 * missing, which panics the system; it also looks for the service with IOPMUPrimary = 1, to write the boot stage to
 * the PMU, and stops when there is none. Only a boot that runs the root's own job list has it (the port's job lists
 * leave it out). On Apple's hardware the driver arms a watchdog in the SoC. Here nothing is armed: every call is
 * accepted and does nothing, each selector logged the first time.
 *
 * Selectors, from watchdogd: check-in 1, enable or disable user space monitoring 3/4 and 0/5, structures 2 and 9.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

class IOWatchdog : public IOService
{
	OSDeclareDefaultStructors(IOWatchdog);
public:
	IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOWatchdog, IOService);

IOReturn
IOWatchdog::setProperties(__unused OSObject * properties)
{
	return kIOReturnSuccess;        /* the boot stage for the PMU there is not */
}

class IOWatchdogUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOWatchdogUserClient);
public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOWatchdogUserClient, IOUserClient);

IOReturn
IOWatchdogUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

static uint64_t mini_watchdog_logged;   /* selectors already logged (< 64) */

IOReturn
IOWatchdogUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    __unused IOExternalMethodDispatch * dispatch, __unused OSObject * target, __unused void * reference)
{
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutputDescriptor != NULL) {
		static const uint64_t zero = 0;
		IOByteCount len = args->structureOutputDescriptor->getLength();

		for (IOByteCount off = 0; off < len; off += 8) {
			args->structureOutputDescriptor->writeBytes(off, &zero, len - off < 8 ? len - off : 8);
		}
	} else if (args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(mini_watchdog_logged & (1ULL << selector))) {
		char name[32];

		if (selector < 64) {
			mini_watchdog_logged |= 1ULL << selector;
		}
		proc_selfname(name, sizeof(name));
		IOLog("MiniIOWatchdog: %s selector %u (scalars %u in, %u out; structure %u in, %u out)\n", name, selector,
		    args->scalarInputCount, args->scalarOutputCount, args->structureInputSize, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

void
MiniIOWatchdogStart(void)
{
	IOWatchdog * service;
	unsigned on = 0;

	if (!PE_parse_boot_argn("iowatchdog", &on, sizeof(on)) || on != 1) {
		return;
	}
	service = OSTypeAlloc(IOWatchdog);
	if (service == NULL || !service->init()) {
		IOLog("MiniIOWatchdog: cannot create the service\n");
		OSSafeReleaseNULL(service);
		return;
	}
	service->setProperty(kIOUserClientClassKey, "IOWatchdogUserClient");
	service->setProperty("IOPMUPrimary", 1ULL, 32);
	if (service->attach(IOService::getResourceService()) && service->start(IOService::getResourceService())) {
		service->registerService();
		IOLog("MiniIOWatchdog: registered, accepts check-ins, never fires\n");
	}
	service->release();
}
