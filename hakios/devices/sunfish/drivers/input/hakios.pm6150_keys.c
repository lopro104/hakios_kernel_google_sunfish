/*
 * The Pixel 4a's buttons, for /dev/hakios-touch, in place of drivers/input/hakios.buttons.c, whose addresses are the
 * reference phone's PMICs. Boot-arg buttons=1, or buttons not given and touchscreen=1, as there.
 *
 *   power        KPDPWR_N, power-on block of the PM6150 (SID 0): real-time status 0x0810, bit 0
 *   volume down  RESIN_N, the same register, bit 1
 *   volume up    GPIO2 of the PM6150L (SID 4): status 0xc110, bit 0, low while pressed
 *
 * THIS DRIVER ONLY READS: the one write is the command word that asks the arbiter for a read. If volume up's pin is not an
 * enabled input, making it one would be a write to a PMIC, so volume up is left out and the log says so. A key that reads as
 * held before it has been seen released is not believed; a change must be seen on two looks in a row.
 *
 * Where the numbers come from: bsd/dev/sweet_keys.c of ios-sunfish, which ran on this phone. Status: RUN ON THE PHONE, iOS 16.5
 * (2026-11-05): the user pressed all three, and each did what it should.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);
extern kern_return_t thread_terminate(thread_t thread);

#define ARB_CORE_PA     0x0c440000UL
#define ARB_CFG_PA      0x0c40a000UL
#define ARB_OBS_PA      0x0e600000UL
#define OBS_STATUS      0x08
#define OBS_DATA0       0x18
#define STATUS_DONE     1u
#define STATUS_BAD      0x0eu
#define COMMAND_READ    (1u << 27)
#define READ_TIMEOUT_US 2000
#define NO_CHANNEL      0xffffu
#define POLL_MS         16
#define START_SECONDS   3

#define PON_BLOCK       0x008u
#define PON_STATUS      0x10u
#define PON_POWER       0x01u
#define PON_VOLUME_DOWN 0x02u
#define GPIO_BLOCK      0x4c1u
#define GPIO_STATUS     0x10u
#define GPIO_SUBTYPE    0x05u
#define GPIO_MODE       0x40u
#define GPIO_ENABLE     0x46u
#define GPIO_LEVEL      0x01u

static vm_offset_t arb_core, arb_cfg, arb_obs;
static unsigned pon_channel = NO_CHANNEL, gpio_channel = NO_CHANNEL;
static uint64_t reads, failures;

static struct key {
	const char *name;
	uint32_t record;
	bool usable, live, last, down;
	unsigned presses;
} power = { .name = "power", .record = HAKIOS_BUTTON_LOCK },
    volume_up = { .name = "volume up", .record = HAKIOS_BUTTON_VOLUME_UP },
    volume_down = { .name = "volume down", .record = HAKIOS_BUTTON_VOLUME_DOWN };

static inline uint32_t
rd(vm_offset_t address)
{
	uint32_t value = *(volatile uint32_t *)address;

	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static unsigned
channel_of(unsigned block, unsigned channels)
{
	unsigned found = NO_CHANNEL;

	for (unsigned i = 0; i < channels; i++) {
		uint32_t map = rd(arb_core + 0x900u + 4u * i);

		if (map == 0 || ((map >> 8) & 0xfffu) != block) {
			continue;
		}
		if (found == NO_CHANNEL || (rd(arb_cfg + 0x700u + 4u * i) & 7u) == 0) {
			found = i;
		}
	}
	return found;
}

static bool
read_reg(unsigned channel, uint8_t reg, uint8_t *value)
{
	vm_offset_t ch = arb_obs + 0x80UL * channel;
	uint64_t start, interval;
	uint32_t status, data;

	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	start = mach_absolute_time();
	hakios_spmi_lock();
	*(volatile uint32_t *)ch = COMMAND_READ | (uint32_t)reg << 4;
	__asm__ volatile("dsb osh" ::: "memory");
	do {
		status = rd(ch + OBS_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	data = rd(ch + OBS_DATA0);
	hakios_spmi_unlock();
	reads++;
	if (!(status & STATUS_DONE) || (status & STATUS_BAD)) {
		failures++;
		return false;
	}
	*value = (uint8_t)data;
	return true;
}

static void
look(struct key *k, bool pressed)
{
	bool steady = pressed == k->last;

	k->last = pressed;
	if (!k->usable || !steady) {
		return;
	}
	if (!k->live) {
		if (!pressed) {
			k->live = true;
			printf("pm6150_keys: %s is released now: believed from here on\n", k->name);
		}
		return;
	}
	if (pressed == k->down) {
		return;
	}
	k->down = pressed;
	if (pressed) {
		k->presses++;
	}
	printf("pm6150_keys: %s %s\n", k->name, pressed ? "PRESSED" : "released");
	hakios_touch_post(pressed ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, k->record, 0, 0);
}

static void
keys_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t version;
	unsigned channels;
	uint8_t value = 0, subtype = 0, mode = 0, enable = 0;

	delay_for_interval(START_SECONDS, NSEC_PER_SEC);
	arb_core = ml_io_map(ARB_CORE_PA, 0x2000);
	arb_cfg = ml_io_map(ARB_CFG_PA, 0x1000);
	arb_obs = ml_io_map(ARB_OBS_PA, 0x10000);
	version = rd(arb_core);
	channels = rd(arb_core + 4) & 0x7ffu;
	if (version < 0x50000000u || version >= 0x70000000u || channels == 0 || channels > 512) {
		printf("pm6150_keys: PMIC arbiter version %#x, %u channels: no buttons\n", version, channels);
		thread_terminate(current_thread());
	}
	pon_channel = channel_of(PON_BLOCK, channels);
	gpio_channel = channel_of(GPIO_BLOCK, channels);
	if (pon_channel != NO_CHANNEL && read_reg(pon_channel, PON_STATUS, &value)) {
		power.usable = volume_down.usable = true;
		power.last = value & PON_POWER;
		volume_down.last = value & PON_VOLUME_DOWN;
	}
	if (gpio_channel != NO_CHANNEL && read_reg(gpio_channel, GPIO_SUBTYPE, &subtype) &&
	    read_reg(gpio_channel, GPIO_MODE, &mode) && read_reg(gpio_channel, GPIO_ENABLE, &enable) &&
	    read_reg(gpio_channel, GPIO_STATUS, &value)) {
		/* the mode's place depends on the block's kind, as Linux's pinctrl-spmi-gpio has it */
		unsigned direction = subtype >= 0x10u && subtype <= 0x13u ? mode & 3u : (mode >> 4) & 7u;

		if ((enable & 0x80u) && (direction == 0 || direction == 2)) {
			volume_up.usable = true;
			volume_up.last = !(value & GPIO_LEVEL);
		} else {
			printf("pm6150_keys: volume up's pin is not an enabled input (subtype %#x, mode %#x, enable %#x): left alone, "
			    "as making it one is a write to a PMIC\n", subtype, mode, enable);
		}
	}
	power.live = power.usable && !power.last;
	volume_down.live = volume_down.usable && !volume_down.last;
	volume_up.live = volume_up.usable && !volume_up.last;
	printf("pm6150_keys: watching every %d ms:%s%s%s, read only (arbiter %#x, channels %u and %u)\n", POLL_MS,
	    power.usable ? " power" : "", volume_up.usable ? " volume-up" : "", volume_down.usable ? " volume-down" : "", version,
	    pon_channel, gpio_channel);
	if (!power.usable && !volume_up.usable) {
		thread_terminate(current_thread());
	}
	for (;;) {
		if (power.usable && read_reg(pon_channel, PON_STATUS, &value)) {
			look(&power, value & PON_POWER);
			look(&volume_down, value & PON_VOLUME_DOWN);
		}
		if (volume_up.usable && read_reg(gpio_channel, GPIO_STATUS, &value)) {
			look(&volume_up, !(value & GPIO_LEVEL));
		}
		if (failures >= 1000 && failures * 2 > reads) {
			printf("pm6150_keys: more than half of %llu reads have failed: giving up\n", reads);
			thread_terminate(current_thread());
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
}

static void
start_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;

	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("pm6150_keys: could not start its thread\n");
	}
}

__startup_func
static void
pm6150_keys_init(void)
{
	uint32_t enabled = 0, touch_screen = 0;

	if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled))) {
		enabled = PE_parse_boot_argn("touchscreen", &touch_screen, sizeof(touch_screen)) && touch_screen != 0;
	}
	if (enabled) {
		thread_call_enter(thread_call_allocate(start_run, NULL));
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, pm6150_keys_init);
