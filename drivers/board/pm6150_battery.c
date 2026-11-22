/*
 * (HakiOS common: started by drivers.c on a board that lists "battery-pm6150"; ported from the Darwin 16.5 port's
 * hakios.pm6150_battery.c, addresses still this one phone's.)
 *
 * The Pixel 4a's battery, read off the PM6150: SunFish has no DSP battery service and no firmware charger page to ask, so
 * this gives drivers/power/hakios.battery.c what those give on the reference phone. Only with boot-arg pm6150_battery=1.
 *
 * THIS DRIVER ONLY READS (but with bat_measure=, below: USB input suspended for a minute). The one thing it writes is the command word that asks the arbiter for a read (as
 * drivers/power/hakios.pmic.c), and it writes nothing to a PMIC. It does not control charging: whether the phone charges
 * is up to the PMIC's hardware and what was set before it. It says what it sees.
 *
 *   BMS  0x4804 type (0x0d), 0x4805 subtype, 0x4808 bit 0 battery there, 0x48c0 voltage, 0x48c2 current
 *   USB  0x1310 bit 4 something on the port; 0x110b the battery's path; 0x1006, 0x1007, 0x100d the charger's state
 *
 * The percentage is an ESTIMATE, not a fuel gauge: Google's open-circuit curve for the ATL cell at 25 C
 * (msm-4.14 batterydata-qcom-s5-SWD-ATL.dtsi) with the load's effect taken off (I * 150 mOhm, the mainline sunfish dts),
 * then the charge counted from the current as Linux's qcom_qg.c does for this phone, moving 1% per 20 s at most. After a
 * reboot it starts again from the voltage, so a phone that has just been under load can be some percent off.
 *
 * THE TEMPERATURE IS NOT MEASURED: 25 C always. The thermistor needs a conversion of the ADC, that is writes to the PMIC
 * (sweet_spmi.c of ios-sunfish did them; not brought here).
 *
 * Where the numbers come from: iokit/Kernel/arm/SweetBattery.cpp of ios-sunfish, which ran on this phone (2026-10-25).
 * Status: RUN ON THE PHONE (2026-11-05, iOS 16.5, on USB, full): 4439 mV, 0 mA, charger state 5, 25 reads, 0 failed, and iOS
 * showed 100%. Not yet seen: on battery, discharging, or charging from low.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/board/input.h>        /* hakios_battery_report(), hakios_spmi_lock() */
#include <sys/systm.h>

#include "board.h"
#include "log.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t);

#define ARB_CORE_PA     0x0c440000UL
#define ARB_CFG_PA      0x0c40a000UL
#define ARB_OBS_PA      0x0e600000UL    /* EE 0, 0x80 for each channel */
#define OBS_COMMAND     0x00
#define OBS_STATUS      0x08
#define OBS_DATA0       0x18
#define STATUS_DONE     1u
#define STATUS_BAD      0x0eu           /* failure, denied, dropped */
#define COMMAND_READ    (1u << 27)
#define READ_TIMEOUT_US 2000
#define NO_CHANNEL      0xffffu
#define PERIOD_SECONDS  10

#define CAPACITY_MAH    3140
#define RESISTANCE_MOHM 150

static const unsigned short sunfish_mv[] = {3000, 3377, 3499, 3578, 3636, 3679,
    3699, 3703, 3705, 3706, 3709, 3719, 3736, 3752, 3766, 3778, 3789, 3797,
    3802, 3806, 3811, 3815, 3820, 3826, 3833, 3840, 3848, 3858, 3868, 3878,
    3888, 3902, 3926, 3953, 3973, 3990, 4007, 4025, 4044, 4064, 4085, 4107,
    4129, 4152, 4176, 4200, 4224, 4249, 4274, 4299, 4323, 4346, 4368, 4390,
    4412, 4437};
static const unsigned char sunfish_soc[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46,
    48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82,
    84, 86, 88, 90, 92, 94, 96, 98, 100};
#define CURVE_POINTS    (sizeof(sunfish_mv) / sizeof(sunfish_mv[0]))

static vm_offset_t arb_core, arb_cfg, arb_obs;
static unsigned arb_channels;
static thread_call_t poll_call;

/* what is counted */
static bool counting;
static int64_t charge_uah;
static uint64_t count_ms, shown_ms;
static int shown = -1;
static unsigned polls, failures;

static inline uint32_t
rd(vm_offset_t address)
{
	uint32_t value = *(volatile uint32_t *)address;

	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

/* The channel of a block of SID 0; the one EE 0 owns if there are two. */
static unsigned
channel_of(unsigned block)
{
	unsigned found = NO_CHANNEL;

	for (unsigned i = 0; i < arb_channels; i++) {
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
read_regs(unsigned address, uint8_t *bytes, unsigned count)
{
	static uint16_t channels[4096];        /* (slave << 8 | peripheral: slave 0 and others, pmic_dump) */
	static bool looked[4096];
	unsigned block = address >> 8;
	vm_offset_t ch;
	uint64_t start, interval;
	uint32_t status, data;

	if (!looked[block]) {
		channels[block] = (uint16_t)channel_of(block);
		looked[block] = true;
	}
	if (channels[block] == NO_CHANNEL) {
		return false;
	}
	ch = arb_obs + 0x80UL * channels[block];
	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	start = mach_absolute_time();
	hakios_spmi_lock();
	*(volatile uint32_t *)(ch + OBS_COMMAND) = COMMAND_READ | (address & 0xffu) << 4 | (count - 1);
	__asm__ volatile("dsb osh" ::: "memory");
	do {
		status = rd(ch + OBS_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	data = rd(ch + OBS_DATA0);
	hakios_spmi_unlock();
	if (!(status & STATUS_DONE) || (status & STATUS_BAD)) {
		return false;
	}
	for (unsigned i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data >> (8 * i));
	}
	return true;
}

static bool
read8(unsigned address, uint8_t *value)
{
	return read_regs(address, value, 1);
}

static bool
read16(unsigned address, uint16_t *value)
{
	uint8_t b[2];

	if (!read_regs(address, b, 2)) {
		return false;
	}
	*value = (uint16_t)(b[0] | b[1] << 8);
	return true;
}

/*
 * ---- bat_measure=T [bat_measure_s=N]: the phone's own draw, measured with the cable in ----
 * T seconds after boot the charger is told to take nothing from USB (USBIN_SUSPEND, bit 0 of USBIN_CMD_IL 0x1340, as
 * Linux's qcom smb5 driver does it): the phone then runs from its battery, and the gauge's current is what the phone
 * draws. Logged every second for N s (default 60, at most 120), then the bit is cleared again -- always; and at the
 * driver's start it is cleared if a run before left it set. THE ONE WRITE THIS DRIVER MAKES, only with bat_measure.
 */
#define ARB_CHNLS_PA    0x0c600000UL    /* writes: 0x10000 per channel */
#define USBIN_CMD_IL    0x1340u
#define USBIN_SUSPEND   0x01u

static uint32_t measure_at, measure_for = 60;
static bool measure_done;

static bool
write8(unsigned address, uint8_t value)
{
	unsigned channel = channel_of(address >> 8);
	vm_offset_t ch;
	uint64_t start, interval;
	uint32_t status;
	uint8_t back = 0xff;

	if (channel == NO_CHANNEL || (rd(arb_cfg + 0x700u + 4u * channel) & 7u) != 0) {
		hakios_say("pm6150_battery: %#x: no channel this system may write through\n", address);
		return false;
	}
	ch = ml_io_map(ARB_CHNLS_PA + 0x10000UL * channel, 0x1000);
	if (ch == 0) {
		return false;
	}
	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	start = mach_absolute_time();
	hakios_spmi_lock();
	*(volatile uint32_t *)(ch + 0x10) = value;                                     /* WDATA0 */
	*(volatile uint32_t *)(ch + OBS_COMMAND) = (address & 0xffu) << 4;             /* extended write, 1 byte */
	__asm__ volatile("dsb osh" ::: "memory");
	do {
		status = rd(ch + OBS_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	hakios_spmi_unlock();
	if (!(status & STATUS_DONE) || (status & STATUS_BAD) || !read8(address, &back) || back != value) {
		hakios_say("pm6150_battery: %#x <- %#x failed (status %#x, reads %#x)\n", address, value, status, back);
		return false;
	}
	return true;
}

static bool
usb_suspend(bool on)
{
	uint8_t now;

	if (!read8(USBIN_CMD_IL, &now)) {
		return false;
	}
	if (((now & USBIN_SUSPEND) != 0) == on) {
		return true;
	}
	return write8(USBIN_CMD_IL, on ? (uint8_t)(now | USBIN_SUSPEND) : (uint8_t)(now & ~USBIN_SUSPEND));
}

static int
current_ma(bool *ok)
{
	uint8_t subtype;
	uint16_t raw;

	*ok = read8(0x4805, &subtype) && (subtype == 3 || subtype == 4) && read16(0x48c2, &raw) && raw != 0x8000;
	if (!*ok) {
		return 0;
	}
	int signed_raw = (raw & 0x8000u) ? (int)raw - 65536 : (int)raw;

	return (int)(-((int64_t)signed_raw * (subtype == 3 ? 152588 : 305176)) / 1000000);
}

static void
measure_run(__unused void *parameter, __unused wait_result_t result)
{
	int64_t sum = 0;
	unsigned n = 0;
	int low = 1 << 30, high = -(1 << 30);

	if (!usb_suspend(true)) {
		hakios_say("pm6150_battery: measure: USB input could not be suspended\n");
		thread_terminate(current_thread());
		return;
	}
	hakios_say("pm6150_battery: measure: USB input suspended for %u s, the phone on its battery\n", measure_for);
	delay_for_interval(3, NSEC_PER_SEC);    /* (the current settles) */
	for (unsigned i = 0; i < measure_for; i++) {
		bool ok;
		int ma = current_ma(&ok);

		if (ok) {
			sum += ma;
			n++;
			low = ma < low ? ma : low;
			high = ma > high ? ma : high;
			hakios_say("pm6150_battery: measure: %u s %d mA\n", i, ma);
		}
		delay_for_interval(1, NSEC_PER_SEC);
	}
	for (int tries = 0; tries < 5 && !usb_suspend(false); tries++) {
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	hakios_say("pm6150_battery: measure: MEAN %lld mA over %u readings (%d to %d); USB input back on\n",
	    n ? (long long)(sum / n) : 0LL, n, low, high);
	thread_terminate(current_thread());
}

static void
measure_check(void)
{
	uint64_t now;
	thread_t thread;

	if (measure_at == 0 || measure_done) {
		return;
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &now);
	if (now / NSEC_PER_SEC < measure_at) {
		return;
	}
	measure_done = true;
	if (kernel_thread_start(measure_run, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Boot-arg pmic_dump=1: every peripheral of the PM6150 (slave 1) and the PM6150L (slave 5), where PMIC5 keeps its
 * regulators, said once:
 * its type, whether it is ready (STATUS1 bit 7) and enabled (EN_CTL bit 7), and its voltage (VSET, mV). For the
 * battery: which supplies are on that Android leaves off (research/power, 2026-11-19). Reads only.
 */
static void
pmic_dump(void)
{
	static const unsigned slaves[2] = { 1, 5 };     /* (the regulators: the second address of each PMIC) */
	unsigned on = 0;

	for (unsigned s = 0; s < 2; s++) {
		for (unsigned periph = 0x10; periph < 0xc0; periph++) {
			unsigned base = (slaves[s] << 16) | (periph << 8);
			uint8_t type, subtype, status, enable, lo, hi;

			if (!read8(base + 0x04, &type) || type == 0 || type == 0xff || !read8(base + 0x05, &subtype) ||
			    !read8(base + 0x08, &status) || !read8(base + 0x46, &enable)) {
				continue;
			}
			/* PMIC5's regulators: types 0x04 (LDO, HF/FT SMPS on some), 0x1c, 0x1d, 0x21, 0x22, 0x2c (BOB) */
			/* (all of them: the regulators are told apart by their type afterwards) */
			lo = hi = 0;
			read8(base + 0x40, &lo);
			read8(base + 0x41, &hi);
			if ((enable & 0x80) || (status & 0x80)) {
				on++;
			}
			hakios_say("pmic_dump: sid %u periph %#04x type %#04x/%#04x status %#04x enable %#04x %u mV%s\n", slaves[s], periph,
			    type, subtype, status, enable, (unsigned)(lo | hi << 8), (enable & 0x80) || (status & 0x80) ? "  ON" : "");
		}
	}
	hakios_say("pmic_dump: %u regulators on\n", on);
}

/*
 * With pmic_dump=1 too: the chip's power domains (GDSCs, msm-4.14 sdmmagpie-gdsc.dtsi) -- powered (bit 31) or not --
 * for the battery: what the boot loaders left on that Android switches off. A clock controller's registers are read
 * only when GCC says its bus clock runs (CLK_OFF, bit 31 of the branch, clear): reading one without it can hang the bus.
 */
static uint32_t
phys_read32(uint64_t pa)
{
	vm_offset_t page = ml_io_map((vm_offset_t)(pa & ~0x3fffULL), 0x4000);

	return page != 0 ? rd(page + (pa & 0x3fffULL)) : 0xffffffffu;
}

static void
gdsc_dump(void)
{
	static const struct { const char *name; uint64_t pa; uint32_t ahb; } domains[] = {
		{ "pcie_0", 0x16b004, 0 }, { "ufs_phy", 0x177004, 0 }, { "usb30_prim", 0x10f004, 0 },
		{ "vote audio_tbu", 0x17d030, 0 }, { "vote pcie_tbu", 0x17d03c, 0 }, { "vote tbu1", 0x17d034, 0 },
		{ "vote tbu2", 0x17d038, 0 }, { "vote mmnoc_hf0", 0x17d040, 0 }, { "vote mmnoc_hf1", 0x17d048, 0 },
		{ "vote mmnoc_sf", 0x17d044, 0 },
		{ "camera bps", 0xad07004, 0xb008 }, { "camera ife_0", 0xad0a004, 0xb008 }, { "camera ife_1", 0xad0b004, 0xb008 },
		{ "camera ipe_0", 0xad08004, 0xb008 }, { "camera ipe_1", 0xad09004, 0xb008 }, { "camera titan_top", 0xad0c1c4, 0xb008 },
		{ "display mdss_core", 0xaf03000, 0xb00c },
		{ "gpu cx", 0x509106c, 0 }, { "gpu gx", 0x509100c, 0 },
		{ "video mvsc", 0xab00814, 0xb004 }, { "video mvs0", 0xab00874, 0xb004 }, { "video mvs1", 0xab008b4, 0xb004 },
		{ "npu core", 0x9911028, 0x4d004 },
	};

	for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); i++) {
		if (domains[i].ahb != 0) {
			uint32_t branch = phys_read32(0x100000 + domains[i].ahb);

			if (branch == 0xffffffffu || (branch & 0x80000000u)) {
				hakios_say("gdsc_dump: %-20s its controller's bus clock is off (GCC %#x = %#x): not read\n", domains[i].name,
				    domains[i].ahb, branch);
				continue;
			}
		}
		uint32_t v = phys_read32(domains[i].pa);

		hakios_say("gdsc_dump: %-20s %#010x %s\n", domains[i].name, v, (v & 0x80000000u) ? "POWERED" : "off");
	}
}

/* The curve, between its points. */
static unsigned
percent_at(unsigned mv)
{
	if (mv <= sunfish_mv[0]) {
		return sunfish_soc[0];
	}
	for (unsigned i = 1; i < CURVE_POINTS; i++) {
		if (mv <= sunfish_mv[i]) {
			unsigned low = sunfish_mv[i - 1], high = sunfish_mv[i];

			return sunfish_soc[i - 1] + (sunfish_soc[i] - sunfish_soc[i - 1]) * (mv - low) / (high - low);
		}
	}
	return 100;
}

/* What the screen shows: toward raw by at most 1% per 20 s, never up while discharging nor down while charging. */
static unsigned
smoothed(unsigned raw, int ma, uint64_t now)
{
	int difference;
	int64_t steps;

	if (shown < 0) {
		shown = (int)raw;
		shown_ms = now;
		return raw;
	}
	difference = (int)raw - shown;
	if (difference == 0 || (difference > 0 && ma < -10) || (difference < 0 && ma > 10)) {
		shown_ms = now;
		return (unsigned)shown;
	}
	steps = (int64_t)(now - shown_ms) / 20000;
	if (steps == 0) {
		return (unsigned)shown;
	}
	if (difference > steps) {
		difference = (int)steps;
	} else if (difference < -steps) {
		difference = -(int)steps;
	}
	shown += difference;
	shown_ms += (uint64_t)steps * 20000;
	return (unsigned)shown;
}

static void
poll_once(void)
{
	struct hakios_battery_state b = { .size = sizeof(b) };
	uint8_t type, present, usb, path, charger, error, temperature, subtype;
	uint16_t raw_voltage, raw_current;
	bool current_valid, external, capable, charging, full;
	unsigned mv, state, percent;
	int ma = 0;

	polls++;
	if (!read8(0x4804, &type) || type != 0x0d || !read8(0x4808, &present) || !(present & 1u) ||
	    !read16(0x48c0, &raw_voltage) || raw_voltage == 0x8000 ||
	    !read8(0x1310, &usb) || !read8(0x110b, &path) || !read8(0x1006, &charger) ||
	    !read8(0x1007, &error) || !read8(0x100d, &temperature)) {
		failures++;
		return;         /* nothing reported: no reading must not look like an empty, or a full, battery */
	}
	mv = (unsigned)(((uint64_t)raw_voltage * 194637u) / 1000000u);
	if (mv < 2800 || mv > 4600) {
		failures++;
		return;
	}
	external = (usb & 0x10u) != 0;
	capable = external && (path & 0x10u) && (path & 1u) && !(path & 0x40u);
	state = charger & 7u;
	charging = capable && state >= 1 && state <= 4 && !(error & 2u) && !(temperature & 0x0cu);
	full = external && state == 5 && !(error & 2u);     /* (inhibit, 0, does not by itself mean full) */
	current_valid = read8(0x4805, &subtype) && (subtype == 3 || subtype == 4) && read16(0x48c2, &raw_current) &&
	    raw_current != 0x8000;
	if (current_valid) {
		int signed_raw = (raw_current & 0x8000u) ? (int)raw_current - 65536 : (int)raw_current;

		/* The PMIC: negative is charging. Here: positive. */
		ma = (int)(-((int64_t)signed_raw * (subtype == 3 ? 152588 : 305176)) / 1000000);
	}
	percent = percent_at(mv);
	if (current_valid) {
		const int64_t full_uah = (int64_t)CAPACITY_MAH * 1000;
		int ocv = (int)mv - ma * RESISTANCE_MOHM / 1000;
		unsigned raw = percent_at(ocv > 0 ? (unsigned)ocv : 0);
		int64_t ocv_uah = full_uah * raw / 100, dt;
		uint64_t now;

		absolutetime_to_nanoseconds(mach_absolute_time(), &now);
		now /= 1000000ULL;
		dt = (int64_t)(now - count_ms);
		if (!counting || dt > 10 * 60 * 1000) {
			charge_uah = ocv_uah;
			counting = true;
		} else if (dt > 30 * 1000) {
			charge_uah -= 10000LL * dt / 3600000;   /* asleep: 10 mA */
		} else {
			charge_uah += (int64_t)ma * 1000 * dt / 3600000;
			if (ma > -50 && ma < 50) {
				charge_uah += (ocv_uah - charge_uah) / 64;
			}
		}
		if (full) {
			charge_uah = full_uah;
		}
		charge_uah = charge_uah < 0 ? 0 : (charge_uah > full_uah ? full_uah : charge_uah);
		count_ms = now;
		percent = smoothed((unsigned)((charge_uah * 100 + full_uah / 2) / full_uah), ma, now);
		b.charge_uah = (int32_t)charge_uah;
	}
	if (full) {
		percent = 100;
	}
	b.percent_hundredths = (int32_t)percent * 100;
	b.voltage_uv = (int32_t)mv * 1000;
	b.current_ua = ma * 1000;
	b.temperature_hundredths = 2500;        /* not measured: see above */
	b.present = 1;
	b.external = external;
	b.status = full ? 4 : charging ? 1 : external ? 3 : 2;  /* POWER_SUPPLY_STATUS_* */
	b.full_uah = CAPACITY_MAH * 1000;
	b.design_uah = CAPACITY_MAH * 1000;
	hakios_battery_report(&b);
	if (polls % 6 == 1) {
		hakios_say("pm6150_battery: about %u%%, %u mV, %d mA%s, port %s, charger state %u, %s (%u reads, %u failed)\n",
		    percent, mv, ma, current_valid ? "" : " (no current reading)", external ? "in use" : "free", state,
		    full ? "full" : charging ? "charging" : external ? "on the port, not charging" : "discharging",
		    polls, failures);
	}
}

static void
poll_again(void)
{
	uint64_t deadline;

	clock_interval_to_deadline(PERIOD_SECONDS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(poll_call, deadline);
}

static void
poll_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	if (arb_core == 0) {
		uint32_t version;

		arb_core = ml_io_map(ARB_CORE_PA, 0x2000);
		arb_cfg = ml_io_map(ARB_CFG_PA, 0x1000);
		arb_obs = ml_io_map(ARB_OBS_PA, 0x10000);
		version = rd(arb_core);
		arb_channels = rd(arb_core + 4) & 0x7ffu;
		if (version < 0x50000000u || version >= 0x70000000u || arb_channels == 0 || arb_channels > 512) {
			hakios_say("pm6150_battery: PMIC arbiter version %#x, %u channels: not one this driver knows\n", version,
			    arb_channels);
			return;
		}
		hakios_say("pm6150_battery: reading the PM6150 every %d s, read only; the temperature is not measured\n", PERIOD_SECONDS);
		/* a run before this one may have stopped while measuring: USB input back on */
		uint8_t cmd;

		if (read8(USBIN_CMD_IL, &cmd) && (cmd & USBIN_SUSPEND)) {
			hakios_say("pm6150_battery: USB input was left suspended: %s\n", usb_suspend(false) ? "on again" : "COULD NOT BE TURNED ON");
		}

	}
	poll_once();
	measure_check();
	{
		/* (at 90 s: what the kernel says before about a minute does not reach the run's log) */
		static bool dumped;
		uint32_t dump = 0;
		uint64_t now;

		absolutetime_to_nanoseconds(mach_absolute_time(), &now);
		if (!dumped && now / NSEC_PER_SEC >= 90 && PE_parse_boot_argn("pmic_dump", &dump, sizeof(dump)) && dump) {
			dumped = true;
			pmic_dump();
			gdsc_dump();
		}
	}
	poll_again();
}

void hakios_pm6150_battery_init(void);

void
hakios_pm6150_battery_init(void)
{
	uint32_t enabled = 0;

	PE_parse_boot_argn("bat_measure", &measure_at, sizeof(measure_at));
	PE_parse_boot_argn("bat_measure_s", &measure_for, sizeof(measure_for));
	measure_for = measure_for == 0 ? 60 : (measure_for > 120 ? 120 : measure_for);
	if (!PE_parse_boot_argn("pm6150_battery", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	poll_call = thread_call_allocate(poll_run, NULL);
	poll_again();
}
