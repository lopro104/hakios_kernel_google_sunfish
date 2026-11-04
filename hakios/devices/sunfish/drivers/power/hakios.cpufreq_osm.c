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
 * NOTHING HERE SLOWS A HOT CORE: the old tree had a governor doing that from 95 degrees, this tree has only the stop of
 * hakios.thermal.c at 105. So cpufreq=1 belongs with thermal=1, and is not in this device's default arguments.
 *
 * Also the command database's place for the memory vote: the firmware says where it is (the same locator as Linux's cmd-db.c through
 * aop_cmd_db, and bsd/dev/sweet_rpmh.c); the reference phone's fixed address is another memory here. Checked before use, RAM is never
 * scanned.
 *
 * Status: the raise and the vote ran on the phone from sweet_cpufreq.c and sweet_rpmh.c of ios-sunfish (A55 1804, A76 2208 MHz,
 * booted by ABL). THIS FILE: compiled only, not yet run.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
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

static uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

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
		printf("cpufreq: %s row %u: %u MHz, %u mV%s\n", osm_name[domain], row, khz / 1000, mv,
		    cores == LUT_TURBO_IND ? " (boost: not used)" : "");
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
		if (top < 0 || cores[d] == 0) {
			continue;
		}
		*(volatile uint32_t *)(base + OSM_PERF_STATE) = (uint32_t)top;
		delay_for_interval(20, NSEC_PER_MSEC);
		printf("cpufreq: %s, %u of our cores: row %u -> %d (%u MHz), now running %u MHz\n", osm_name[d], cores[d], was, top,
		    top_khz / 1000, XO_KHZ * (rd(base, OSM_CURRENT_VOTE) & 0x3ff) / 1000);
	}
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
