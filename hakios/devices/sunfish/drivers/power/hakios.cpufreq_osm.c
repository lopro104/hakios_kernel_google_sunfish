/*
 * The clock of the SM7150's cores (Pixel 4a) for drivers/power/hakios.cpufreq.c, whose own registers ("EPSS") are the reference
 * phone's. This SoC has the older "OSM" (Linux qcom-cpufreq-hw, the sdm845-style register set, sm7150.dtsi cpufreq@18323000): a table of
 * frequencies per group, built by the firmware, and hardware that switches clock and voltage when told which row is wanted.
 *   group 0, the six Cortex-A55 (cpu0-5) at 0x18323000; group 1, the two Cortex-A76 (cpu6-7) at 0x18325800
 *   0x000 enable: bit 0, the firmware runs this group
 *   0x110 + 32 n: row n: [31:30] clock source (0: 300 MHz, else the PLL), [18:16] core count (1: a boost row), [7:0] x 19.2 MHz
 *   0x114 + 32 n: its voltage, [11:0] in mV
 *   0x704: what runs now, [9:0] x 19.2 MHz      0x920: the row that is wanted
 *
 * Every group with a core of ours goes to its top row, boost rows left out as Linux leaves them out. Only with cpufreq=1 (the caller).
 * Hot cores: see the governor at the end of this file; beyond it, the stop of
 * hakios.thermal.c at 105 stays. cpufreq=1 is not in this device's default arguments.
 *
 * Also the command database's place for the memory vote: the firmware says where it is (the same locator as Linux's cmd-db.c through
 * aop_cmd_db, and bsd/dev/sweet_rpmh.c); the reference phone's fixed address is another memory here. Checked before use, RAM is never
 * scanned.
 *
 * Status: the raise and the vote ran on the phone from sweet_cpufreq.c and sweet_rpmh.c of ios-sunfish (A55 1804, A76 2208 MHz,
 * booted by ABL). THIS FILE: compiled only, not yet run.
 */
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <stdarg.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.thermal_sensors.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_cpufreq_osm_raise(void);
extern uint64_t hakios_cpufreq_osm_cmd_db(void);

#define OSM_DOMAINS             2
#define OSM_DOMAIN_SIZE         0x1400UL
#define OSM_ENABLE              0x000
#define OSM_FREQ_LUT            0x110
#define OSM_VOLT_LUT            0x114
#define OSM_LUT_ROW             32
#define OSM_LUT_ROWS            40
#define OSM_CURRENT_VOTE        0x704
#define OSM_PERF_STATE          0x920
#define LUT_TURBO_IND           1
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000

#define CMD_DB_LOCATOR_PAGE     0x0c3f0000UL
#define CMD_DB_LOCATOR          0xc             /* two words in that page: the address and the size */
#define CMD_DB_MAX              0x40000u

static const uint64_t osm_phys[OSM_DOMAINS] = { 0x18323000ULL, 0x18325800ULL };
static const char *const osm_name[OSM_DOMAINS] = { "A55 group (cpu0-5)", "A76 group (cpu6-7)" };

extern void loader_log_putc(char c);

/* Into the loader's log as well as the kernel's: the kernel's early and middle lines may be dropped from a long run's log. */
static void
say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[200];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("%s", line);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

static uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static uint32_t row_khz[OSM_DOMAINS][OSM_LUT_ROWS];
static vm_offset_t osm_base[OSM_DOMAINS];
static int top_row_of[OSM_DOMAINS] = { -1, -1 }, cap[OSM_DOMAINS];
static void governor_start(void);

/* The highest row that is not a boost row, or -1 when the firmware does not run this group. */
static int
top_row(vm_offset_t base, uint32_t domain, uint32_t *top_khz)
{
	uint32_t prev = 0;
	int top = -1;

	if (!(rd(base, OSM_ENABLE) & 1u)) {
		printf("cpufreq: %s: not enabled by the firmware (%#x); left alone\n", osm_name[domain], rd(base, OSM_ENABLE));
		return -1;
	}
	for (uint32_t row = 0; row < OSM_LUT_ROWS; row++) {
		uint32_t entry = rd(base, OSM_FREQ_LUT + OSM_LUT_ROW * row);
		uint32_t mv = rd(base, OSM_VOLT_LUT + OSM_LUT_ROW * row) & 0xfff;
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;
		uint32_t cores = (entry >> 16) & 7;

		if (row > 0 && khz == prev) {
			break;                  /* the table ends by repeating its last row */
		}
		say("cpufreq: %s row %u: %u MHz, %u mV%s\n", osm_name[domain], row, khz / 1000, mv,
		    cores == LUT_TURBO_IND ? " (boost: not used)" : "");
		row_khz[domain][row] = khz;
		if (cores != LUT_TURBO_IND && khz > *top_khz) {
			top = (int)row;
			*top_khz = khz;
		}
		prev = khz;
	}
	return top;
}

void
hakios_cpufreq_osm_raise(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t cores[OSM_DOMAINS] = { 0 };

	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		cores[((topology->cpus[i].phys_id >> 8) & 0xff) < 6 ? 0 : 1]++;     /* MPIDR Aff1 is the core's number */
	}
	for (uint32_t d = 0; d < OSM_DOMAINS; d++) {
		vm_offset_t base = ml_io_map(osm_phys[d], OSM_DOMAIN_SIZE);
		uint32_t was, top_khz = 0;
		int top;

		if (base == 0) {
			printf("cpufreq: could not map %s\n", osm_name[d]);
			continue;
		}
		top = top_row(base, d, &top_khz);
		was = rd(base, OSM_PERF_STATE);
		say("cpufreq: %s, as the boot loader left it: row %u, running %u MHz\n", osm_name[d], was,
		    XO_KHZ * (rd(base, OSM_CURRENT_VOTE) & 0x3ff) / 1000);
		if (top < 0 || cores[d] == 0) {
			continue;
		}
		*(volatile uint32_t *)(base + OSM_PERF_STATE) = (uint32_t)top;
		delay_for_interval(20, NSEC_PER_MSEC);
		say("cpufreq: %s, %u of our cores: row %u -> %d (%u MHz), now running %u MHz\n", osm_name[d], cores[d], was, top,
		    top_khz / 1000, XO_KHZ * (rd(base, OSM_CURRENT_VOTE) & 0x3ff) / 1000);
		osm_base[d] = base;
		top_row_of[d] = top;
		cap[d] = top;
	}
	governor_start();
}

/* Where the firmware left the command database, 0 if what it says cannot be right. */
uint64_t
hakios_cpufreq_osm_cmd_db(void)
{
	vm_offset_t page = ml_io_map(CMD_DB_LOCATOR_PAGE, 0x1000);
	uint32_t pa, size;

	if (page == 0) {
		return 0;
	}
	pa = rd(page, CMD_DB_LOCATOR);
	size = rd(page, CMD_DB_LOCATOR + 4);
	if (pa < 0x80000000u || (pa & 3u) || size < 0x1000u || size > CMD_DB_MAX || (uint64_t)pa + size > 0x100000000ULL) {
		printf("ddr: the firmware's locator of the command database makes no sense (%#x, %#x)\n", pa, size);
		return 0;
	}
	return pa;
}

/*
 * The governor: slows a group of cores when its sensors are hot, and speeds it up again. The rules are the old tree's
 * (bsd/sys/sweet_thermal.h of ios-sunfish, from HackiOS's hk_thermal.h), which ran on this phone. Every half second:
 *   at or past the limit      a row down; 5 degrees or more past it, three rows down
 *   5 degrees under the limit for 4 looks in a row    a row up, to the top row at the most
 * It reads the sensors itself (one load each, as hakios.thermal.c does), and writes only the row a group is to run at.
 * A55: cpu-0-*, cpuss-*. A76: cpu-1-*, and gpuss-*: nothing here sets the GPU's clock, so a hot GPU slows the big cores, which
 * lessens what it is asked to draw and what is near it; it does not slow the GPU itself. So a GPU heating alone is not governed:
 * the stop of hakios.thermal.c at 105 stays for that.
 * With no reading for 5 s a group goes to half its rows, and says so.
 * Boot-arg thermal_slow=<C> (40 to 110, default 95): the limit; thermal_slow=0: no governor.
 * Status: RUN ON THE PHONE (2026-11-05, logs/034326-run.log, thermal_slow=60): both groups stepped down as far as row 0 and 1 as
 * they passed 60 C, temperatures fell, and they stepped up again under 55 C. At the real limit, 95, not yet seen firing. The
 * "now running" number comes from OSM_CURRENT_VOTE, which read 2995 MHz whatever the row, so it says nothing here.
 */
#define TM_STATUS(n)            (0xa0 + 4 * (n))
#define TM_VALID                (1u << 21)
#define GOV_MS                  500
#define GOV_COOL_LOOKS          4
#define GOV_BLIND_LOOKS         10

extern const struct hakios_thermal_sensor hakios_thermal_sm7150[HAKIOS_THERMAL_SM7150_SENSORS];

static vm_offset_t gov_tm[2];
static thread_call_t gov_call;
static int gov_limit_tenths = 950, gov_cool[OSM_DOMAINS], gov_blind[OSM_DOMAINS];
static int gov_throttles, gov_limit_c = 95;
SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, thermal_throttles, CTLFLAG_RD | CTLFLAG_LOCKED, &gov_throttles, 0,
    "how many times a group of cores was slowed");

/* The hottest of a group's sensors, tenths of a degree; false if none has a reading. */
static bool
gov_hottest(uint32_t d, int *hottest, const char **name)
{
	bool any = false;

	for (unsigned i = 0; i < HAKIOS_THERMAL_SM7150_SENSORS; i++) {
		const struct hakios_thermal_sensor *z = &hakios_thermal_sm7150[i];
		bool mine = d == 0 ? (strncmp(z->name, "cpu-0-", 6) == 0 || strncmp(z->name, "cpuss", 5) == 0) :
		    (strncmp(z->name, "cpu-1-", 6) == 0 || strncmp(z->name, "gpuss", 5) == 0);
		uint32_t status;
		int value;

		if (!mine) {
			continue;
		}
		status = *(volatile uint32_t *)(gov_tm[z->tm == 0xc263000u ? 0 : 1] + TM_STATUS(z->number));
		value = (int32_t)(status << 20) >> 20;
		if ((status & TM_VALID) && value > -400 && value < 1600 && (!any || value > *hottest)) {
			*hottest = value;
			*name = z->name;
			any = true;
		}
	}
	return any;
}

static void
gov_look(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	for (uint32_t d = 0; d < OSM_DOMAINS; d++) {
		int hot = 0, want;
		const char *name = "?";

		if (top_row_of[d] < 0) {
			continue;
		}
		want = cap[d];
		if (!gov_hottest(d, &hot, &name)) {
			if (++gov_blind[d] == GOV_BLIND_LOOKS) {
				want = top_row_of[d] / 2;
				say("cpufreq: %s: no temperature for 5 s: to row %d\n", osm_name[d], want);
			}
		} else {
			gov_blind[d] = 0;
			if (hot >= gov_limit_tenths) {
				want = cap[d] - (hot >= gov_limit_tenths + 50 ? 3 : 1);
				gov_cool[d] = 0;
			} else if (hot < gov_limit_tenths - 50) {
				if (++gov_cool[d] >= GOV_COOL_LOOKS) {
					want = cap[d] + 1;
					gov_cool[d] = 0;
				}
			} else {
				gov_cool[d] = 0;
			}
		}
		want = want < 0 ? 0 : want > top_row_of[d] ? top_row_of[d] : want;
		if (want != cap[d]) {
			if (want < cap[d]) {
				gov_throttles++;
			}
			*(volatile uint32_t *)(osm_base[d] + OSM_PERF_STATE) = (uint32_t)want;
			say("cpufreq: %s: %s at %d.%d C (limit %d): row %d -> %d, %u MHz\n", osm_name[d], name, hot / 10, hot % 10,
			    gov_limit_c, cap[d], want, row_khz[d][want] / 1000);
			cap[d] = want;
		}
	}
	clock_interval_to_deadline(GOV_MS, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(gov_call, deadline);
}

static void
governor_start(void)
{
	uint32_t limit = 95;
	uint64_t deadline;

	if (PE_parse_boot_argn("thermal_slow", &limit, sizeof(limit)) && (limit == 0 || limit < 40 || limit > 110)) {
		say("cpufreq: no governor (thermal_slow=%u): hot cores are not slowed, only the stop at 105\n", limit);
		return;
	}
	gov_limit_c = (int)limit;
	gov_limit_tenths = (int)limit * 10;
	gov_tm[0] = ml_io_map(0xc263000, 0x1000);
	gov_tm[1] = ml_io_map(0xc265000, 0x1000);
	if (gov_tm[0] == 0 || gov_tm[1] == 0) {
		say("cpufreq: no governor: could not map the sensors\n");
		return;
	}
	say("cpufreq: governor: a group is slowed at %u C, and sped up again under %u C\n", limit, limit - 5);
	gov_call = thread_call_allocate(gov_look, NULL);
	clock_interval_to_deadline(GOV_MS, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(gov_call, deadline);
}
