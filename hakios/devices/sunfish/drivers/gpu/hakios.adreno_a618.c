/*
 * For HakiOS: bsd/dev/sweet_adreno.c of ios-sunfish, where all 21 steps ran on this phone started by ABL (2026-10-25). Here
 * with the small image's firmware and the calls HakiOS's kernel makes (at the end of this file). Boot-arg gpu=1 asks for what
 * ran there: adreno=21 adreno_keep=1 adreno_pause=0 adreno_zap_fresh=1 adreno_skip=4096 adreno_delay=0.
 * Status: RUN ON THE PHONE through U-Boot (2026-11-05, gpu=1, logs/021850-run.log, iOS 16.5): no panic; at the end of the run
 * the core was idle with its ring read to the end, the GMU answered slumber, TrustZone gave up the zap shader. Nothing has
 * drawn with it yet. The early lines of that log were dropped, so step by step it is not seen.
 *
 * The Adreno 618 of the SM7150 (sweet and sunfish), brought up step by step, from the first look at
 * its clocks to the 3D core writing a word into our memory on command. Nothing here is touched
 * unless boot-arg adreno=N asks for it.
 *
 * Ported from HackiOS (OOSEX GANG, https://github.com/oosexgang/hackios-xnu-kernel,
 * xnu/files/bsd/dev/redwood_adreno.c, APSL 2.0), which brings an Adreno 642L on the SM7325 up
 * to drawing frames this way. The step structure, the logging discipline and the measuring of
 * clocks are theirs; every address and selector below was re-derived for SM7150 (see "Sources").
 *
 * How much of the GPU block a kernel under Qualcomm's hypervisor may touch is found out on the
 * phone, one step a boot: an access the hypervisor does not like can end the run at once or
 * seconds later, often without a word. So adreno=N performs steps 1 to N and stops; a step says
 * what it is about to touch before it touches it and what it found after, reads come before
 * writes, and a step that does not find what it needs ends the sequence instead of going on blind.
 *
 *   1  reads   the GPU's clocks in the global clock controller, and RPMh's names for its rails
 *   2  reads   the fuses that say which speed bin of the GPU this is
 *   3  reads   the GPU's own clock controller as the boot loader left it
 *   4  writes  the "CX" power switch on (GMU, bus interface, the GPU's SMMU)
 *   5  writes  the clocks of that half on (GMU 200 MHz from GPLL0's 300 MHz tap), and measures
 *              them with the SoC's frequency counter
 *   6  reads   the first words of the GPU block itself: the GMU (its always-on counter must
 *              count at 19.2 MHz) and the bus interface
 *   7  reads   the GMU's RPMh channel (RSCC)
 *   8  reads   the GPU SMMU's ID registers and stream table
 *   9  reads   that SMMU's translation context 0
 *  10  writes  two words into the GMU's data memory and reads them back (puts them back after)
 *  11  asks    TrustZone questions that pass no memory: does it take calls from this kernel
 *  12  asks    TrustZone to check the signature of the zap shader (the signed image without which
 *              the 3D core stays in secure mode): a615_zap.mdt
 *  13  writes  page tables for the GMU's memory, and the SMMU: context 1 with them, stream 5 through it
 *  14  writes  the GMU's firmware (a618_gmu.bin) into its code memory
 *  15  writes  the programs of the GMU's RPMh channel and of the always-on sequencer (PDC), the
 *              GMU's configuration; lets its processor out of reset
 *  16  talks   to the GMU: "boot" with the 3D core's rail level, the shader processors' power, its
 *              message queues, the tables of speeds and bus votes
 *  17  asks    the GMU to power the 3D core and for a speed; reads the core's registers
 *  18  writes  the 3D core's memory (ring, microcode a630_sqe.fw) and SMMU context 0, stream 0
 *  19  asks    TrustZone to load the zap shader into its reserved memory (0x98f95000)
 *  20  writes  the 3D core's registers; starts its command processor with "initialize"
 *  21  commands the core out of secure mode and to write a word into a page of ours
 * At the end, what the steps switched on is switched off again in reverse (the GMU slumbers and
 * is held in reset, the zap shader is given up), so the phone is as ABL left it and the system
 * goes on starting. Steps 12 on read the phone's GPU firmware from /fw/adreno/ of the iOS root
 * (docs/GPU.md).
 *
 * Boot-args:
 *   adreno=N           steps 1..N (0 or absent: the GPU is left alone)
 *   adreno_only=K      step K alone
 *   adreno_skip=MASK   leave out the steps whose bits are set (bit 2 = step 2)
 *   adreno_pause=MS    how long the newest step waits after announcing a group of accesses, so
 *                      that the line reaches the persistent / USB log before a possible reset
 *                      (default 1500)
 *   adreno_fine=1      the newest step waits like that before every single access
 *   adreno_delay=S     start S seconds after the root is mounted (default 40), after the GUI
 *   adreno_keep=1      do not switch off again what the steps switched on; after all 21, the GPU
 *                      stays on for the system (the service HakiosGPU: "the GPU kept on" below)
 *   adreno_speed=N     step 16/17: the 3D core's speed, 1 180 MHz, 2 267 MHz, 3 355 MHz, 4 430 MHz,
 *                      5 565 MHz (default)
 *   adreno_zap=0       step 19 loads no zap shader (step 12 can be skipped too: adreno_skip=0x1800)
 *
 * Sources (all in the repositories of this port):
 *   addresses   linux sm7150.dtsi (gpu@5000000, gmu@506a000, iommu@5040000, clock-controller@5090000,
 *               efuse@784000, aop-cmd-db@85f20000) and msm-4.14 sdmmagpie-gpu.dtsi / sdmmagpie-gdsc.dtsi
 *   GCC         linux gcc-sm7150.c, msm-4.14 gcc-sdmmagpie.c
 *   GPU CC      msm-4.14 gpucc-sdmmagpie.c (mainline: gpucc-sc7180.c, which SM7150 is compatible with)
 *   counter     msm-4.14 debugcc-sdmmagpie.c, clk-debug.c
 *   GMU / RSCC  linux a6xx_gmu.c (A618: RSCC at GMU + 0x23000), registers/adreno/a6xx_gmu.xml
 *   SMMU        linux arm-smmu.h, io-pgtable-arm.c
 *   steps 11-21 the vendor's driver for this GPU and firmware (msm-4.14 drivers/gpu/msm:
 *               adreno_a6xx_gmu.c, kgsl_gmu.c, kgsl_hfi.c; sdmmagpie-gpu.dtsi has
 *               qcom,gpu-quirk-hfi-use-reg), checked against linux's "legacy" GMU path
 *               (a6xx_gmu.c, a6xx_hfi.c, a6xx_gpu.c hw_init); the structure, the SMMU and
 *               TrustZone code and the ring are HackiOS's (redwood_adreno.c steps 11-21)
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
int hakios_rpmh_crypto_vote(uint32_t vote);
int hakios_rpmh_ddr_vote(uint32_t vote, bool send);
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
/* Mach MIG declaration is not included by BSD's exported thread.h. */
extern kern_return_t thread_terminate(thread_t);
void hakios_adreno_init(void);

static uint32_t last_step, skip_steps, pause_ms = 1500, fine, start_delay_s = 40, keep;
static uint32_t speed_bin;              /* which of the GPUs of this kind the fuses say this is (step 2) */
static uint32_t step;                   /* the one being performed */
static uint32_t newest_step;            /* the highest one asked for: the one not yet seen to work */
static char last_write[96] = "nothing written";
static bool quiet;                      /* repeated accesses that are known to work: say nothing */

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- register access ------------------------------------------------------------------------------- */

/*
 * One access is one plain load or store of a general register: some of what is touched (the SMMU's
 * global registers at least) is the hypervisor's imitation of the hardware, which decodes the
 * trapped instruction and copes with the simplest forms only.
 */
static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static uint32_t __attribute__((noinline))
rd8(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldrb %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

/* A stretch of registers; mapped when a step first needs it. */
struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the log to take what has just been said, where it is not yet known to be harmless. */
static void
hold(bool single_access)
{
	if (step == newest_step && pause_ms && (!single_access || fine)) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("adreno: step %u: %s\n", step, what);
	hold(false);
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (quiet) {
		return rd(b->base + offset);
	}
	if (step == newest_step) {
		printf("adreno: %u: about to read %s %s at %#x\n", step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd(b->base + offset);
	printf("adreno: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static uint32_t
get8(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (step == newest_step) {
		printf("adreno: %u: about to read %s %s at %#x (a byte)\n", step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd8(b->base + offset);
	printf("adreno: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	if (!quiet) {
		printf("adreno: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
		snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name,
		    b->phys + offset);
		hold(true);
	}
	wr(b->base + offset, value);
}

/* Set bits of a register unless they are set. Returns what the register holds afterwards. */
static uint32_t
set(struct block *b, const char *name, uint32_t offset, uint32_t bits)
{
	uint32_t was = get(b, name, offset);

	if ((was & bits) == bits) {
		return was;
	}
	put(b, name, offset, was | bits);
	return get(b, name, offset);
}

static bool
wait_for(struct block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd(b->base + offset) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return (rd(b->base + offset) & mask) == value;
}

/* What the steps switched on, to be switched off again at the end (unless adreno_keep=1). */
#define UNDO_MAX                16
static struct {
	struct block *b;
	const char *name;
	uint32_t offset, bits;
} undo[UNDO_MAX];
static uint32_t undo_count;
static bool cx_switched_on;

static void
remember(struct block *b, const char *name, uint32_t offset, uint32_t bits)
{
	if (undo_count < UNDO_MAX) {
		undo[undo_count].b = b;
		undo[undo_count].name = name;
		undo[undo_count].offset = offset;
		undo[undo_count].bits = bits;
		undo_count++;
	}
}

/* ---- step 1: the clocks from outside, and RPMh's names ------------------------------------------------ */

/*
 * dt: clock-controller@100000 (qcom,sm7150-gcc). gcc_gpu_cfg_ahb is the clock of the GPU clock
 * controller's registers (Linux keeps it on for good: gcc-sm7150.c probe); memnoc_gfx and
 * snoc_dvm_gfx are the GPU's two bus connections, ddrss_gpu_axi its memory; GPLL0's main and
 * divided (300 MHz) taps for the GPU are votes in 0x52004 (gcc-sm7150.c: bits 15 and 16).
 */
static struct block gcc = { "gcc", 0x00100000, 0x80000 };
#define GCC_PLL_VOTE            0x52004
#define GCC_PLL_VOTE_GPU_DIV    (1u << 16)      /* gcc_gpu_gpll0_div_clk_src */
#define GCC_GPU_CFG_AHB_CBCR    0x71004
#define GCC_GPU_MEMNOC_GFX_CBCR 0x7100c
#define GCC_GPU_SNOC_DVM_GFX_CBCR 0x71018
#define GCC_DDRSS_GPU_AXI_CBCR  0x4452c
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)

/* dt: reserved-memory aop-cmd-db@85f20000: the boot loader's list of what RPMh can switch (cmd-db.c) */
#define CMD_DB_PHYS             0x85f20000UL
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

/* Where RPMh has a resource (0: nowhere) and, for a rail, the levels it knows. */
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned *level_count)
{
	const struct cmd_db_header *db = cmd_db;
	char id[8] = { 0 };

	*level_count = 0;
	if (db == NULL) {
		db = cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);  /* memory, not registers */
	}
	if (db == NULL || db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512 ||
		    (unsigned)slave->header_offset + slave->count * sizeof(*entry) > CMD_DB_SIZE - 0x1000) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data = db->data + slave->data_offset + entry[i].offset;
			unsigned count = entry[i].length / 2u;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				count = 0;
			}
			for (unsigned l = 0; l < count && l < 16; l++) {
				levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
				if (l > 0 && levels[l] == 0) {
					break;
				}
				*level_count = l + 1;
			}
			return entry[i].address;
		}
	}
	return 0;
}

static void
show_rpmh_resource(const char *name)
{
	uint16_t level[16];
	unsigned count = 0;
	uint32_t address = rpmh_resource(name, level, &count);
	char levels[160];
	int at = 0;

	if (address == 0) {
		printf("adreno: %u: RPMh has no \"%s\" (or no cmd-db at %#lx)\n", step, name, CMD_DB_PHYS);
		return;
	}
	levels[0] = 0;
	for (unsigned l = 0; l < count && at < (int)sizeof(levels) - 8; l++) {
		at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level[l]);
	}
	printf("adreno: %u: RPMh has \"%s\" at %#x; levels:%s\n", step, name, address, levels);
}

static bool
step_outside(void)
{
	announce("read the GPU's clocks in the global clock controller (the touch driver uses this controller too)");
	get(&gcc, "pll votes", GCC_PLL_VOTE);
	get(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
	/*
	 * Not the GPU's bus clocks (gpu_memnoc_gfx and the others): on a Poco X3 NFC, the same SoC,
	 * the first read of gpu_memnoc_gfx (0x17100c) made this early stopped the phone; turned on in
	 * the device tree's order after gpu_cfg_ahb, as step 5 does, it reads (ios-surya NOTES.md).
	 */

	announce("look up the GPU's rails in the boot loader's list of RPMh resources (memory at 0x85f20000)");
	show_rpmh_resource("gfx.lvl");
	show_rpmh_resource("cx.lvl");
	show_rpmh_resource("mx.lvl");
	return true;
}

/* ---- step 2: the fuses ------------------------------------------------------------------------------- */

/*
 * dt: efuse@784000 (the error-corrected copy) with gpu-speed-bin@1a2, 8 bits from bit 5; the
 * vendor's tree says the same as qcom,gpu-speed-bin = <0x41a0 0x1fe00000 21> from 0x780000.
 * opp-supported-hw in sm7150.dtsi: bins 0 (825 MHz), 1 (610), 2 (700), 3 (800), 4 (650 max).
 */
static struct block qfprom = { "qfprom", 0x00780000, 0x5000 };
#define QFPROM_CORRECTED        0x4000
#define QFPROM_GPU_SPEED_BIN    0x1a2

static bool
step_fuses(void)
{
	uint32_t bin;

	announce("read the GPU's speed bin in the error-corrected copy of the fuses (0x784000), which Linux reads");
	bin = get8(&qfprom, "speed bin, low", QFPROM_CORRECTED + QFPROM_GPU_SPEED_BIN);
	bin |= get8(&qfprom, "speed bin, high", QFPROM_CORRECTED + QFPROM_GPU_SPEED_BIN + 1) << 8;
	bin = bin >> 5 & 0xff;
	speed_bin = bin;
	printf("adreno: %u: speed bin %u\n", step, bin);
	return true;
}

/* ---- step 3: the GPU's clock controller ----------------------------------------------------------------- */

/*
 * dt: clock-controller@5090000 (qcom,sm7150-gpucc, qcom,sc7180-gpucc), 0x9000 long; offsets:
 * gpucc-sdmmagpie.c, power switches from sdmmagpie-gdsc.dtsi. Its registers answer only while
 * gcc_gpu_cfg_ahb runs. The A618 has no "hub" clocks (those are A660's).
 */
static struct block gpucc = { "gpucc", 0x05090000, 0x9000 };
#define GPUCC_PLL0_MODE         0x0000
#define GPUCC_PLL1_MODE         0x0100
#define GPUCC_CXO_AON_CBCR      0x1004
#define GPUCC_GX_SW_RESET       0x1008          /* dt: gpu_gx_sw_reset */
#define GPUCC_GX_GDSCR          0x100c          /* dt: gpu_gx_gdsc, the 3D core's power: the GMU's to operate */
#define GPUCC_GX_GFX3D_RCGR     0x101c
#define GPUCC_GX_GMU_CBCR       0x1064
#define GPUCC_CX_GDSCR          0x106c          /* dt: gpu_cx_gdsc */
#define GPUCC_AHB_CBCR          0x1078          /* gpu_cc_ahb_clk: the register bus inside the GPU block */
#define GPUCC_CX_APB_CBCR       0x1088
#define GPUCC_CX_SNOC_DVM_CBCR  0x108c
#define GPUCC_CX_GMU_CBCR       0x1098          /* gpu_cc_cx_gmu_clk: the GMU */
#define GPUCC_CXO_CBCR          0x109c          /* gpu_cc_cxo_clk: the crystal, for the GMU's timers */
#define GPUCC_GMU_RCGR          0x1120          /* gpu_cc_gmu_clk_src: +0 command, +4 source and divider */
#define GPUCC_GX_DOMAIN_MISC    0x1508          /* dt: gpu_gx_domain_addr */
#define GPUCC_CX_HW_CTRL        0x1540          /* dt: gpu_cx_hw_ctrl */

#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_HW_CONTROL         (1u << 1)
#define GDSC_POWER_ON           (1u << 31)

static bool
step_gpucc(void)
{
	announce("make sure the clock of the GPU clock controller's registers runs (gcc_gpu_cfg_ahb)");
	if ((set(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR, CBCR_ENABLE) & CBCR_OFF) &&
	    !wait_for(&gcc, GCC_GPU_CFG_AHB_CBCR, CBCR_OFF, 0, 10)) {
		printf("adreno: %u: it stays off (%#x): the GPU's clock controller would not answer\n", step,
		    rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
		return false;
	}

	announce("read the GPU's clock controller (0x5090000) as it is: the first access to the GPU's part of the SoC");
	get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET);
	get(&gpucc, "gx_domain_misc", GPUCC_GX_DOMAIN_MISC);
	get(&gpucc, "pll0 mode", GPUCC_PLL0_MODE);
	get(&gpucc, "pll0 multiplier", GPUCC_PLL0_MODE + 4);
	get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	get(&gpucc, "pll1 multiplier", GPUCC_PLL1_MODE + 4);
	get(&gpucc, "gmu_clk_src command", GPUCC_GMU_RCGR);
	get(&gpucc, "gmu_clk_src source", GPUCC_GMU_RCGR + 4);
	get(&gpucc, "gx_gfx3d_clk_src command", GPUCC_GX_GFX3D_RCGR);
	get(&gpucc, "cxo_aon", GPUCC_CXO_AON_CBCR);
	get(&gpucc, "gx_gmu", GPUCC_GX_GMU_CBCR);
	get(&gpucc, "ahb", GPUCC_AHB_CBCR);
	get(&gpucc, "cx_apb", GPUCC_CX_APB_CBCR);
	get(&gpucc, "cx_snoc_dvm", GPUCC_CX_SNOC_DVM_CBCR);
	get(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR);
	get(&gpucc, "cxo", GPUCC_CXO_CBCR);
	return true;
}

/* ---- step 4: CX power --------------------------------------------------------------------------------- */

static bool
gpucc_answers(void)
{
	need(&gcc);
	need(&gpucc);
	if (rd(gcc.base + GCC_GPU_CFG_AHB_CBCR) & CBCR_OFF) {
		printf("adreno: %u: the GPU clock controller's registers have no clock (gcc gpu_cfg_ahb %#x): step 3\n", step,
		    rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
		return false;
	}
	return true;
}

static bool
cx_is_on(void)
{
	return (rd(gpucc.base + GPUCC_CX_GDSCR) | rd(gpucc.base + GPUCC_CX_HW_CTRL)) & GDSC_POWER_ON;
}

/*
 * The vendor's driver (gdsc-regulator.c) clears the collapse bit and waits for bit 31 of the
 * hw-ctrl-addr register (0x5091540); Linux waits for bit 31 of the control register itself.
 * Either counts here.
 */
static bool
step_cx_power(void)
{
	uint32_t gdscr;

	if (!gpucc_answers()) {
		return false;
	}
	announce("switch the CX half of the GPU block on (gpu_cx_gdsc): clear the collapse bit of its power switch");
	gdscr = get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	if (cx_is_on()) {
		printf("adreno: %u: CX is on already (ABL or the hardware keeps it): going on with it as it is\n", step);
		return true;
	}
	if (gdscr & GDSC_HW_CONTROL) {
		printf("adreno: %u: the switch is under hardware control: not ours to operate\n", step);
		return false;
	}
	put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
	for (int i = 0; i < 500 && !cx_is_on(); i++) {
		sleep_us(100);
	}
	get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	if (!cx_is_on()) {
		printf("adreno: %u: CX does not come on\n", step);
		return false;
	}
	cx_switched_on = true;
	printf("adreno: %u: CX is on\n", step);
	if (rd(gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON) {
		printf("adreno: %u: the 3D core (GX) has power already: unexpected after ABL, not going further\n", step);
		return false;
	}
	return true;
}

/* ---- step 5: clocks ------------------------------------------------------------------------------------ */

#define RCG_UPDATE              (1u << 0)
#define RCG_SOURCE_MASK         0x371fu         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define RCG_GPLL0_DIV(twice)    (6u << 8 | ((twice) - 1))       /* source 6: GPLL0's 300 MHz tap (gpu_cc_parent_map_0) */

/* Turn a clock on and see it run. Not every clock's status bit can be believed: `must` says which. */
static bool
clock_on(struct block *b, const char *name, uint32_t cbcr, bool must)
{
	uint32_t was = get(b, name, cbcr);

	if (!(was & CBCR_ENABLE)) {
		remember(b, name, cbcr, CBCR_ENABLE);
	}
	if (!(set(b, name, cbcr, CBCR_ENABLE) & CBCR_OFF) || wait_for(b, cbcr, CBCR_OFF, 0, 10)) {
		return true;
	}
	printf("adreno: %u: %s %s stays off (%#x)%s\n", step, b->name, name, rd(b->base + cbcr),
	    must ? "" : "; its status bit is not one to rely on, going on");
	return !must;
}

static bool
clock_source(const char *name, uint32_t rcgr, uint32_t source)
{
	uint32_t was = get(&gpucc, name, rcgr + 4);

	if ((was & RCG_SOURCE_MASK) == source) {
		return true;
	}
	put(&gpucc, name, rcgr + 4, (was & ~RCG_SOURCE_MASK) | source);
	put(&gpucc, name, rcgr, rd(gpucc.base + rcgr) | RCG_UPDATE);
	if (!wait_for(&gpucc, rcgr, RCG_UPDATE, 0, 10)) {
		printf("adreno: %u: %s does not take its new source (command %#x)\n", step, name, rd(gpucc.base + rcgr));
		return false;
	}
	get(&gpucc, name, rcgr + 4);
	return true;
}

/*
 * The GPU SMMU's clock vote: gpu_cc_hlos1_vote_gpu_smmu_clk, 0x5000 bit 0 of the GPU clock
 * controller (msm-4.14 gpucc-sm6150.c; sunfish's gpucc-sdmmagpie.c leaves it out). Without it
 * the SMMU's registers have no clock and the first read of one stalls the bus for good: found
 * on a Poco X3 NFC, the same SoC (ios-surya, NOTES.md "GPUCC HLOS1 SMMU clock vote"), where
 * setting it made the SMMU answer. Steps 8 on check it before they touch the SMMU.
 */
#define GPUCC_HLOS1_VOTE_GPU_SMMU 0x5000

static bool
smmu_vote(void)
{
	uint32_t vote = get(&gpucc, "hlos1_vote_gpu_smmu", GPUCC_HLOS1_VOTE_GPU_SMMU);

	if (!(vote & CBCR_ENABLE)) {
		remember(&gpucc, "hlos1_vote_gpu_smmu", GPUCC_HLOS1_VOTE_GPU_SMMU, CBCR_ENABLE);
		vote = set(&gpucc, "hlos1_vote_gpu_smmu", GPUCC_HLOS1_VOTE_GPU_SMMU, CBCR_ENABLE);
	}
	if (!(vote & CBCR_ENABLE)) {
		printf("adreno: %u: the SMMU's clock vote does not hold (%#x): the SMMU is not to be touched\n", step, vote);
		return false;
	}
	return true;
}

/* Before a step touches the SMMU: its clock vote is in. */
static bool
smmu_clocked(void)
{
	if (!(rd(gpucc.base + GPUCC_HLOS1_VOTE_GPU_SMMU) & CBCR_ENABLE)) {
		printf("adreno: %u: the SMMU's clock vote (gpucc 0x5000) is not set: step 5 first\n", step);
		return false;
	}
	return true;
}

/*
 * The SoC's frequency counter (vendor-only: debugcc-sdmmagpie.c, clk-debug.c): the GPU clock
 * controller puts one of its clocks on its debug output (mux 0x1568, divider 0x10fc /2, enable
 * 0x1100), the global controller takes that as its input 0x144 (mux 0x62008, divider 0x62000 /4,
 * enable 0x62004) and counts it against the crystal / 4 (enable 0x43008, control 0x62024,
 * status 0x62028). Rate = count formula times the two dividers (8).
 */
#define GCC_DEBUG_MUX           0x62008
#define GCC_DEBUG_MUX_GPUCC     0x144
#define GCC_DEBUG_DIV           0x62000
#define GCC_DEBUG_DIV_GPUCC     4
#define GCC_DEBUG_CBCR          0x62004
#define GCC_XO_DIV4_CBCR        0x43008
#define GCC_MEASURE_CTL         0x62024
#define MEASURE_RUN             (1u << 20)
#define GCC_MEASURE_STATUS      0x62028
#define MEASURE_DONE            (1u << 25)
#define MEASURE_BRIEF           0x1000          /* ticks of 4.8 MHz: ~1 ms */
#define MEASURE_FULL            0x10000         /* ~14 ms */
#define MEASURE_FAILED          0xffffffffu
#define GPUCC_DEBUG_DIV         0x10fc
#define GPUCC_DEBUG_DIV_VAL     2
#define GPUCC_DEBUG_CBCR        0x1100
#define GPUCC_DEBUG_MUX         0x1568
#define GPUCC_DEBUG_CXO         0x0a
#define GPUCC_DEBUG_CXO_AON     0x0b
#define GPUCC_DEBUG_GX_GFX3D    0x0c
#define GPUCC_DEBUG_GX_GMU      0x10
#define GPUCC_DEBUG_AHB         0x11
#define GPUCC_DEBUG_CX_APB      0x15
#define GPUCC_DEBUG_CX_SNOC_DVM 0x16
#define GPUCC_DEBUG_CX_GMU      0x19

/* clk-debug.c run_measurement(): stop with the tick count set, wait idle, run, wait done. */
static uint32_t
count_clock(uint32_t ticks)
{
	uint32_t status = 0;

	put(&gcc, "measure control", GCC_MEASURE_CTL, ticks);
	for (int i = 0; i < 2000 && (rd(gcc.base + GCC_MEASURE_STATUS) & MEASURE_DONE); i++) {
		sleep_us(10);
	}
	put(&gcc, "measure control", GCC_MEASURE_CTL, ticks | MEASURE_RUN);
	for (int i = 0; i < 2000 && !(status & MEASURE_DONE); i++) {
		sleep_us(100);
		status = rd(gcc.base + GCC_MEASURE_STATUS);
	}
	put(&gcc, "measure control", GCC_MEASURE_CTL, ticks);
	return status & MEASURE_DONE ? status & 0x1ffffff : MEASURE_FAILED;
}

/* Rate in Hz of one of the GPU clock controller's clocks; 0 if it does not run. Everything changed is put back. */
static uint32_t
measure(uint32_t select)
{
	uint32_t gpu_mux = get(&gpucc, "debug mux", GPUCC_DEBUG_MUX), gpu_div = get(&gpucc, "debug divider", GPUCC_DEBUG_DIV);
	uint32_t gpu_cbcr = get(&gpucc, "debug output", GPUCC_DEBUG_CBCR), mux = get(&gcc, "debug mux", GCC_DEBUG_MUX);
	uint32_t div = get(&gcc, "debug divider", GCC_DEBUG_DIV), cbcr = get(&gcc, "debug input", GCC_DEBUG_CBCR);
	uint32_t base = get(&gcc, "crystal/4", GCC_XO_DIV4_CBCR), control = get(&gcc, "measure control", GCC_MEASURE_CTL);
	uint32_t brief, full;

	put(&gpucc, "debug mux", GPUCC_DEBUG_MUX, (gpu_mux & ~0xffu) | select);
	put(&gpucc, "debug divider", GPUCC_DEBUG_DIV, (gpu_div & ~0x3u) | (GPUCC_DEBUG_DIV_VAL - 1));
	put(&gpucc, "debug output", GPUCC_DEBUG_CBCR, gpu_cbcr | CBCR_ENABLE);
	put(&gcc, "debug mux", GCC_DEBUG_MUX, (mux & ~0x3ffu) | GCC_DEBUG_MUX_GPUCC);
	put(&gcc, "debug divider", GCC_DEBUG_DIV, (div & ~0xfu) | (GCC_DEBUG_DIV_GPUCC - 1));
	put(&gcc, "debug input", GCC_DEBUG_CBCR, cbcr | CBCR_ENABLE);
	put(&gcc, "crystal/4", GCC_XO_DIV4_CBCR, base | CBCR_ENABLE);
	/* a counter whose input stands still keeps its last count: the two counts differ only if the clock runs */
	brief = count_clock(MEASURE_BRIEF);
	full = brief == MEASURE_FAILED ? MEASURE_FAILED : count_clock(MEASURE_FULL);
	put(&gcc, "measure control", GCC_MEASURE_CTL, control);
	put(&gcc, "crystal/4", GCC_XO_DIV4_CBCR, base);
	put(&gcc, "debug input", GCC_DEBUG_CBCR, cbcr);
	put(&gcc, "debug divider", GCC_DEBUG_DIV, div);
	put(&gcc, "debug mux", GCC_DEBUG_MUX, mux);
	put(&gpucc, "debug output", GPUCC_DEBUG_CBCR, gpu_cbcr);
	put(&gpucc, "debug divider", GPUCC_DEBUG_DIV, gpu_div);
	put(&gpucc, "debug mux", GPUCC_DEBUG_MUX, gpu_mux);

	if (full == MEASURE_FAILED) {
		return MEASURE_FAILED;
	}
	if (full == brief) {
		return 0;
	}
	return (uint32_t)(((uint64_t)full * 10 + 15) * 4800000 / ((uint64_t)MEASURE_FULL * 10 + 35) *
	       (GCC_DEBUG_DIV_GPUCC * GPUCC_DEBUG_DIV_VAL));
}

static uint32_t
measure_quietly(uint32_t select)
{
	uint32_t hz;

	quiet = true;
	hz = measure(select);
	quiet = false;
	return hz;
}

/* The crystal first and aloud: its rate is known, so it shows whether the counter can be believed. */
static bool
measure_clocks(void)
{
	static const struct {
		const char *name;
		uint32_t select;
	} clocks[] = {
		{ "cx_gmu (200 MHz was asked for)", GPUCC_DEBUG_CX_GMU }, { "cxo_aon", GPUCC_DEBUG_CXO_AON },
		{ "cx_apb", GPUCC_DEBUG_CX_APB }, { "cx_snoc_dvm", GPUCC_DEBUG_CX_SNOC_DVM },
		{ "gx_gmu (should be off)", GPUCC_DEBUG_GX_GMU }, { "gx_gfx3d (should be off)", GPUCC_DEBUG_GX_GFX3D },
		{ "ahb, the register bus of the GPU block,", GPUCC_DEBUG_AHB },
	};
	uint32_t hz, ahb_hz = MEASURE_FAILED;

	announce("measure the GPU block's clocks with the SoC's frequency counter (gcc 0x162000.., 0x143008; "
	    "gpucc 0x50910fc, 0x5091100, 0x5091568)");
	hz = measure(GPUCC_DEBUG_CXO);
	if (hz == MEASURE_FAILED) {
		printf("adreno: %u: the counter did not finish: it cannot be used as it is set up here\n", step);
		return false;
	}
	printf("adreno: %u: measured: cxo runs at %u Hz (the crystal: 19200000)\n", step, hz);
	if (hz < 18600000 || hz > 19800000) {
		printf("adreno: %u: the counter does not show the crystal's rate: nothing it says can be used\n", step);
		return false;
	}
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		hz = measure_quietly(clocks[i].select);
		if (hz == MEASURE_FAILED) {
			printf("adreno: %u: measured: %s: the counter did not finish\n", step, clocks[i].name);
		} else {
			printf("adreno: %u: measured: %s runs at %u Hz\n", step, clocks[i].name, hz);
		}
		if (clocks[i].select == GPUCC_DEBUG_AHB) {
			ahb_hz = hz;
		}
	}
	if (ahb_hz != MEASURE_FAILED && ahb_hz >= 1000000) {
		printf("adreno: %u: THE REGISTER BUS OF THE GPU BLOCK HAS ITS CLOCK\n", step);
		return true;
	}
	printf("adreno: %u: the register bus of the GPU block has no clock\n", step);
	return false;
}

/*
 * The clocks the device tree gives the GMU (gmu, cxo, axi, memnoc) and the SMMU (ahb, bus,
 * iface), at the rate Linux brings the GMU up with: 200 MHz from GPLL0's 300 MHz tap / 1.5
 * (gpucc-sdmmagpie.c ftbl_gpu_cc_gmu_clk_src). No PLL of the GPU's own is started.
 */
static bool
step_clocks(void)
{
	if (!gpucc_answers()) {
		return false;
	}
	if (!cx_is_on()) {
		printf("adreno: %u: CX is off: step 4\n", step);
		return false;
	}
	announce("in the global clock controller: vote for GPLL0's 300 MHz tap for the GPU; the GPU's bus and memory clocks on");
	if (!(get(&gcc, "pll votes", GCC_PLL_VOTE) & GCC_PLL_VOTE_GPU_DIV)) {
		remember(&gcc, "pll votes", GCC_PLL_VOTE, GCC_PLL_VOTE_GPU_DIV);
	}
	set(&gcc, "pll votes", GCC_PLL_VOTE, GCC_PLL_VOTE_GPU_DIV);
	/* sdmmagpie-gpu.dtsi kgsl-iommu clocks, in their order (iface gpu_cfg_ahb, step 3; mem; mem_iface), as on ios-surya */
	if (!clock_on(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR, false) ||
	    !clock_on(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR, false) ||
	    !clock_on(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR, false)) {
		return false;
	}

	announce("in the GPU's clock controller: the GMU's clock to 200 MHz");
	if (!clock_source("gmu_clk_src", GPUCC_GMU_RCGR, RCG_GPLL0_DIV(3))) {   /* 300 MHz / 1.5 */
		return false;
	}

	announce("in the GPU's clock controller: the clocks of the GMU and of the SMMU on, and the SMMU's clock vote");
	if (!clock_on(&gpucc, "cxo", GPUCC_CXO_CBCR, true) ||
	    !clock_on(&gpucc, "cxo_aon", GPUCC_CXO_AON_CBCR, false) ||
	    !clock_on(&gpucc, "ahb", GPUCC_AHB_CBCR, false)) {
		return false;
	}
	/* the GMU's clock with its wake and sleep counters at their most, as ios-surya brings it up */
	put(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, (rd(gpucc.base + GPUCC_CX_GMU_CBCR) & ~0xff0u) | 0xff0u);
	if (!clock_on(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, true) || !smmu_vote()) {
		return false;
	}
	return measure_clocks();
}

/* What every later step stands on: CX powered, the GMU's clock running, the register bus's clock running. */
static bool
cx_ready(void)
{
	uint32_t gmu_clock, ahb_hz;

	if (!gpucc_answers()) {
		return false;
	}
	gmu_clock = rd(gpucc.base + GPUCC_CX_GMU_CBCR);
	if (!cx_is_on() || (gmu_clock & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE) {
		printf("adreno: %u: needs CX on (it is %s) and the GMU's clock running (cx_gmu %#x): steps 4 and 5\n", step,
		    cx_is_on() ? "on" : "off", gmu_clock);
		return false;
	}
	ahb_hz = measure_quietly(GPUCC_DEBUG_AHB);
	if (ahb_hz == MEASURE_FAILED || ahb_hz < 1000000) {
		printf("adreno: %u: the GPU block's register bus clock measures %u: not touching the block\n", step, ahb_hz);
		return false;
	}
	return true;
}

/* ---- step 6: the first words from the GPU block ------------------------------------------------------- */

/*
 * dt: gmu@506a000, 0x31000 long. The GMU's register offsets in a6xx_gmu.xml are words from the
 * GPU's base (0x5000000); the GMU's own window starts at word 0x1a800 (0x506a000), as on every
 * A6xx, so these are the same numbers HackiOS uses for its A642L.
 */
static struct block gmu = { "gmu", 0x0506a000, 0x31000 };
#define GMU_WORD(n)             (4u * (n))
#define GMU_DTCM                GMU_WORD(0x1c00)
#define GMU_DTCM_VERSIONS       GMU_WORD(0x1c00 + 0xff8)
#define GMU_CM3_SYSRESET        GMU_WORD(0x5000)
#define GMU_CM3_BOOT_CONFIG     GMU_WORD(0x5001)
#define GMU_CM3_FW_INIT_RESULT  GMU_WORD(0x501c)
#define GMU_CM3_CFG             GMU_WORD(0x502d)
#define GMU_ALWAYS_ON_COUNTER_L GMU_WORD(0x5088)
#define GMU_ALWAYS_ON_COUNTER_H GMU_WORD(0x5089)
#define GMU_SPTPRAC_PWR_CLK_STATUS GMU_WORD(0x50d0)
#define GMU_RPMH_POWER_STATE    GMU_WORD(0x50ec)
#define GMU_HFI_CTRL_STATUS     GMU_WORD(0x5180)
#define GMU_HFI_VERSION_INFO    GMU_WORD(0x5181)
#define GMU_GENERAL_7           GMU_WORD(0x51cc)
#define GMU_AO_HOST_INTERRUPT_STATUS GMU_WORD(0x9305)
#define GMU_RSCC_CONTROL_ACK    GMU_WORD(0x9308)
#define GMU_AO_GPU_CX_BUSY_STATUS GMU_WORD(0x930c)
#define GMU_AHB_FENCE_STATUS    GMU_WORD(0x9313)

/* The 3D core's bus interface ("GBIF", A618 has one: words 0x3c00 on) and cx_mem (dt: 0x509e000). */
static struct block gbif = { "gbif", 0x05000000 + 4 * 0x3c00, 0x1000 };
#define GBIF_SCACHE_CNTL0       (4 * 0x01)
#define GBIF_HALT               (4 * 0x45)
#define GBIF_HALT_ACK           (4 * 0x46)
static struct block cx_misc = { "cx_misc", 0x0509e000, 0x1000 };
#define CX_MISC_SYSTEM_CACHE_CNTL_0 (4 * 1)
#define CX_MISC_SYSTEM_CACHE_CNTL_1 (4 * 2)

static bool
step_first_words(void)
{
	uint64_t start, nanos;
	uint32_t before, hz;

	if (!cx_ready()) {
		return false;
	}
	announce("read registers of the GMU (0x506a000): the first words from the GPU block itself");
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	get(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG);
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "cm3_cfg", GMU_CM3_CFG);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	get(&gmu, "general_7", GMU_GENERAL_7);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
	get(&gmu, "always_on_counter_h", GMU_ALWAYS_ON_COUNTER_H);
	get(&gmu, "always_on_counter_l", GMU_ALWAYS_ON_COUNTER_L);
	start = mach_absolute_time();
	before = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L);
	sleep_ms(10);
	hz = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L) - before;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	hz = nanos ? (uint32_t)((uint64_t)hz * NSEC_PER_SEC / nanos) : 0;
	printf("adreno: %u: the GMU's counter counts at %u Hz: %s\n", step, hz, hz > 18000000 && hz < 20500000 ?
	    "the 19.2 MHz crystal: THE GPU BLOCK IS ALIVE" : "not the crystal's 19.2 MHz: see the rest");
	for (uint32_t i = 0; i < 5; i++) {
		get(&gmu, "dtcm, where firmware has its versions", GMU_DTCM_VERSIONS + 4 * i);
	}

	announce("read the GPU's bus interface (0x500f000) and cx_mem (0x509e000)");
	get(&gbif, "scache_cntl0", GBIF_SCACHE_CNTL0);
	get(&gbif, "halt", GBIF_HALT);
	get(&gbif, "halt_ack", GBIF_HALT_ACK);
	get(&cx_misc, "system_cache_cntl_0", CX_MISC_SYSTEM_CACHE_CNTL_0);
	get(&cx_misc, "system_cache_cntl_1", CX_MISC_SYSTEM_CACHE_CNTL_1);
	return true;
}

/* ---- step 7: the GMU's channel to RPMh ----------------------------------------------------------------- */

/*
 * On the A618 the GMU's RSCC is inside the GMU's window, 0x23000 on (a6xx_gmu.c: "gmu->rscc =
 * gmu->mmio + 0x23000" for everything but the A650 family): 0x508d000. Offsets in words.
 */
static struct block rscc = { "rscc", 0x0506a000 + 0x23000, 0x2000 };
#define RSCC_RSC_STATUS0_DRV0   (4 * 0x004)
#define RSCC_PDC_SLAVE_ID_DRV0  (4 * 0x00b)
#define RSCC_SEQ_BUSY_DRV0      (4 * 0x101)
#define RSCC_SEQ_MEM_0_DRV0     (4 * 0x180)
#define RSCC_TCS0_DRV0_STATUS   (4 * 0x346)

static bool
step_rscc(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the GMU's RPMh channel (0x508d000)");
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);
	get(&rscc, "tcs0_drv0_status", RSCC_TCS0_DRV0_STATUS);
	get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
	return true;
}

/* ---- steps 8 and 9: the SMMU --------------------------------------------------------------------------- */

/*
 * dt: iommu@5040000 (qcom,adreno-smmu, smmu-v2), 0x10000 long. Streams (sdmmagpie-gpu.dtsi):
 * 0 the GPU, 2 secure content, 4 and 5 the GMU. The global registers are the ones the
 * hypervisor imitates; offsets: arm-smmu.h.
 */
static struct block smmu = { "smmu", 0x05040000, 0x10000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_IDR2               0x028
#define SMMU_IDR7               0x03c
#define SMMU_SGFSR              0x048
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TCR             0x30
#define SMMU_CB_FSR             0x58

static uint32_t smmu_page, smmu_pages, smmu_contexts, smmu_matches;

static bool
smmu_layout(void)
{
	uint32_t idr0 = get(&smmu, "idr0", SMMU_IDR0), idr1 = get(&smmu, "idr1", SMMU_IDR1);

	smmu_page = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	smmu_contexts = idr1 & 0xff;
	smmu_matches = idr0 & 0xff;
	printf("adreno: %u: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n", step,
	    smmu_matches, smmu_contexts, smmu_pages, smmu_page);
	if (2 * smmu_pages * smmu_page != smmu.size) {
		printf("adreno: %u: that makes %#x of registers, the device tree says %#x: not reading further\n", step,
		    2 * smmu_pages * smmu_page, smmu.size);
		return false;
	}
	return true;
}

static bool
step_smmu(void)
{
	if (!cx_ready() || !smmu_clocked()) {
		return false;
	}
	announce("read the ID registers of the GPU's SMMU (0x5040000): global registers, answered by the hypervisor");
	if (!smmu_layout()) {
		return false;
	}
	get(&smmu, "idr2", SMMU_IDR2);
	get(&smmu, "idr7", SMMU_IDR7);
	get(&smmu, "scr0", SMMU_SCR0);
	get(&smmu, "sgfsr", SMMU_SGFSR);

	announce("read the SMMU's stream table and the kinds of its contexts, as the boot loader and hypervisor left them");
	for (uint32_t i = 0; i < smmu_matches && i < 16; i++) {
		get(&smmu, "smr", SMMU_SMR(i));
		get(&smmu, "s2cr", SMMU_S2CR(i));
	}
	for (uint32_t i = 0; i < smmu_contexts && i < 16; i++) {
		get(&smmu, "cbar", smmu_page + SMMU_CBAR(i));
	}
	return true;
}

/* Only context 0, the GPU's own; at least one other belongs to TrustZone (secure content). */
static bool
step_smmu_context(void)
{
	uint32_t context;

	if (!cx_ready() || !smmu_clocked()) {
		return false;
	}
	announce("read translation context 0 of the SMMU: registers of the hardware itself, not the hypervisor's");
	if (!smmu_layout()) {
		return false;
	}
	context = smmu_pages * smmu_page;
	get(&smmu, "cb0 sctlr", context + SMMU_CB_SCTLR);
	get(&smmu, "cb0 ttbr0, low", context + SMMU_CB_TTBR0);
	get(&smmu, "cb0 ttbr0, high", context + SMMU_CB_TTBR0 + 4);
	get(&smmu, "cb0 tcr", context + SMMU_CB_TCR);
	get(&smmu, "cb0 fsr", context + SMMU_CB_FSR);
	return true;
}

/* ---- step 10: the GMU's memory -------------------------------------------------------------------------- */

/*
 * Firmware is loaded by writing it through these registers into the processor's memory while
 * it is held in reset (a6xx_gmu_fw_load). Two words go there and back, and what was there is put
 * back; the processor stays in reset (it has no firmware to run).
 */
static bool
step_gmu_memory(void)
{
	static const uint32_t pattern[2] = { 0x600d0001, 0x0a110002 };
	uint32_t was[2], back[2];

	if (!cx_ready()) {
		return false;
	}
	announce("hold the GMU's processor in reset, write two words into its data memory (0x5071000), read them back");
	if (get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT) == 0xbabeface) {
		printf("adreno: %u: firmware is running in the GMU: leaving its memory alone\n", step);
		return false;
	}
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	for (int i = 0; i < 2; i++) {
		was[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, pattern[i]);
	}
	for (int i = 0; i < 2; i++) {
		back[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, was[i]);
	}
	printf("adreno: %u: the GMU's memory %s\n", step, back[0] == pattern[0] && back[1] == pattern[1] ?
	    "TAKES WHAT IS WRITTEN: its firmware can be loaded" : "does not hold what was written");
	return back[0] == pattern[0] && back[1] == pattern[1];
}

/* ---- the firmware, from the root filesystem ---------------------------------------------------------- */

/*
 * The GPU needs four of the phone's own files (Android's /vendor/firmware): the GMU's firmware,
 * the 3D core's microcode and the signed zap shader, in its .mdt header and its segments. They
 * are read from the iOS root, which is mounted by the time the steps run, in FW_DIRECTORY:
 * docs/GPU.md says how they get there. Names as the vendor's driver for this GPU asks for them
 * (msm-4.14 adreno-gpulist.h, ADRENO_REV_A618). Looked for first in the device tree's /sweet-fw
 * node, one property per file (tools/mkadt.py --firmware DIR): then the boot image carries them
 * and the root needs no copy.
 */
#define FW_DIRECTORY            "/fw/adreno/"
#define FW_GMU                  "a618_gmu.bin"
#define FW_SQE                  "a630_sqe.fw"
#define FW_ZAP                  "a615_zap"
#define FW_LARGEST              0x100000

/* A file of fw/ in the small image: put the phone's /vendor/firmware/<name> into fw-gpu/ and run userland/build.sh. */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
fw_file(const char *name, uint64_t *size)
{
	int disk = -1;
	const uint8_t *data = hakios_image_file("fw", name, size, &disk);

	if (data != NULL) {
		printf("adreno: %u: fw/%s: %llu bytes in the image of /dev/md%d\n", step, name, *size, disk);
	} else {
		printf("adreno: %u: no fw/%s in the small image\n", step, name);
	}
	return data;
}

/* ---- memory the GPU block reaches ------------------------------------------------------------------- */

/*
 * Whatever in the GPU block reads or writes memory -- the GMU's processor, later the 3D core --
 * does it through the SMMU (step 8), which makes of the address it uses ("the GPU's address"
 * below) a physical one by walking page tables of ours: three levels of 4K pages, 39 bits of
 * address. A context of the SMMU is one such translation, and its table of streams says which
 * requests go through which: stream 0 is the 3D core, 5 the GMU (sdmmagpie-gpu.dtsi: iommus,
 * gmu_kernel). Formats and the order of programming: linux io-pgtable-arm.c and arm-smmu.c; this
 * part is HackiOS's redwood_adreno.c, as it is.
 */
#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_UNPRIVILEGED        (1ull << 6)     /* the 3D core's accesses are; the GMU's are not */
#define PTE_READ_ONLY           (1ull << 7)
#define PTE_SHARED              (2ull << 8)
#define PTE_ACCESSED            (1ull << 10)
#define PTE_NOT_GLOBAL          (1ull << 11)
#define PTE_ADDRESS             0x0000fffffffff000ull
#define PTE_GMU                 (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)
#define PTE_CORE                (PTE_GMU | PTE_UNPRIVILEGED)

#define SMMU_S2CR_FAULT         (2u << 16)
#define SMMU_SMR_VALID          (1u << 31)
/*
 * Exact stream matches. The downstream dt's mask 0x400 made the first Pixel run fail: the hypervisor
 * dropped SMR 0x84000005 (read back 0), probably as a match that also covers stream 0x405. The mainline
 * sm7150.dtsi has #iommu-cells = <1> for this SMMU (GMU: <&adreno_smmu 5>), and ios-surya, on the
 * same SoC, attaches its stream as SMR = valid | SID.
 */
#define SMMU_STREAM_MASK        0x0
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12)
#define SMMU_SCTLR_SERVE_AFTER_FAULT (1u << 8)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe

static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

/* Push what this processor has written to a stretch of memory out of its cache: the GPU block reads memory. */
static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
}

#define SPACE_TABLES            1024           /* 2 GB of addresses, were they all in use */

struct gpu_space {
	const char *name;
	uint32_t context;               /* which of the SMMU's */
	uint32_t control;               /* bits of its control register besides the usual */
	uint32_t table_count;
	uint64_t *root;
	struct {
		uint64_t phys;
		uint64_t *table;
	} tables[SPACE_TABLES];
};

static uint64_t *
space_new_table(struct gpu_space *space)
{
	static vm_offset_t spare;
	static unsigned spare_left;
	uint64_t *table;

	if (space->table_count == SPACE_TABLES) {
		return NULL;
	}
	if (spare_left == 0) {
		if (kmem_alloc(kernel_map, &spare, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
		    KERN_SUCCESS) {
			return NULL;
		}
		spare_left = PAGE_SIZE / 4096;
	}
	table = (uint64_t *)(spare + PAGE_SIZE - 4096 * spare_left--);
	space->tables[space->table_count].phys = kvtophys((vm_offset_t)table);
	space->tables[space->table_count++].table = table;
	return table;
}

static uint64_t *
space_next(struct gpu_space *space, uint64_t *entry)
{
	uint64_t *table;

	if (*entry & 1) {
		for (uint32_t i = 0; i < space->table_count; i++) {
			if (space->tables[i].phys == (*entry & PTE_ADDRESS)) {
				return space->tables[i].table;
			}
		}
		return NULL;
	}
	table = space_new_table(space);
	if (table != NULL) {
		*entry = kvtophys((vm_offset_t)table) | PTE_TABLE;
	}
	return table;
}

/* One 4K page. */
static bool
space_map(struct gpu_space *space, uint64_t address, uint64_t phys, uint64_t attributes)
{
	uint64_t *level2, *level3;

	if (address >> 39 || (space->root == NULL && (space->root = space_new_table(space)) == NULL)) {
		return false;
	}
	level2 = space_next(space, &space->root[address >> 30 & 0x1ff]);
	level3 = level2 == NULL ? NULL : space_next(space, &level2[address >> 21 & 0x1ff]);
	if (level3 == NULL) {
		return false;
	}
	level3[address >> 12 & 0x1ff] = phys == 0 && attributes == 0 ? 0 : phys | attributes | PTE_PAGE;
	return true;
}

/* The SMMU reads the tables from memory, not from this processor's cache. */
static void
space_commit(struct gpu_space *space)
{
	for (uint32_t i = 0; i < space->table_count; i++) {
		to_memory((vm_offset_t)space->tables[i].table, 4096);
	}
}

/* Memory of ours, in one piece, that the GPU block sees at an address of its own. */
struct gpu_block {
	uint64_t address;               /* the GPU's */
	uint32_t size;
	vm_offset_t memory;             /* ours */
	uint64_t phys;
};

static bool
block_new(struct gpu_space *space, struct gpu_block *block, uint64_t address, uint32_t size, uint64_t attributes)
{
	block->address = address;
	block->size = (size + 4095) & ~4095u;
	if (kmem_alloc_contig(kernel_map, &block->memory, round_page(block->size), 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adreno: %u: no %#x bytes of memory in one piece for %s\n", step, block->size, space->name);
		return false;
	}
	block->phys = kvtophys(block->memory);
	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (!space_map(space, address + offset, block->phys + offset, attributes)) {
			return false;
		}
	}
	to_memory(block->memory, block->size);
	printf("adreno: %u: %s sees %#llx..%#llx in memory of ours (%#llx)\n", step, space->name, address,
	    address + block->size - 1, block->phys);
	return true;
}

/* A second view of a block without this processor's cache in between, for memory both sides write a word at a time. */
static vm_offset_t
block_uncached(const struct gpu_block *block)
{
	return ml_io_map_wcomb(block->phys, block->size);
}

static uint32_t
smmu_context(const struct gpu_space *space)
{
	return (smmu_pages + space->context) * smmu_page;
}

/* What a context has recorded of a request it could not serve. */
static bool
space_faulted(const struct gpu_space *space)
{
	uint32_t context = smmu_context(space), status = rd(smmu.base + context + SMMU_CB_FSR);

	if ((status & SMMU_FSR_ALL) == 0) {
		return false;
	}
	printf("adreno: %u: THE SMMU REFUSED %s: status %#x, address %#llx, syndrome %#x\n", step, space->name, status,
	    (uint64_t)rd(smmu.base + context + SMMU_CB_FAR + 4) << 32 | rd(smmu.base + context + SMMU_CB_FAR),
	    rd(smmu.base + context + SMMU_CB_FSYNR0));
	return true;
}

/* Give a context our tables and send a stream through it; `match` is the entry of the stream table to use. */
static bool
space_on(struct gpu_space *space, uint32_t match, uint32_t stream)
{
	uint32_t context = smmu_context(space);
	uint64_t root = space->tables[0].phys | (uint64_t)space->context << 48;

	space_commit(space);
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, 0);
	put(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context), SMMU_CBA2R_64_BIT);
	put(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context), SMMU_CBAR_STAGE_1);
	put(&smmu, "cb tcr2", context + SMMU_CB_TCR2, SMMU_TCR2);
	put(&smmu, "cb tcr", context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0, (uint32_t)root);
	put(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	put(&smmu, "cb ttbr1, low", context + SMMU_CB_TTBR1, 0);
	put(&smmu, "cb ttbr1, high", context + SMMU_CB_TTBR1 + 4, 0);
	put(&smmu, "cb mair0", context + SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&smmu, "cb mair1", context + SMMU_CB_MAIR1, 0);
	put(&smmu, "cb fsr", context + SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&smmu, "cb tlbiasid", context + SMMU_CB_TLBIASID, space->context);
	put(&smmu, "cb tlbsync", context + SMMU_CB_TLBSYNC, 1);
	if (!wait_for(&smmu, context + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		printf("adreno: %u: the SMMU does not finish forgetting old translations (%#x)\n", step,
		    rd(smmu.base + context + SMMU_CB_TLBSTATUS));
		return false;
	}
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, SMMU_SCTLR_ON | space->control);
	put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
	put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream);

	/* the global ones are the hypervisor's to grant: see what it made of them */
	if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream) ||
	    (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context ||
	    (get(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
	    !(get(&smmu, "cb sctlr", context + SMMU_CB_SCTLR) & 1) ||
	    get(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0) != (uint32_t)root ||
	    get(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		printf("adreno: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
		    stream, space->context);
		return false;
	}
	get(&smmu, "cb fsr", context + SMMU_CB_FSR);
	get(&smmu, "sgfsr", SMMU_SGFSR);
	return true;
}

/*
 * Which entry of the stream table a stream is to use: the one that already names it, if the boot
 * loader or the hypervisor left one, else the first that is not in use; `fallback` if every one is
 * (step 8 shows the table as it was found).
 */
static uint32_t
smmu_match_for(uint32_t stream, uint32_t fallback)
{
	uint32_t free_match = ~0u;

	for (uint32_t i = 0; i < smmu_matches && i < 64; i++) {
		uint32_t smr = rd(smmu.base + SMMU_SMR(i));

		if ((smr & SMMU_SMR_VALID) && (smr & 0x7fff) == stream) {
			printf("adreno: %u: stream %u has entry %u of the stream table already (smr %#x, s2cr %#x)\n", step, stream,
			    i, smr, rd(smmu.base + SMMU_S2CR(i)));
			return i;
		}
		if (!(smr & SMMU_SMR_VALID) && free_match == ~0u) {
			free_match = i;
		}
	}
	return free_match != ~0u ? free_match : fallback;
}

/* A stream goes nowhere again, and its context translates nothing. */
static void
space_off(struct gpu_space *space, uint32_t match)
{
	put(&smmu, "smr", SMMU_SMR(match), 0);
	put(&smmu, "s2cr", SMMU_S2CR(match), SMMU_S2CR_FAULT);
	put(&smmu, "cb sctlr", smmu_context(space) + SMMU_CB_SCTLR, 0);
}

/* ---- step 11: TrustZone -------------------------------------------------------------------------------- */

/*
 * The 3D core comes out of reset in "secure" mode, and only TrustZone lets it out: it does when
 * handed the vendor's signed zap shader to authenticate (image 13; qcom_scm.c qcom_scm_pas_*,
 * a6xx_gpu.c a6xx_zap_shader_init). Questions that pass no memory show whether TrustZone listens
 * to this kernel at all. A call is an SMC, which the hypervisor passes on (qcom_scm-smc.c).
 * HackiOS's step 11, with three images this SoC starts that way for comparison (sdmmagpie.dtsi
 * qcom,pas-id: audio 1, modem 4, video 9).
 */
#define SCM_FAST                (1ull << 31)
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_IS_CALL_AVAILABLE   SCM_CALL(6, 1)
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_PAS_IS_SUPPORTED    SCM_CALL(2, 7)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1
#define PAS_GPU                 13

/* One call. What TrustZone returns: 0 or an error of its own, and a word of answer. */
static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer)
{
	uint64_t session = 0;

	printf("adreno: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function,
	    first, second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	hold(true);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("adreno: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("adreno: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static bool
step_trustzone(void)
{
	static const struct {
		const char *name;
		uint32_t id;
	} images[] = { { "audio", 1 }, { "modem", 4 }, { "video", 9 }, { "GPU zap shader", PAS_GPU } };
	uint64_t answer = 0, how = SCM_FAST | SCM_64 | SCM_SIP;
	uint32_t fast[4] = { 0 }, slow[4] = { 0 };

	announce("ask TrustZone what Linux asks it first: is the call \"is this call available\" available");
	if (scm("whether it can be asked", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE, 0,
	    0, &answer) != 0 || answer != 1) {
		how = SCM_FAST | SCM_SIP;
		if (scm("the same, the 32-bit way", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE,
		    0, 0, &answer) != 0 || answer != 1) {
			printf("adreno: %u: TrustZone does not answer the way it answers Linux\n", step);
			return false;
		}
	}

	announce("ask TrustZone whether it has the calls that authenticate a firmware image");
	scm("for the call that takes an image's signature", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
	    SCM_SIP | SCM_PAS_INIT_IMAGE, 0, 0, &answer);
	scm("for the call that authenticates and starts an image", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
	    SCM_SIP | SCM_PAS_AUTH_AND_RESET, 0, 0, &answer);

	announce("ask TrustZone which images it \"supports\": three this phone starts through it, and the GPU's");
	for (unsigned i = 0; i < 4; i++) {
		answer = 0;
		fast[i] = scm(images[i].name, how | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0, 0, &answer) == 0 && answer;
		answer = 0;
		slow[i] = scm(images[i].name, (how & ~SCM_FAST) | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0, 0,
		    &answer) == 0 && answer;
	}
	printf("adreno: %u: \"supported\", asked at once / as Linux asks: audio %u/%u, modem %u/%u, video %u/%u, GPU %u/%u\n",
	    step, fast[0], slow[0], fast[1], slow[1], fast[2], slow[2], fast[3], slow[3]);
	return true;
}

/* ---- step 12: the zap shader's signature ----------------------------------------------------------------- */

/*
 * The first of the three calls with which Linux has a zap shader loaded (mdt_loader.c
 * qcom_mdt_pas_init): TrustZone is given the image's header -- the ELF header, its table of
 * segments and the segment of hashes and certificates, which is the file a615_zap.mdt -- in a
 * page of ours, and checks the signature. Nothing of the GPU is involved yet. TrustZone protects
 * that memory while it works on it: it is not touched again, and never freed.
 */
static const uint8_t *zap_mdt;
/*
 * boot-arg adreno_zap_meta=PHYS: the zap shader's header for PAS_INIT_IMAGE at that physical
 * address instead of a page of the kernel's. All of this kernel's RAM is above 4 GiB (mkadt.py
 * --highmem); Linux's metadata buffer (dma_alloc_coherent) is normally below, and some TrustZone
 * versions accept a header above 4 GiB at init_image yet fail what follows (our PAS_MEM_SETUP:
 * -3145798). 0xb0000000 is free low RAM: the base this kernel itself used without --highmem.
 */
static uint64_t zap_meta;
static uint64_t zap_mdt_size;

static bool
zap_header(uint64_t *phys)
{
	vm_offset_t page = 0;
	uint32_t segments;

	if (zap_mdt == NULL && (zap_mdt = fw_file(FW_ZAP ".mdt", &zap_mdt_size)) == NULL) {
		return false;
	}
	/* a 32-bit ELF: the count of segments at 44, their table at 52, 32 bytes each */
	segments = zap_mdt_size >= 52 ? zap_mdt[44] | zap_mdt[45] << 8 : 0;
	if (memcmp(zap_mdt, "\177ELF\1", 5) != 0 || segments < 2 || segments > 16 || zap_mdt_size < 52 + 32 * segments ||
	    zap_mdt_size > PAGE_SIZE) {
		printf("adreno: %u: that is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step,
		    zap_mdt[0], zap_mdt[1], zap_mdt[2], zap_mdt[3], segments, zap_mdt_size);
		return false;
	}
	if (zap_meta != 0) {
		volatile uint8_t *low = (volatile uint8_t *)ml_io_map_wcomb(zap_meta, PAGE_SIZE);
		for (uint64_t i = 0; i < PAGE_SIZE; i++) {
			low[i] = i < zap_mdt_size ? zap_mdt[i] : 0;
		}
		__builtin_arm_dsb(0xf);
		*phys = zap_meta;
		printf("adreno: %u: the zap shader's header at %#llx, below 4 GiB (adreno_zap_meta)\n", step, zap_meta);
		return true;
	}
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	memcpy((void *)page, zap_mdt, zap_mdt_size);
	to_memory(page, PAGE_SIZE);
	*phys = kvtophys(page);
	return true;
}

static bool
step_zap_signature(void)
{
	uint64_t header = 0, answer = ~0ull;
	int32_t result;

	announce("read the zap shader's header, " FW_DIRECTORY FW_ZAP ".mdt, from the root filesystem");
	if (!zap_header(&header)) {
		return false;
	}
	announce("hand the header to TrustZone to check its signature (image 13)");
	result = scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
	    header, 0, &answer);
	if (result == 0 && answer == 0) {
		printf("adreno: %u: TRUSTZONE ACCEPTS THE ZAP SHADER'S SIGNATURE FROM THIS KERNEL\n", step);
		return true;
	}
	printf("adreno: %u: TrustZone does not take it: the call returns %d, its answer is %#llx\n", step, result, answer);
	return false;
}

/* ---- step 13: memory for the GMU ------------------------------------------------------------------------ */

/*
 * The A618's GMU is of the older kind ("legacy" in Linux, HFI version 1; the vendor's driver
 * calls the A615 family so): its firmware is one image for its 16K of code memory, written
 * through registers (step 14), and the memory it reaches through the SMMU is at addresses its
 * driver chooses (msm-4.14 kgsl_gmu.c gmu_memory_probe, gmu_vma): its caches' backing at
 * 0x4000 and 0x44000, and then a megabyte apart from 0x60000000 the "dummy" page, the message
 * queues ("HFI"), the memory it dumps into when it fails, and its log.
 */
#define GMU_ICACHE_ADDRESS      0x00004000u
#define GMU_DCACHE_ADDRESS      0x00044000u
#define GMU_CACHE_SIZE          (0x40000u - 0x4000u)
#define GMU_DUMMY_ADDRESS       0x60000000u
#define GMU_HFI_ADDRESS         0x60100000u
#define GMU_HFI_SIZE            0x4000u         /* the table and three queues of 4K (Linux's a6xx_hfi_init) */
#define GMU_DUMP_ADDRESS        0x60200000u
#define GMU_DUMP_SIZE           0x4000u
#define GMU_LOG_ADDRESS         0x60300000u
#define GMU_LOG_SIZE            0x1000u         /* LOGMEM_SIZE */
#define GMU_STREAM              5               /* dt: gmu_kernel, iommus = <&kgsl_smmu 5> */

static uint32_t gmu_match = 1;          /* its entry of the stream table (smmu_match_for) */

static struct gpu_space gmu_space = { .name = "the GMU", .context = 1 };
static struct gpu_block gmu_icache, gmu_dcache, gmu_dummy, gmu_hfi, gmu_dump, gmu_log;
static const uint8_t *gmu_firmware;
static uint64_t gmu_firmware_size;
static bool gmu_space_is_on, gmu_runs, gmu_talks, core_is_on, rsc_awake;

static bool
step_gmu_space(void)
{
	if (!cx_ready() || !smmu_clocked()) {
		return false;
	}
	announce("read the GMU's firmware, " FW_DIRECTORY FW_GMU ", and make the memory its driver gives it");
	if (gmu_firmware == NULL && (gmu_firmware = fw_file(FW_GMU, &gmu_firmware_size)) == NULL) {
		return false;
	}
	if (gmu_firmware_size > 0x8000 || (gmu_firmware_size & 3)) {
		printf("adreno: %u: %llu bytes is not the image of an A618's GMU (at most 32K, whole words)\n", step,
		    gmu_firmware_size);
		return false;
	}
	if (!block_new(&gmu_space, &gmu_icache, GMU_ICACHE_ADDRESS, GMU_CACHE_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_dcache, GMU_DCACHE_ADDRESS, GMU_CACHE_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_dummy, GMU_DUMMY_ADDRESS, 4096, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_hfi, GMU_HFI_ADDRESS, GMU_HFI_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_dump, GMU_DUMP_ADDRESS, GMU_DUMP_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_log, GMU_LOG_ADDRESS, GMU_LOG_SIZE, PTE_GMU)) {
		return false;
	}

	announce("give the SMMU's context 1 these tables and send the GMU's requests (stream 5) through it: the first "
	    "writes to the SMMU, the global ones through the hypervisor");
	if (!smmu_layout()) {
		return false;
	}
	gmu_match = smmu_match_for(GMU_STREAM, 1);
	gmu_space_is_on = true;         /* whatever of it has been written by the time something fails is taken back */
	return space_on(&gmu_space, gmu_match, GMU_STREAM);
}

/* ---- step 14: the GMU's firmware ----------------------------------------------------------------------- */

#define GMU_ITCM                GMU_WORD(0x0c00)

/* msm-4.14 adreno_a6xx_gmu.c _load_legacy_gmu_fw: the whole file into code memory, from its start. */
static bool
step_gmu_firmware(void)
{
	uint32_t wrong = 0;

	if (!cx_ready() || gmu_firmware == NULL) {
		return false;
	}
	announce("hold the GMU's processor in reset and write its firmware into its code memory (0x506d000)");
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	for (uint64_t at = 0; at < gmu_firmware_size; at += 4) {
		uint32_t word;

		memcpy(&word, gmu_firmware + at, sizeof(word));
		wr(gmu.base + GMU_ITCM + at, word);
	}
	settle();
	snprintf(last_write, sizeof(last_write), "step %u wrote the GMU's firmware into its code memory", step);
	for (uint64_t at = 0; at < gmu_firmware_size; at += 4) {
		uint32_t word;

		memcpy(&word, gmu_firmware + at, sizeof(word));
		wrong += rd(gmu.base + GMU_ITCM + at) != word;
	}
	if (wrong) {
		printf("adreno: %u: %u words of the GMU's code memory do not read back as written\n", step, wrong);
		return false;
	}
	printf("adreno: %u: THE GMU'S FIRMWARE IS LOADED (%llu bytes, read back)\n", step, gmu_firmware_size);
	return true;
}

/* ---- step 15: the GMU's way to RPMh, and the GMU started ------------------------------------------------- */

/*
 * The GMU raises and drops the 3D core's supply by itself, through its RPMh channel (step 7) and
 * a sequencer in the always-on part of the SoC ("PDC"). On the A618 both programs are the
 * kernel's to load, before the firmware starts (msm-4.14 _load_gmu_rpmh_ucode; linux
 * a6xx_gmu_rpmh_init: the values for the A618 are its own in both). PDC: dt kgsl_gmu_pdc_cfg
 * 0xb290000, kgsl_gmu_pdc_seq 0xb490000; register numbers a6xx_reg.h, in words.
 */
static struct block pdc = { "pdc", 0x0b290000, 0x10000 };
static struct block pdc_seq = { "pdc_seq", 0x0b490000, 0x10000 };
#define PDC_GPU_ENABLE_PDC      (4 * 0x1140)
#define PDC_GPU_SEQ_START_ADDR  (4 * 0x1148)
#define PDC_TCS1                (4 * 0x1572)    /* control; +1 enable bank; +2 wait bank; +3.. commands */
#define PDC_TCS3                (4 * 0x15d6)
#define PDC_CONTROL             0
#define PDC_ENABLE_BANK         4
#define PDC_WAIT_BANK           8
#define PDC_CMD(n, field)       (12 + 16 * (n) + 4 * (field))   /* field 0 message id, 1 address, 2 data */
#define RSCC_PDC_SEQ_START_ADDR (4 * 0x008)
#define RSCC_PDC_MATCH_VALUE_LO (4 * 0x009)
#define RSCC_PDC_MATCH_VALUE_HI (4 * 0x00a)
#define RSCC_HIDDEN_TCS_CMD0_ADDR (4 * 0x00d)
#define RSCC_HIDDEN_TCS_CMD0_DATA (4 * 0x00e)
#define RSCC_OVERRIDE_START_ADDR (4 * 0x100)
#define RSCC_ASLEEP             (1u << 16)      /* of rsc_status0_drv0 (linux a6xx_rpmh_stop) */

#define GMU_GX_SPTPRAC_POWER_CONTROL GMU_WORD(0x0081)
#define GMU_ICACHE_CONFIG       GMU_WORD(0x4c00)
#define GMU_DCACHE_CONFIG       GMU_WORD(0x4c01)
#define GMU_SYS_BUS_CONFIG      GMU_WORD(0x4c0f)
#define GMU_CX_GMU_WFI_CONFIG   GMU_WORD(0x5002)
#define GMU_POWER_COUNTER_ENABLE GMU_WORD(0x5040)
#define GMU_PWR_COL_INTER_FRAME_CTRL GMU_WORD(0x50c0)
#define GMU_RPMH_CTRL           GMU_WORD(0x50e8)
#define GMU_RPMH_CTRL_ALL       0xf11           /* the RPMh interface, and every vote the firmware may cast */
#define GMU_PWR_COL_CP_MSG      GMU_WORD(0x5100)
#define GMU_PWR_COL_CP_RESP     GMU_WORD(0x5101)
#define GMU_HFI_SFR_ADDR        GMU_WORD(0x5182)        /* at start: which GPU this is */
#define GMU_HFI_QTBL_INFO       GMU_WORD(0x5184)
#define GMU_HFI_QTBL_ADDR       GMU_WORD(0x5185)
#define GMU_HFI_CTRL_INIT       GMU_WORD(0x5186)
#define GMU_GMU2HOST_INTR_CLR   GMU_WORD(0x5191)
#define GMU_GMU2HOST_INTR_INFO  GMU_WORD(0x5192)
#define GMU_GMU2HOST_INTR_MASK  GMU_WORD(0x5193)
#define GMU_HOST2GMU_INTR_SET   GMU_WORD(0x5194)
#define GMU_BOOT_SLUMBER_OPTION GMU_WORD(0x23f8)
#define GMU_GX_VOTE_IDX         GMU_WORD(0x23f9)
#define GMU_MX_VOTE_IDX         GMU_WORD(0x23fa)
#define GMU_DCVS_ACK_OPTION     GMU_WORD(0x23fc)
#define GMU_DCVS_PERF_SETTING   GMU_WORD(0x23fd)
#define GMU_DCVS_BW_SETTING     GMU_WORD(0x23fe)
#define GMU_DCVS_RETURN         GMU_WORD(0x23ff)
#define GMU_AO_HOST_INTERRUPT_CLR GMU_WORD(0x9304)
#define GMU_AO_HOST_INTERRUPT_MASK GMU_WORD(0x9306)
#define GMU_RSCC_CONTROL_REQ    GMU_WORD(0x9307)
#define GMU_AO_GPU_CX_BUSY_MASK GMU_WORD(0x930e)
#define GMU_AO_AHB_FENCE_CTRL   GMU_WORD(0x9310)
#define GMU_AHB_FENCE_RANGE_0   GMU_WORD(0x9311)
#define GMU_AHB_FENCE_STATUS_CLR GMU_WORD(0x9314)
#define GMU_CHIP                0x06018000      /* dt qcom,chipid 0x06010800, with minor and patch in 4 bits each */
#define GMU_STARTED             0xbabeface      /* fw_init_result of a legacy firmware that has started */

static void
rscc_show(void)
{
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);
	get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
}

static void
pdc_tcs(uint32_t tcs, uint32_t sleep_level)
{
	put(&pdc, "tcs enable bank", tcs + PDC_ENABLE_BANK, 7);
	put(&pdc, "tcs wait bank", tcs + PDC_WAIT_BANK, 0);
	put(&pdc, "tcs control", tcs + PDC_CONTROL, 0);
	put(&pdc, "tcs cmd0 msgid", tcs + PDC_CMD(0, 0), 0x10108);
	put(&pdc, "tcs cmd0 addr", tcs + PDC_CMD(0, 1), 0x30010);
	put(&pdc, "tcs cmd0 data", tcs + PDC_CMD(0, 2), tcs == PDC_TCS1 ? 1 : 2);
	put(&pdc, "tcs cmd1 msgid", tcs + PDC_CMD(1, 0), 0x10108);
	put(&pdc, "tcs cmd1 addr", tcs + PDC_CMD(1, 1), 0x30000);
	put(&pdc, "tcs cmd1 data", tcs + PDC_CMD(1, 2), sleep_level);
	put(&pdc, "tcs cmd2 msgid", tcs + PDC_CMD(2, 0), 0x10108);
	put(&pdc, "tcs cmd2 addr", tcs + PDC_CMD(2, 1), 0x30090);             /* the A618's */
	put(&pdc, "tcs cmd2 data", tcs + PDC_CMD(2, 2), tcs == PDC_TCS1 ? 0 : 3);
}

/* The table at the head of the HFI memory and the three queues after it (linux a6xx_hfi_init, legacy). */
struct hfi_queue {
	uint32_t status, start, type, words, message_words, dropped, rx_watermark, tx_watermark, rx_request, tx_request;
	uint32_t read, write;
};
struct hfi_table {
	uint32_t version, words, first_queue, queue_words, queues, active_queues;
	struct hfi_queue queue[3];
};
#define HFI_QUEUES              3               /* 0: ours to the GMU; 1: its answers; 2: its debugging messages */
#define HFI_QUEUE_WORDS         1024
#define HFI_QUEUE_AT(n)         (4096u * ((n) + 1))

static volatile struct hfi_table *hfi;  /* the uncached view */

static bool
step_gmu_start(void)
{
	static const uint32_t rsc_program[5] = { 0xa7a506a0, 0xa1e6a6e7, 0xa2e081e1, 0xe9a982e2, 0x0020e8a8 };
	static const uint32_t pdc_program[5] = { 0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc };
	static const uint8_t queue_id[HFI_QUEUES] = { 0, 4, 5 };        /* the legacy firmware's numbers */
	uint32_t result = 0;

	if (!cx_ready() || !gmu_space_is_on) {
		return false;
	}
	announce("give the GMU's RPMh channel its sleep and wake program (rscc, 0x508d000)");
	rscc_show();
	put(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0, 1u << 24);
	put(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0, 1);
	put(&rscc, "hidden_tcs_cmd0_data", RSCC_HIDDEN_TCS_CMD0_DATA, 0);
	put(&rscc, "hidden_tcs_cmd0_addr", RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
	put(&rscc, "hidden_tcs_cmd1_data", RSCC_HIDDEN_TCS_CMD0_DATA + 4 * 2, 0);
	put(&rscc, "hidden_tcs_cmd1_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 4 * 2, 0);
	put(&rscc, "hidden_tcs_cmd2_data", RSCC_HIDDEN_TCS_CMD0_DATA + 4 * 4, 0x80000000);
	put(&rscc, "hidden_tcs_cmd2_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 4 * 4, 0);
	put(&rscc, "override_start_addr", RSCC_OVERRIDE_START_ADDR, 0);
	put(&rscc, "pdc_seq_start_addr", RSCC_PDC_SEQ_START_ADDR, 0x4520);
	put(&rscc, "pdc_match_value_lo", RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	put(&rscc, "pdc_match_value_hi", RSCC_PDC_MATCH_VALUE_HI, 0x4514);
	for (unsigned i = 0; i < 5; i++) {
		put(&rscc, "seq_mem_drv0", RSCC_SEQ_MEM_0_DRV0 + 4 * i, rsc_program[i]);
	}
	if (get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0) != rsc_program[0]) {
		printf("adreno: %u: the RPMh channel does not keep its program\n", step);
		return false;
	}

	announce("give the GPU's part of the always-on sequencer (pdc, 0xb290000 and 0xb490000: parts of the SoC not touched "
	    "before) its program and commands, and enable it");
	get(&pdc, "gpu_enable_pdc", PDC_GPU_ENABLE_PDC);
	for (unsigned i = 0; i < 5; i++) {
		put(&pdc_seq, "seq_mem", 4 * i, pdc_program[i]);
	}
	pdc_tcs(PDC_TCS1, 0);
	pdc_tcs(PDC_TCS3, 2);                   /* 2 on the A618 */
	put(&pdc, "gpu_seq_start_addr", PDC_GPU_SEQ_START_ADDR, 0);
	put(&pdc, "gpu_enable_pdc", PDC_GPU_ENABLE_PDC, 0x80000001);
	get(&pdc, "gpu_enable_pdc", PDC_GPU_ENABLE_PDC);
	rsc_awake = true;

	hfi = (volatile struct hfi_table *)block_uncached(&gmu_hfi);
	if (hfi == NULL) {
		return false;
	}
	hfi->version = 0;
	hfi->words = sizeof(struct hfi_table) / 4;
	hfi->first_queue = 6;
	hfi->queue_words = sizeof(struct hfi_queue) / 4;
	hfi->queues = hfi->active_queues = HFI_QUEUES;
	for (unsigned i = 0; i < HFI_QUEUES; i++) {
		hfi->queue[i].start = GMU_HFI_ADDRESS + HFI_QUEUE_AT(i);
		hfi->queue[i].type = 10u << 8 | queue_id[i];
		hfi->queue[i].words = HFI_QUEUE_WORDS;
		hfi->queue[i].rx_watermark = hfi->queue[i].tx_watermark = hfi->queue[i].rx_request = 1;
		hfi->queue[i].status = 1;
	}
	settle();

	/* msm-4.14 a6xx_gmu_fw_start (cold boot), a6xx_gmu_power_config, a6xx_gmu_start */
	announce("tell the GMU where things are, by its registers: the message table, the log, which GPU this is; its "
	    "caches and its use of RPMh; then let its processor out of reset");
	if (get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET) != 1) {
		printf("adreno: %u: the GMU's processor is not held in reset: not configuring it while it runs\n", step);
		return false;
	}
	put(&gmu, "general_7", GMU_GENERAL_7, 1);                /* its memories kept through power collapse */
	put(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT, 0);
	put(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG, 2);    /* TCM_BOOT */
	put(&gmu, "hfi_qtbl_addr", GMU_HFI_QTBL_ADDR, GMU_HFI_ADDRESS);
	put(&gmu, "hfi_qtbl_info", GMU_HFI_QTBL_INFO, 1);
	put(&gmu, "ahb_fence_range_0", GMU_AHB_FENCE_RANGE_0, 1u << 31 | (0xa << 2) << 18 | 0x8a0);
	put(&gmu, "cm3_cfg", GMU_CM3_CFG, 0x4052);
	put(&gmu, "hfi_sfr_addr", GMU_HFI_SFR_ADDR, GMU_CHIP);
	put(&gmu, "pwr_col_cp_msg", GMU_PWR_COL_CP_MSG, GMU_LOG_ADDRESS | (GMU_LOG_SIZE / 4096 - 1));
	put(&gmu, "sys_bus_config", GMU_SYS_BUS_CONFIG, 1);
	put(&gmu, "icache_config", GMU_ICACHE_CONFIG, 1);
	put(&gmu, "dcache_config", GMU_DCACHE_CONFIG, 1);
	put(&gmu, "pwr_col_inter_frame_ctrl", GMU_PWR_COL_INTER_FRAME_CTRL, 0x9c40400);
	set(&gmu, "rpmh_ctrl", GMU_RPMH_CTRL, GMU_RPMH_CTRL_ALL);
	put(&gmu, "ao_host_interrupt_clr", GMU_AO_HOST_INTERRUPT_CLR, 0xffffffff);
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, 0xffffffff);
	put(&gmu, "cx_gmu_wfi_config", GMU_CX_GMU_WFI_CONFIG, 0);
	put(&gbif, "halt", GBIF_HALT, 0);

	gmu_runs = true;
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 0);
	for (int i = 0; i < 1000 && result != GMU_STARTED; i++) {
		sleep_us(100);
		result = rd(gmu.base + GMU_CM3_FW_INIT_RESULT);
	}
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "gmu2host_intr_info", GMU_GMU2HOST_INTR_INFO);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	if (space_faulted(&gmu_space) || result != GMU_STARTED) {
		printf("adreno: %u: the GMU's firmware does not come up\n", step);
		return false;
	}
	printf("adreno: %u: THE GMU'S FIRMWARE RUNS\n", step);
	return true;
}

/* ---- step 16: the 3D core's rail, and the GMU's message queues ------------------------------------------- */

/*
 * "Out of band" requests: a bit in host2gmu_intr_set, answered by a bit in gmu2host_intr_info,
 * and, for some, given back by setting the answer's bit (msm-4.14 a6xx_gmu_oob_set and _clear
 * for the A615 family; linux the same as "legacy").
 */
#define OOB_GPU                 16              /* keep the 3D core powered for us: answer 24, given back with 24 */
#define OOB_BOOT_SLUMBER        22              /* boot or slumber, as boot_slumber_option says: answer 30 */
#define OOB_DCVS                23              /* the speed in dcvs_perf_setting: answer 31 */
#define OOB_ANSWER(request)     ((request) + 8)

static bool
oob_set(const char *what, uint32_t request)
{
	uint32_t answer = 1u << OOB_ANSWER(request), info = 0;

	put(&gmu, "host2gmu_intr_set", GMU_HOST2GMU_INTR_SET, 1u << request);
	for (int i = 0; i < 1000 && !(info & answer); i++) {
		sleep_us(100);
		info = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
	}
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, answer);
	if (!(info & answer)) {
		printf("adreno: %u: the GMU does not answer the request %s (gmu2host_intr_info %#x, fw_init_result %#x)\n", step,
		    what, info, rd(gmu.base + GMU_CM3_FW_INIT_RESULT));
		space_faulted(&gmu_space);
		return false;
	}
	printf("adreno: %u: the GMU answers the request %s\n", step, what);
	return true;
}

static void
oob_clear(uint32_t request)
{
	put(&gmu, "host2gmu_intr_set", GMU_HOST2GMU_INTR_SET, 1u << OOB_ANSWER(request));
}

/*
 * A message is words: the first says which (bits 7:0), how many words (15:8), a request (19:16
 * = 0) and a number (31:20); the GMU's answer (19:16 = 2 for a legacy firmware) repeats the
 * request's first word as its second, and its third is the verdict, 0 for done. linux
 * a6xx_hfi.c with gmu->legacy: no rounding of queue positions to four words.
 */
#define HFI_INIT                0
#define HFI_FW_VERSION          1
#define HFI_BW_TABLE            3
#define HFI_PERF_TABLE          4
#define HFI_TEST                5
#define HFI_ERROR               100
#define HFI_ANSWER_V1           2

struct hfi_perf_table_v1 {
	uint32_t header, gpu_levels, gmu_levels;
	struct {
		uint32_t vote, khz;
	} gpu[16], gmu[4];
};
struct hfi_bw_table {
	uint32_t header, levels, cnoc_commands, ddr_commands, cnoc_wait, ddr_wait;
	uint32_t cnoc_address[6], cnoc_data[2][6], ddr_address[8], ddr_data[16][8];
};

static uint32_t hfi_number;

/* The 3D core's power switch, which the GMU's firmware operates. */
static bool
gx_on(void)
{
	return (rd(gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON) != 0;
}

/* Everything waiting in a queue of the GMU's; true if the answer to the message numbered `number` was among it. */
static bool
hfi_receive(unsigned which, uint32_t number, uint32_t *answer, uint32_t answer_words)
{
	volatile struct hfi_queue *queue = &hfi->queue[which];
	volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(which));
	bool answered = false;

	while (queue->read != queue->write) {
		uint32_t message[8] = { 0 }, at = queue->read % HFI_QUEUE_WORDS, words = data[at] >> 8 & 0xff;

		if (words == 0) {
			queue->read = queue->write;
			break;
		}
		for (uint32_t i = 0; i < words; i++) {
			if (i < 8) {
				message[i] = data[at];
			}
			at = (at + 1) % HFI_QUEUE_WORDS;
		}
		queue->read = at;
		if ((message[0] >> 16 & 0xf) == HFI_ANSWER_V1 && message[1] >> 20 == number) {
			for (uint32_t i = 0; i < answer_words && i + 2 < 8; i++) {
				answer[i] = message[i + 2];
			}
			answered = true;
		} else if ((message[0] & 0xff) == HFI_ERROR) {
			printf("adreno: %u: THE GMU REPORTS AN ERROR: %#x %#x %#x\n", step, message[1], message[2], message[3]);
		} else {
			printf("adreno: %u: the GMU says (queue %u, %u words): %#x %#x %#x %#x %#x %#x\n", step, which, words,
			    message[0], message[1], message[2], message[3], message[4], message[5]);
		}
	}
	return answered;
}

/* Send a message whose first word is yet to be made; the GMU's answer (its verdict first) into `answer`. */
static bool
hfi_send(const char *what, uint32_t id, uint32_t *message, uint32_t words, uint32_t *answer, uint32_t answer_words)
{
	volatile struct hfi_queue *queue = &hfi->queue[0];
	volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(0));
	uint32_t at = queue->write % HFI_QUEUE_WORDS, number = ++hfi_number & 0xfff, verdict[4] = { ~0u, 0, 0, 0 };
	bool answered = false;

	message[0] = id | words << 8 | number << 20;
	printf("adreno: %u: about to send the GMU %s: message %u, %u words, number %u\n", step, what, id, words, number);
	snprintf(last_write, sizeof(last_write), "step %u sent the GMU message %u (%s)", step, id, what);
	hold(true);
	for (uint32_t i = 0; i < words; i++) {
		data[at] = message[i];
		at = (at + 1) % HFI_QUEUE_WORDS;
	}
	settle();
	queue->write = at;
	settle();
	wr(gmu.base + GMU_HOST2GMU_INTR_SET, 1);
	for (int i = 0; i < 10000 && !answered; i++) {
		sleep_us(100);
		if (rd(gmu.base + GMU_GMU2HOST_INTR_INFO) & 1) {
			wr(gmu.base + GMU_GMU2HOST_INTR_CLR, 1);
			answered = hfi_receive(1, number, verdict, 4);
		}
	}
	hfi_receive(2, ~0u, verdict + 3, 0);
	if (!answered || verdict[0] != 0) {
		printf("adreno: %u: the GMU %s (%#x) to %s; fw_init_result %#x, gmu2host_intr_info %#x\n", step,
		    answered ? "says no" : "does not answer", verdict[0], what, rd(gmu.base + GMU_CM3_FW_INIT_RESULT),
		    rd(gmu.base + GMU_GMU2HOST_INTR_INFO));
		space_faulted(&gmu_space);
		return false;
	}
	for (uint32_t i = 0; i < answer_words && i < 3; i++) {
		answer[i] = verdict[i + 1];
	}
	printf("adreno: %u: the GMU accepts %s\n", step, what);
	return true;
}

/*
 * A vote for a level of a supply (linux a6xx_gmu_rpmh_arc_votes_init): the level's place in
 * RPMh's list for the rail, the place of the first level of the memories' rail ("mx") that is
 * at least as high (or the highest there is), and the rail's level itself.
 */
static bool
rail_vote(uint32_t level, const uint16_t *rail, unsigned rail_levels, const uint16_t *mx, unsigned mx_levels, uint32_t *vote)
{
	unsigned at, with = 0;

	for (at = 0; at < rail_levels && rail[at] < level; at++) {
	}
	if (at == rail_levels || mx_levels == 0) {
		return false;
	}
	for (unsigned j = 0; j < mx_levels; j++) {
		if (mx[j] >= level) {
			with = j;
			break;
		} else if (mx[j]) {
			with = j;
		}
	}
	*vote = (uint32_t)rail[at] << 16 | with << 8 | at;
	return true;
}

/*
 * The speeds the 3D core is offered, low ones only for now, with their RPMh levels (linux
 * sm7150.dtsi gpu opp-table: 180 MHz min_svs 48, 267 low_svs 64, 355 svs 128), and the GMU's own
 * (200 MHz at min_svs). Index 0 is "off", as Linux and the vendor's driver have it.
 */
static const struct {
	uint32_t khz, level;
} gpu_speeds[] = {
	/*
	 * kHz and the 3D core's rail level (RPMh, without the +1 of the dt-bindings) as the Pixel's own
	 * kernel pairs them (msm-4.14 gpucc-sdmmagpie.c, gpu_cc_gx_gfx3d_clk_src rate_max over
	 * vdd_gx_corner): 180 MIN_SVS, 267 LOW_SVS, 355 SVS, 430 SVS_L1, 565 NOM. Every speed bin of
	 * sdmmagpie-gpu.dtsi goes to 565 MHz at least (610 to 825 above it, by bin): 565 is the fastest
	 * that holds for any chip. At 355 MHz CoreAnimation drew 20-24 frames a second.
	 */
	{ 0, 0 }, { 180000, 48 }, { 267000, 64 }, { 355000, 128 }, { 430000, 192 }, { 565000, 256 },
};
#define GPU_SPEEDS              ((uint32_t)(sizeof(gpu_speeds) / sizeof(gpu_speeds[0])))
static uint32_t gpu_votes[GPU_SPEEDS], gpu_speed = GPU_SPEEDS - 1;

static bool
step_gmu_queues(void)
{
	static struct hfi_perf_table_v1 perf;
	static struct hfi_bw_table bw;
	uint32_t init[5] = { 0, 0, GMU_DUMP_ADDRESS, GMU_DUMP_SIZE, 1 /* a cold boot */ };
	uint32_t version[2] = { 0, 1u << 28 | 8u << 16 };
	uint32_t test[1] = { 0 }, answer[3] = { 0, 0, 0 }, speed = 0;
	uint16_t gfx[16], cx[16], mx[16], unused[16];
	unsigned gfx_levels, cx_levels, mx_levels, n;

	if (!cx_ready() || !gmu_runs) {
		return false;
	}
	if (!rpmh_resource("gfx.lvl", gfx, &gfx_levels) || !rpmh_resource("cx.lvl", cx, &cx_levels) ||
	    !rpmh_resource("mx.lvl", mx, &mx_levels)) {
		printf("adreno: %u: RPMh's list lacks one of the rails\n", step);
		return false;
	}
	for (uint32_t i = 0; i < GPU_SPEEDS; i++) {
		if (!rail_vote(gpu_speeds[i].level, gfx, gfx_levels, mx, mx_levels, &gpu_votes[i])) {
			printf("adreno: %u: RPMh has no level %u for the 3D core's rail\n", step, gpu_speeds[i].level);
			return false;
		}
		perf.gpu[i].vote = gpu_votes[i];
		perf.gpu[i].khz = gpu_speeds[i].khz;
	}
	perf.gpu_levels = GPU_SPEEDS;
	perf.gmu_levels = 2;
	perf.gmu[1].khz = 200000;
	if (!rail_vote(0, cx, cx_levels, mx, mx_levels, &perf.gmu[0].vote) ||
	    !rail_vote(48, cx, cx_levels, mx, mx_levels, &perf.gmu[1].vote)) {
		return false;
	}
	printf("adreno: %u: votes: 3D core %#x %#x %#x %#x, GMU %#x %#x\n", step, perf.gpu[0].vote, perf.gpu[1].vote,
	    perf.gpu[2].vote, perf.gpu[3].vote, perf.gmu[0].vote, perf.gmu[1].vote);

	/* linux a618_build_bw_table: one "off" level, the GMU of the A618 does not scale the bus; addresses by name */
	bw.levels = 1;
	bw.ddr_commands = 3;
	bw.ddr_wait = 1;
	bw.ddr_address[0] = rpmh_resource("MC0", unused, &n);
	bw.ddr_address[1] = rpmh_resource("SH0", unused, &n);
	bw.ddr_address[2] = rpmh_resource("ACV", unused, &n);
	bw.ddr_data[0][0] = bw.ddr_data[0][1] = bw.ddr_data[0][2] = 0x40000000;
	bw.cnoc_commands = 1;
	bw.cnoc_wait = 1;
	bw.cnoc_address[0] = rpmh_resource("CN0", unused, &n);
	bw.cnoc_data[0][0] = 0x40000000;
	bw.cnoc_data[1][0] = 0x60000001;
	printf("adreno: %u: RPMh's bus resources: MC0 %#x, SH0 %#x, ACV %#x, CN0 %#x\n", step, bw.ddr_address[0],
	    bw.ddr_address[1], bw.ddr_address[2], bw.cnoc_address[0]);
	if (bw.ddr_address[0] == 0 || bw.ddr_address[1] == 0 || bw.ddr_address[2] == 0 || bw.cnoc_address[0] == 0) {
		return false;
	}
	if (PE_parse_boot_argn("adreno_speed", &speed, sizeof(speed)) && speed >= 1 && speed < GPU_SPEEDS) {
		gpu_speed = speed;
	}

	/* msm-4.14 a6xx_gmu_fw_start after a6xx_gmu_start, for HFI_USE_REG (sdmmagpie-gpu.dtsi has the quirk) */
	announce("tell the GMU the boot sequence has begun and which level the 3D core's rail starts at (\"boot\")");
	put(&gmu, "boot_slumber_option", GMU_BOOT_SLUMBER_OPTION, 0);
	put(&gmu, "gx_vote_idx", GMU_GX_VOTE_IDX, gpu_votes[gpu_speed] & 0xff);
	put(&gmu, "mx_vote_idx", GMU_MX_VOTE_IDX, gpu_votes[gpu_speed] >> 8 & 0xff);
	if (!oob_set("\"boot\"", OOB_BOOT_SLUMBER)) {
		return false;
	}
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	/*
	 * On ios-surya (same GPU, same firmware) this request alone, with no message queues at all,
	 * powered the 3D core (gx_gdsc on) and its command processor later ran commands. So from here
	 * a failure is said and the steps go on whenever the core has power: `gmu_talks` says the GMU
	 * is up either way, and step 17 checks the power switch itself.
	 */
	gmu_talks = true;
	/* a6xx_sptprac_enable: the shader processors' and texture units' power, ours to switch while the GMU idles none */
	put(&gmu, "gx_sptprac_power_control", GMU_GX_SPTPRAC_POWER_CONTROL, 0x778000);
	if (!wait_for(&gmu, GMU_SPTPRAC_PWR_CLK_STATUS, 0x38, 0x28, 10)) {
		printf("adreno: %u: the shader processors' power does not come on (%#x); going on\n", step,
		    rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS));
	}
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);

	announce("ask the GMU to start its message queues");
	put(&gmu, "hfi_ctrl_init", GMU_HFI_CTRL_INIT, 1);
	if (!wait_for(&gmu, GMU_HFI_CTRL_STATUS, 1, 1, 100)) {
		get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
		space_faulted(&gmu_space);
		printf("adreno: %u: the GMU does not start its message queues; %s\n", step, gx_on() ?
		    "THE 3D CORE HAS POWER ALL THE SAME (as on ios-surya): going on without them" : "and the 3D core has no power");
		return gx_on();
	}

	/* linux a6xx_hfi_start_v1; msm-4.14 hfi_start with HFI version 1 */
	announce("send the GMU \"init\", ask its version, send the tables of speeds and of bus votes, and \"test\" (no more "
	    "messages until the next start)");
	if (!hfi_send("\"init\"", HFI_INIT, init, 5, answer, 0) ||
	    !hfi_send("the question of its version", HFI_FW_VERSION, version, 2, answer, 1)) {
		printf("adreno: %u: no messages with the GMU; %s\n", step, gx_on() ?
		    "THE 3D CORE HAS POWER ALL THE SAME: going on without them" : "and the 3D core has no power");
		return gx_on();
	}
	printf("adreno: %u: the GMU's firmware is version %u.%u.%u (the vendor's driver expects 1.8)\n", step,
	    answer[0] >> 28, answer[0] >> 16 & 0xfff, answer[0] & 0xffff);
	if (!hfi_send("the table of speeds", HFI_PERF_TABLE, &perf.header, sizeof(perf) / 4, answer, 0) ||
	    !hfi_send("the table of bus votes", HFI_BW_TABLE, &bw.header, sizeof(bw) / 4, answer, 0) ||
	    !hfi_send("\"test\"", HFI_TEST, test, 1, answer, 0)) {
		printf("adreno: %u: the GMU does not take its tables; %s\n", step, gx_on() ?
		    "THE 3D CORE HAS POWER ALL THE SAME: going on" : "and the 3D core has no power");
		return gx_on();
	}
	printf("adreno: %u: THE GMU'S MESSAGE QUEUES ARE UP\n", step);
	return !space_faulted(&gmu_space);
}

/* ---- step 17: the 3D core powered ------------------------------------------------------------------------ */

/* dt: kgsl_3d0_reg_memory: the 3D core's registers; a6xx.xml counts in words */
static struct block gpu = { "gpu", 0x05000000, 0x40000 };
#define GPU_WORD(n)             (4u * (n))
#define RBBM_VBIF_CLIENT_QOS_CNTL GPU_WORD(0x0010)
#define RBBM_GBIF_HALT          GPU_WORD(0x0016)
#define RBBM_GBIF_HALT_ACK      GPU_WORD(0x0017)
#define RBBM_INTERFACE_HANG_INT_CNTL GPU_WORD(0x001f)
#define RBBM_INT_0_STATUS       GPU_WORD(0x0201)
#define RBBM_STATUS             GPU_WORD(0x0210)
#define RBBM_STATUS_BUSY        (1u << 23)      /* GPU_BUSY_IGN_AHB: linux a6xx_idle */
#define RBBM_PERFCTR_CNTL       GPU_WORD(0x0500)
#define RBBM_PERFCTR_GPU_BUSY_MASKED GPU_WORD(0x050b)
#define RBBM_NC_MODE_CNTL       GPU_WORD(0x0534)
#define CP_RB_BASE              GPU_WORD(0x0800)
#define CP_RB_CNTL              GPU_WORD(0x0802)
#define CP_RB_RPTR              GPU_WORD(0x0806)
#define CP_RB_WPTR              GPU_WORD(0x0807)
#define CP_SQE_CNTL             GPU_WORD(0x0808)
#define CP_HW_FAULT             GPU_WORD(0x0821)
#define CP_INTERRUPT_STATUS     GPU_WORD(0x0823)
#define CP_PROTECT_STATUS       GPU_WORD(0x0824)
#define CP_SQE_INSTR_BASE       GPU_WORD(0x0830)
#define CP_ROQ_THRESHOLDS_1     GPU_WORD(0x08c1)
#define CP_ROQ_THRESHOLDS_2     GPU_WORD(0x08c2)
#define CP_MEM_POOL_SIZE        GPU_WORD(0x08c3)
#define CP_PERFCTR_CP_SEL_0     GPU_WORD(0x08d0)
#define CP_AHB_CNTL             GPU_WORD(0x098d)
#define UCHE_MODE_CNTL          GPU_WORD(0x0e01)
#define UCHE_WRITE_RANGE_MAX    GPU_WORD(0x0e05)
#define UCHE_WRITE_THRU_BASE    GPU_WORD(0x0e07)
#define UCHE_TRAP_BASE          GPU_WORD(0x0e09)
#define UCHE_GMEM_RANGE_MIN     GPU_WORD(0x0e0b)
#define UCHE_GMEM_RANGE_MAX     GPU_WORD(0x0e0d)
#define UCHE_CACHE_WAYS         GPU_WORD(0x0e17)
#define UCHE_FILTER_CNTL        GPU_WORD(0x0e18)
#define UCHE_CLIENT_PF          GPU_WORD(0x0e19)
#define RB_NC_MODE_CNTL         GPU_WORD(0x8e08)
#define PC_DBG_ECO_CNTL         GPU_WORD(0x9e00)
#define SP_NC_MODE_CNTL         GPU_WORD(0xae02)
#define TPL1_NC_MODE_CNTL       GPU_WORD(0xb604)
#define RBBM_SECVID_TSB_TRUSTED_BASE GPU_WORD(0xf800)
#define RBBM_SECVID_TSB_TRUSTED_SIZE GPU_WORD(0xf802)
#define RBBM_SECVID_TSB_CNTL    GPU_WORD(0xf803)
#define RBBM_SECVID_TRUST_CNTL  GPU_WORD(0xf400)

/*
 * linux a6xx_hw_init begins by asking the GMU to keep the 3D core powered (oob GPU_SET); with
 * the GMU of the A618 that is also what powers it (its firmware switches gx_gdsc: gpucc-sc7180.c
 * leaves that switch to it). Then the speed, as the legacy way asks for it (a6xx_gmu_set_freq:
 * registers and the DCVS request).
 */
static bool
step_core_power(void)
{
	uint32_t returned;

	if (!cx_ready() || !gmu_talks) {
		return false;
	}
	announce("ask the GMU to power the 3D core and keep it powered");
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	core_is_on = true;                      /* asked for, at least: to be given back */
	if (!oob_set("to keep the 3D core powered", OOB_GPU)) {
		if (!gx_on()) {
			return false;
		}
		printf("adreno: %u: the 3D core has power from \"boot\" (step 16): going on\n", step);
	}
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	if (!(rd(gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON)) {
		printf("adreno: %u: the GMU says yes, but the 3D core's power switch is off\n", step);
		return false;
	}

	printf("adreno: %u: the speed asked for: %u MHz (adreno_speed=%u)\n", step, gpu_speeds[gpu_speed].khz / 1000, gpu_speed);
	announce("ask the GMU for that speed (dcvs registers and request)");
	put(&gmu, "dcvs_ack_option", GMU_DCVS_ACK_OPTION, 0);
	put(&gmu, "dcvs_perf_setting", GMU_DCVS_PERF_SETTING, 3u << 28 | gpu_speed);
	put(&gmu, "dcvs_bw_setting", GMU_DCVS_BW_SETTING, 0xff);       /* the firmware's choice of bus vote */
	if (oob_set("for the speed", OOB_DCVS)) {
		oob_clear(OOB_DCVS);
	} else {
		printf("adreno: %u: no speed set: the core runs at whatever the GMU chose; going on\n", step);
	}
	returned = get(&gmu, "dcvs_return", GMU_DCVS_RETURN);
	printf("adreno: %u: measured: the 3D core's clock runs at %u Hz (dcvs_return %d)\n", step,
	    measure_quietly(GPUCC_DEBUG_GX_GFX3D), (int)returned);

	announce("read registers of the 3D core itself (0x5000000): the first time it has power");
	get(&gpu, "rbbm_status", RBBM_STATUS);
	get(&gpu, "rbbm_int_0_status", RBBM_INT_0_STATUS);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	printf("adreno: %u: THE 3D CORE HAS POWER AND ANSWERS\n", step);
	return !space_faulted(&gmu_space);
}

/* ---- step 18: the 3D core's memory and its microcode -------------------------------------------------------- */

/*
 * The 3D core takes its orders as words in a ring of memory, which a small processor inside it
 * ("SQE", microcode a630_sqe.fw) reads up to where a register says we have written. It reaches
 * memory through the SMMU: stream 0, given context 0 here.
 */
#define CORE_STREAM             0
#define CORE_RING_ADDRESS       0x01000000u
#define CORE_RING_SIZE          0x8000u         /* Linux's; 8192 words */
#define CORE_SQE_ADDRESS        0x01100000u
#define CORE_SCRATCH_ADDRESS    0x01200000u
#define CP_RB_CNTL_VALUE        (12u | 2u << 8 | 1u << 27)      /* 2^12 pairs of words, blocks of 32; no read position in memory */

/* commands (Mesa's adreno_pm4.xml); a packet is a header word and so many more */
#define CP_ME_INIT              0x48
#define CP_MEM_WRITE            0x3d
#define CP_SET_SECURE_MODE      0x66
#define CP_SCRATCH_REG0_NUMBER  0x0883
#define CP_SCRATCH_REG0         GPU_WORD(CP_SCRATCH_REG0_NUMBER)

static struct gpu_space core_space = { .name = "the 3D core", .context = 0, .control = SMMU_SCTLR_SERVE_AFTER_FAULT };
static struct gpu_block core_ring, core_sqe, core_scratch;
static bool core_space_is_on, core_runs, zap_is_loaded;
/*
 * boot-arg adreno_trust=1: with no zap shader, leave secure mode by writing RBBM_SECVID_TRUST_CNTL
 * (linux a6xx_gpu.c hw_init does that when a device has no zap shader). The 3D core comes out of
 * reset secure; Mesa's draws came back as zeros while CP_MEM_WRITE (step 21) still landed. Off by
 * default: where the secure world wants a zap shader the write is blocked and Linux warns of a
 * permissions violation (the phone may reset).
 */
static uint32_t trust_wanted;
/*
 * boot-arg adreno_zap_fresh=1: the zap shader is loaded as the downstream Pixel kernel does it
 * (subsys-pil-tz.c, peripheral-loader.c): init_image, mem_setup, the image, auth_and_reset, once,
 * without asking TrustZone first to shut down an image that never ran. With that shutdown, and
 * after step 12's own init_image, PAS_MEM_SETUP came back -3145798. (adreno_skip=4096 leaves out
 * step 12.)
 */
static uint32_t zap_fresh;

static uint32_t core_match;             /* its entry of the stream table */
#define RING_WORDS              (CORE_RING_SIZE / 4)
static uint32_t ring_at, ring_sent;

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static void
ring(uint32_t word)
{
	((uint32_t *)core_ring.memory)[ring_at] = word;
	ring_at = (ring_at + 1) % RING_WORDS;
}

/* The header of a command with so many words after it (a "type 7" packet). */
static void
ring_command(uint32_t command, uint32_t words)
{
	ring(7u << 28 | words | odd_parity(words) << 15 | command << 16 | odd_parity(command) << 23);
}

/* A register of the 3D core (by its word number) written by command, in the order of the other commands. */
static void
ring_register(uint32_t number, uint32_t value)
{
	ring(4u << 28 | 1 | odd_parity(1) << 7 | number << 8 | odd_parity(number) << 27);
	ring(value);
}

static void
ring_send(void)
{
	if (ring_at >= ring_sent) {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (ring_at - ring_sent));
	} else {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (RING_WORDS - ring_sent));
		to_memory(core_ring.memory, 4 * ring_at);
	}
	ring_sent = ring_at;
	wr(gpu.base + CP_RB_WPTR, ring_at);
}

/* Let the 3D core have what is in the ring, and wait until it has taken all of it and rests. */
static bool
ring_run(const char *what)
{
	uint32_t taken = ~0u, status = ~0u;

	printf("adreno: %u: about to have the 3D core %s: the ring up to word %u\n", step, what, ring_at);
	snprintf(last_write, sizeof(last_write), "step %u had the 3D core %s", step, what);
	hold(true);
	ring_send();
	for (int i = 0; i < 20000 && (taken != ring_at || (status & RBBM_STATUS_BUSY)); i++) {
		sleep_us(100);
		taken = rd(gpu.base + CP_RB_RPTR);
		status = rd(gpu.base + RBBM_STATUS);
	}
	if (taken == ring_at && !(status & RBBM_STATUS_BUSY) && rd(gpu.base + CP_HW_FAULT) == 0 && !space_faulted(&core_space)) {
		printf("adreno: %u: the 3D core has done it (ring read up to %u, status %#x)\n", step, taken, status);
		return true;
	}
	printf("adreno: %u: the 3D core does not get through: ring read up to %u of %u, status %#x, interrupts %#x, "
	    "command processor: fault %#x, interrupts %#x, protection %#x\n", step, taken, ring_at, status,
	    rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS),
	    rd(gpu.base + CP_PROTECT_STATUS));
	return false;
}

static void
put64(struct block *b, const char *name, uint32_t offset, uint64_t value)
{
	put(b, name, offset, (uint32_t)value);
	put(b, name, offset + 4, (uint32_t)(value >> 32));
}

static bool
step_core_space(void)
{
	const uint8_t *microcode;
	uint64_t size = 0;

	if (!cx_ready() || !core_is_on) {
		return false;
	}
	announce("make the 3D core's memory: its ring, its microcode (" FW_DIRECTORY FW_SQE "), a page to try writing to");
	microcode = fw_file(FW_SQE, &size);
	if (microcode == NULL || size < 8 || size > 0x40000 ||
	    !block_new(&core_space, &core_ring, CORE_RING_ADDRESS, CORE_RING_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_sqe, CORE_SQE_ADDRESS, (uint32_t)size - 4, PTE_CORE | PTE_READ_ONLY) ||
	    !block_new(&core_space, &core_scratch, CORE_SCRATCH_ADDRESS, 4096, PTE_CORE)) {
		return false;
	}
	memcpy((void *)core_sqe.memory, microcode + 4, size - 4);       /* the file's first word is not microcode */
	to_memory(core_sqe.memory, core_sqe.size);

	announce("give the SMMU's context 0 these tables and send the 3D core's requests (stream 0) through it");
	core_match = smmu_match_for(CORE_STREAM, gmu_match == 0 ? 1 : 0);
	core_space_is_on = true;
	if (!space_on(&core_space, core_match, CORE_STREAM)) {
		return false;
	}
	get(&gpu, "rbbm_status", RBBM_STATUS);
	return !space_faulted(&core_space);
}

/* ---- step 19: the zap shader, loaded -------------------------------------------------------------------- */

/*
 * The rest of what Linux does with the zap shader (mdt_loader.c qcom_mdt_load, adreno_gpu.c
 * zap_shader_load_mdt): after the header, TrustZone is told where the image will be -- the
 * memory the device tree reserves for it (sunfish: pil_gpu_mem 0x98f95000, 0x2000, the
 * zap-shader node's memory-region) --, the image's loadable segments (files a615_zap.bNN) are
 * put there, relocated to its start, and TrustZone is asked to authenticate and run it. The
 * vendor's driver does this before it starts the 3D core's command processor; that order is
 * kept. A kernel before this one may have left a shader loaded: TrustZone is first asked to give
 * it up, as the vendor's driver does when it is unloaded.
 */
#define ZAP_MEMORY              0x98f95000ull
#define ZAP_MEMORY_SIZE         0x2000u
#define ELF_PT_LOAD             1
static uint32_t zap_wanted = 1;         /* boot-arg adreno_zap=0: none (ios-surya's command processor ran without one) */
static bool zap_load(void);
#define MDT_TYPE_HASH           (2u << 24)      /* of a segment's flags (qcom_mdt_loader: QCOM_MDT_TYPE_MASK) */

static bool
zap_unload(void)
{
	uint64_t answer = ~0ull;

	return scm("to give up the zap shader", SCM_64 | SCM_SIP | SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_GPU, 0, 0, &answer) == 0 &&
	       answer == 0;
}

struct elf32_phdr {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

static bool
loadable(const struct elf32_phdr *segment)
{
	return segment->type == ELF_PT_LOAD && (segment->flags & (7u << 24)) != MDT_TYPE_HASH && segment->memsz != 0;
}

/*
 * Whether there is a zap shader or not, the steps go on: ios-surya's A618 (same SoC, same
 * firmware) ran its command processor's commands with none ever loaded, so this kernel may well
 * do without TrustZone here. Step 21 then leaves secure mode only if a zap shader is there to
 * ask, and otherwise just tries the memory write.
 */
static bool
step_zap_load(void)
{
	if (!cx_ready() || !core_is_on) {
		return false;
	}
	if (!zap_wanted) {
		printf("adreno: %u: no zap shader (adreno_zap=0): the 3D core stays as it starts\n", step);
		return true;
	}
	if (!zap_load()) {
		printf("adreno: %u: NO ZAP SHADER; going on without one, as ios-surya's A618 runs commands without one\n", step);
	}
	return true;
}

static bool
zap_load(void)
{
	struct elf32_phdr segment;
	volatile uint8_t *memory;
	uint64_t header = 0, answer = ~0ull, low = ~0ull, high = 0;
	uint32_t segments;

	if (!cx_ready() || !core_is_on) {
		return false;
	}
	/*
	 * linux qcom_scm.c votes the crypto engine's path to memory around every PAS call; without it our
	 * PAS_MEM_SETUP failed (-3145798). Left on: the GPU keeps the memory busy anyway.
	 */
	announce("vote the crypto engine's path to memory (CE0, SN11, SN0; MC0, SH0, ACV), as Linux does for the PAS calls");
	printf("adreno: %u: crypto vote %d, memory vote %d\n", step, hakios_rpmh_crypto_vote(0x3fff),
	    hakios_rpmh_ddr_vote(0x3fff, true));
	if (!zap_fresh) {
		announce("ask TrustZone to give up a zap shader that a kernel before this one may have left");
		printf("adreno: %u: %s\n", step, zap_unload() ? "TrustZone has given up what it had (it says so with none, too)" :
		    "TrustZone does not give it up: the next answers tell whether it keeps one");
	}
	if (!zap_header(&header)) {
		return false;
	}
	segments = zap_mdt[44] | zap_mdt[45] << 8;
	for (uint32_t i = 0; i < segments; i++) {
		memcpy(&segment, zap_mdt + 52 + 32 * i, sizeof(segment));
		if (loadable(&segment)) {
			low = MIN(low, segment.paddr);
			/* ending on a 4K page, as ios-topaz's zap_load (working on SM6225's TrustZone): PAS_MEM_SETUP
			 * with the bare 0x7c0 came back -3145798 here */
			high = MAX(high, ((uint64_t)segment.paddr + segment.memsz + 0xfff) & ~0xfffull);
		}
	}
	if (high <= low || high - low > ZAP_MEMORY_SIZE) {
		printf("adreno: %u: the image's segments span %#llx..%#llx: not within the %#x bytes reserved for it\n", step,
		    low, high, ZAP_MEMORY_SIZE);
		return false;
	}

	announce("hand the zap shader's header to TrustZone, tell it where the image goes, put the image there, and ask "
	    "TrustZone to authenticate and run it");
	if (scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
	    header, 0, &answer) != 0 || answer != 0) {
		printf("adreno: %u: TrustZone does not take the signature (%#llx): taken to mean that it keeps a zap shader from "
		    "before, which is used as it is\n", step, answer);
		zap_is_loaded = true;
		return true;
	}
	if (scm("where the image will be", SCM_64 | SCM_SIP | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_GPU, ZAP_MEMORY,
	    high - low, &answer) != 0 || answer != 0) {
		printf("adreno: %u: TrustZone does not take the place (%#llx)\n", step, answer);
		return false;
	}
	memory = (volatile uint8_t *)ml_io_map_wcomb(ZAP_MEMORY, ZAP_MEMORY_SIZE);
	for (uint32_t i = 0; i < ZAP_MEMORY_SIZE; i++) {
		memory[i] = 0;
	}
	for (uint32_t i = 0; i < segments; i++) {
		char name[24];
		const uint8_t *data;
		uint64_t size = 0;

		memcpy(&segment, zap_mdt + 52 + 32 * i, sizeof(segment));
		if (!loadable(&segment) || segment.filesz == 0) {
			continue;
		}
		snprintf(name, sizeof(name), FW_ZAP ".b%02u", i);
		data = fw_file(name, &size);
		if (data == NULL || size < segment.filesz || segment.filesz > segment.memsz) {
			return false;
		}
		printf("adreno: %u: about to write %u bytes of %s at %#llx\n", step, segment.filesz, name,
		    ZAP_MEMORY + segment.paddr - low);
		snprintf(last_write, sizeof(last_write), "step %u wrote the zap shader into memory at %#llx", step, ZAP_MEMORY);
		hold(true);
		for (uint32_t b = 0; b < segment.filesz; b++) {
			memory[segment.paddr - low + b] = data[b];
		}
	}
	settle();
	if (scm("to authenticate the zap shader and run it", SCM_64 | SCM_SIP | SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_GPU, 0,
	    0, &answer) != 0 || answer != 0) {
		printf("adreno: %u: TrustZone does not authenticate the zap shader (%#llx)\n", step, answer);
		return false;
	}
	printf("adreno: %u: TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER\n", step);
	zap_is_loaded = true;
	return true;
}

/* ---- step 20: the 3D core's registers, its command processor started, its first command ------------------- */

/*
 * linux a6xx_gpu.c hw_init with the A618's values (a6xx_catalog.c: prim_fifo_threshold, the
 * defaults of the A615 family; UBWC 2.0 with highest bank bit 14: ubwc_config.c for sm7150), less
 * clock gating, the protection of registers against user programs and interrupts: those matter
 * once programs draw.
 */
static bool
step_core_start(void)
{
	static const uint32_t address_modes[] = { 0x0842, 0x0c01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601, 0x0e00, 0xae01,
		                                  0xb601, 0xf810 };

	if (!cx_ready() || !core_is_on || !core_space_is_on) {
		return false;
	}
	announce("write the 3D core's registers as Linux does before it starts the command processor: the first writes "
	    "to the core itself");
	put(&gbif, "halt", GBIF_HALT, 0);
	put(&gpu, "rbbm_gbif_halt", RBBM_GBIF_HALT, 0);
	put(&gpu, "rbbm_secvid_tsb_cntl", RBBM_SECVID_TSB_CNTL, 0);
	put64(&gpu, "rbbm_secvid_tsb_trusted_base", RBBM_SECVID_TSB_TRUSTED_BASE, 0);
	put(&gpu, "rbbm_secvid_tsb_trusted_size", RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
	for (unsigned i = 0; i < sizeof(address_modes) / sizeof(address_modes[0]); i++) {
		put(&gpu, "address mode: 64 bits", GPU_WORD(address_modes[i]), 1);
	}
	put(&gpu, "rbbm_vbif_client_qos_cntl", RBBM_VBIF_CLIENT_QOS_CNTL, 3);
	put(&gpu, "rbbm_perfctr_gpu_busy_masked", RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
	put64(&gpu, "uche_write_range_max", UCHE_WRITE_RANGE_MAX, 0x1fffffffff000ull + 0xfc0);
	put64(&gpu, "uche_trap_base", UCHE_TRAP_BASE, 0x1fffffffff000ull);
	put64(&gpu, "uche_write_thru_base", UCHE_WRITE_THRU_BASE, 0x1fffffffff000ull);
	put64(&gpu, "uche_gmem_range_min", UCHE_GMEM_RANGE_MIN, 0x100000);
	put64(&gpu, "uche_gmem_range_max", UCHE_GMEM_RANGE_MAX, 0x100000 + 0x80000 - 1);       /* 512K of GMEM */
	put(&gpu, "uche_filter_cntl", UCHE_FILTER_CNTL, 0x804);
	put(&gpu, "uche_cache_ways", UCHE_CACHE_WAYS, 0x4);
	put(&gpu, "cp_roq_thresholds_2", CP_ROQ_THRESHOLDS_2, 0x010000c0);
	put(&gpu, "cp_roq_thresholds_1", CP_ROQ_THRESHOLDS_1, 0x8040362c);
	put(&gpu, "cp_mem_pool_size", CP_MEM_POOL_SIZE, 128);
	put(&gpu, "pc_dbg_eco_cntl", PC_DBG_ECO_CNTL, 0x00180000);
	put(&gpu, "cp_ahb_cntl", CP_AHB_CNTL, 1);
	put(&gpu, "rbbm_perfctr_cntl", RBBM_PERFCTR_CNTL, 1);
	put(&gpu, "cp_perfctr_cp_sel_0", CP_PERFCTR_CP_SEL_0, 0);
	put(&gpu, "rb_nc_mode_cntl", RB_NC_MODE_CNTL, 1u << 1);
	put(&gpu, "tpl1_nc_mode_cntl", TPL1_NC_MODE_CNTL, 1u << 1);
	put(&gpu, "sp_nc_mode_cntl", SP_NC_MODE_CNTL, 1u << 1);
	put(&gpu, "uche_mode_cntl", UCHE_MODE_CNTL, 1u << 21);
	put(&gpu, "rbbm_nc_mode_cntl", RBBM_NC_MODE_CNTL, 0);
	put(&gpu, "rbbm_interface_hang_int_cntl", RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0x1fffff);
	put(&gpu, "uche_client_pf", UCHE_CLIENT_PF, 1u << 7 | 1);
	put64(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE, CORE_SQE_ADDRESS);
	put64(&gpu, "cp_rb_base", CP_RB_BASE, CORE_RING_ADDRESS);
	put(&gpu, "cp_rb_cntl", CP_RB_CNTL, CP_RB_CNTL_VALUE);
	get(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	get(&gpu, "rbbm_status", RBBM_STATUS);

	announce("start the 3D core's command processor");
	core_runs = true;
	put(&gpu, "cp_sqe_cntl", CP_SQE_CNTL, 1);
	sleep_ms(20);
	printf("adreno: %u: alive after it; status %#x, interrupts %#x, command processor: fault %#x, interrupts %#x, "
	    "protection %#x, ring read up to %u\n", step, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
	    rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS),
	    rd(gpu.base + CP_RB_RPTR));
	if (space_faulted(&core_space)) {
		return false;
	}

	announce("give the 3D core its first command, \"initialize\"");
	ring_command(CP_ME_INIT, 8);            /* a6xx_cp_init */
	ring(0x0000002f);
	ring(0x00000003);                       /* several hardware contexts */
	ring(0x20000000);                       /* error detection */
	ring(0);
	ring(0);
	ring(0);
	ring(0);
	ring(0);
	if (!ring_run("initialize its command processor")) {
		return false;
	}
	/* ios-surya's proof on this GPU: a register written by command, read back here */
	wr(gpu.base + CP_SCRATCH_REG0, 0);
	ring_register(CP_SCRATCH_REG0_NUMBER, 0x1337beef);
	if (!ring_run("write a register") || rd(gpu.base + CP_SCRATCH_REG0) != 0x1337beef) {
		printf("adreno: %u: cp_scratch_reg0 reads %#x, not the 0x1337beef it was told to write\n", step,
		    rd(gpu.base + CP_SCRATCH_REG0));
		return false;
	}
	printf("adreno: %u: THE 3D CORE TAKES COMMANDS (cp_scratch_reg0 = 0x1337beef, written by command)\n", step);
	return true;
}

/* ---- step 21: out of secure mode; a word written on command ------------------------------------------------ */

static bool
step_core_commands(void)
{
	volatile uint32_t *scratch = (volatile uint32_t *)core_scratch.memory;

	if (!cx_ready() || !core_runs) {
		return false;
	}
	if (zap_is_loaded) {
		announce("command the 3D core out of secure mode (it asks the zap shader), then to write a word into a page of ours");
		ring_command(CP_SET_SECURE_MODE, 1);
		ring(0);
		if (!ring_run("leave secure mode")) {
			return false;
		}
	} else if (trust_wanted) {
		announce("no zap shader: out of secure mode through RBBM_SECVID_TRUST_CNTL (adreno_trust=1), then write a word");
		put(&gpu, "rbbm_secvid_trust_cntl", RBBM_SECVID_TRUST_CNTL, 0);
		printf("adreno: %u: rbbm_secvid_trust_cntl reads %#x after the write\n", step, rd(gpu.base + RBBM_SECVID_TRUST_CNTL));
	} else {
		announce("no zap shader: command the 3D core to write a word into a page of ours as it is");
	}
	ring_command(CP_MEM_WRITE, 3);
	ring(CORE_SCRATCH_ADDRESS);
	ring(0);
	ring(0x600dc0de);
	if (!ring_run("write a word to memory")) {
		return false;
	}
	to_memory(core_scratch.memory, 64);
	printf("adreno: %u: the page reads %#x: %s\n", step, scratch[0], scratch[0] == 0x600dc0de ?
	    "THE 3D CORE WRITES OUR MEMORY ON COMMAND" : "not what it was told to write");
	return scratch[0] == 0x600dc0de;
}

/* ---- the way back ------------------------------------------------------------------------------------- */

/*
 * Nothing of the GPU block may outlive the run: the memory its SMMU contexts point into goes
 * back to the kernel. In the order of the vendor's a6xx_gmu_notify_slumber / a6xx_gmu_stop for
 * HFI_USE_REG: the 3D core given back, the GMU told to slumber and stopped, its RPMh channel put
 * to sleep, the zap shader given up; then the streams sent nowhere. (HackiOS's way back, with
 * the legacy GMU's requests.)
 */
static void
core_bus_halt(void)
{
	wr(gbif.base + GBIF_HALT, 1);
	for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 1); i++) {
	}
	wr(gbif.base + GBIF_HALT, 2);
	for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 2); i++) {
	}
	wr(gbif.base + GBIF_HALT, 0);
}

static void
gpu_core_stop(void)
{
	if (core_runs && core_is_on) {
		printf("adreno: the 3D core before it is stopped: status %#x, interrupts %#x, ring read up to %u of %u\n",
		    rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_RB_RPTR), ring_at);
		core_bus_halt();
	}
	if (core_is_on) {
		oob_clear(OOB_GPU);
		core_is_on = false;
	}
	if (gmu_talks) {
		put(&gmu, "power_counter_enable", GMU_POWER_COUNTER_ENABLE, 0);
		put(&gmu, "gx_sptprac_power_control", GMU_GX_SPTPRAC_POWER_CONTROL, 0x778001);
		put(&gmu, "boot_slumber_option", GMU_BOOT_SLUMBER_OPTION, 1);
		if (oob_set("\"slumber\"", OOB_BOOT_SLUMBER)) {
			get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);    /* 0xf: it has */
		}
		oob_clear(OOB_BOOT_SLUMBER);
		put(&gmu, "ao_ahb_fence_ctrl", GMU_AO_AHB_FENCE_CTRL, 0);
		gmu_talks = false;
	} else if (gmu_runs) {
		oob_clear(OOB_BOOT_SLUMBER);    /* (vendor's GMU_FW_STOP for HFI_USE_REG) */
	}
	if (gmu_runs) {
		put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
		if (rd(gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON) {  /* (a GMU that has slumbered has switched it off) */
			put(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR, rd(gpucc.base + GPUCC_GX_GDSCR) | GDSC_SW_COLLAPSE);
			wait_for(&gpucc, GPUCC_GX_GDSCR, GDSC_POWER_ON, 0, 100);
			get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
		}
	}
	if (zap_is_loaded) {
		printf("adreno: the zap shader: %s\n", zap_unload() ? "TrustZone has given it up" :
		    "TrustZone does not give it up; the next start will find it there");
		zap_is_loaded = false;
	}
	if (rsc_awake) {
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 1);
		if (!wait_for(&rscc, RSCC_RSC_STATUS0_DRV0, RSCC_ASLEEP, RSCC_ASLEEP, 100)) {
			printf("adreno: the GMU's RPMh channel does not go to sleep\n");
		}
		get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
		rsc_awake = false;
	}
	if (gmu_space_is_on) {
		space_faulted(&gmu_space);
		space_off(&gmu_space, gmu_match);
		gmu_space_is_on = false;
	}
	if (core_space_is_on) {
		space_faulted(&core_space);
		space_off(&core_space, core_match);
		core_space_is_on = core_runs = false;
	}
}

/* ---- the GPU kept on, for the system to draw with --------------------------------------------------- */

/*
 * With boot-arg adreno_keep=1 and all 21 steps done, nothing is undone: the 3D core stays powered
 * and taking commands, and the service HakiosGPU (iokit/Kernel/arm/HakiosGPU.cpp) hands it to
 * programs through the interface of Linux's msm driver, which Mesa's freedreno speaks. That file
 * keeps the buffers and the clients; what is the GPU's is here: addresses in the 3D core's SMMU
 * context, the ring, and the word in which the core writes the number of each submission it is
 * through with (a "fence"). The caller holds one lock around all of these. Ported from HackiOS
 * (redwood_adreno.c, "the GPU kept on"), less its timing and recovery.
 *
 * One address space for everything the core reads and writes: below GPU_ADDRESSES_START the
 * steps' own blocks at their constants (ring, microcode, scratch page), from there up addresses
 * handed out a buffer at a time, first fit, coalesced when given back (as Linux's msm hands them
 * out: userspace only asks, MSM_INFO_GET_IOVA). Below 4 GB: the A618 has ADRENO_QUIRK_4GB_VA.
 */
#define GPU_CHIP_ID             0x06010800ull   /* dt: qcom,chipid */
#define GPU_ADDRESSES_START     0x20000000ull
#define GPU_ADDRESSES_SIZE      0xe0000000ull
#define GPU_ADDRESS_GRANULE     0x10000ull
#define GPU_FENCE_AT            0x40            /* in the scratch page */
#define CP_SCRATCH_REG2         0x0885
#define CP_INDIRECT_BUFFER      0x3f
#define CP_EVENT_WRITE          0x46
#define CP_WAIT_FOR_IDLE        0x26
#define EVENT_CACHE_FLUSH_TS    0x04
#define EVENT_INVALIDATE_DEPTH  0x18
#define EVENT_INVALIDATE_COLOR  0x19
#define CP_IB1_BASE             GPU_WORD(0x0928)
#define CP_IB1_REM_SIZE         GPU_WORD(0x092a)

extern void HakiosGPUStart(void);
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
void hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_to_memory(void *start, uint64_t size);
bool hakios_gpu_done(uint32_t fence);
uint32_t hakios_gpu_last(void);
uint32_t hakios_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
void hakios_gpu_report(const char *why);

static bool gpu_kept;
static uint32_t gpu_fence;              /* the number of the last submission */
static struct {
	uint64_t start, size;
} gpu_free[1024];                       /* the addresses not in use, in order */
static unsigned gpu_free_count;

bool
hakios_gpu_ready(void)
{
	return gpu_kept;
}

/* The answers to msm's GET_PARAM (linux adreno_get_param for this GPU; the numbers are msm_drm.h's). */
bool
hakios_gpu_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01:                              /* GPU_ID: none, the chip ID says it */
	case 0x08:                              /* PP_PGTABLE */
	case 0x09:                              /* FAULTS */
	case 0x0a:                              /* SUSPENDS */
	case 0x11:                              /* RAYTRACING */
	case 0x13:                              /* MACROTILE_MODE: 4 channels below UBWC 3.1 */
		*value = 0;
		return true;
	case 0x02:                              /* GMEM_SIZE */
		*value = 512 * 1024;
		return true;
	case 0x03:                              /* CHIP_ID, with the speed bin above it */
		*value = GPU_CHIP_ID | (uint64_t)speed_bin << 32;
		return true;
	case 0x04:                              /* MAX_FREQ */
		*value = gpu_speeds[gpu_speed].khz * 1000ull;
		return true;
	case 0x05:                              /* TIMESTAMP: the 19.2 MHz counter, which is this kernel's clock too */
		*value = mach_absolute_time();
		return true;
	case 0x06:                              /* GMEM_BASE: 1 MB below the A650 family (UCHE_GMEM_RANGE_MIN, step 20) */
		*value = 0x100000;
		return true;
	case 0x07:                              /* PRIORITIES */
		*value = 1;
		return true;
	case 0x10:                              /* HIGHEST_BANK_BIT (ubwc_config.c, sm7150) */
		*value = 14;
		return true;
	case 0x12:                              /* UBWC_SWIZZLE: levels 2 and 3 */
		*value = 6;
		return true;
	default:                                /* VA_START, VA_SIZE among them: refused to clients of a shared space, as msm does */
		return false;
	}
}

/* Addresses for a buffer of so many bytes; 0: none left. The buffer's until hakios_gpu_address_free(). */
uint64_t
hakios_gpu_address(uint64_t size)
{
	size = (size + GPU_ADDRESS_GRANULE - 1) & ~(GPU_ADDRESS_GRANULE - 1);
	for (unsigned i = 0; i < gpu_free_count; i++) {
		if (gpu_free[i].size >= size) {
			uint64_t address = gpu_free[i].start;

			gpu_free[i].start += size;
			gpu_free[i].size -= size;
			if (gpu_free[i].size == 0) {
				memmove(&gpu_free[i], &gpu_free[i + 1], (--gpu_free_count - i) * sizeof(gpu_free[0]));
			}
			return address;
		}
	}
	return 0;
}

void
hakios_gpu_address_free(uint64_t address, uint64_t size)
{
	unsigned at;

	size = (size + GPU_ADDRESS_GRANULE - 1) & ~(GPU_ADDRESS_GRANULE - 1);
	for (at = 0; at < gpu_free_count && gpu_free[at].start < address; at++) {
	}
	if (at > 0 && gpu_free[at - 1].start + gpu_free[at - 1].size == address) {
		gpu_free[at - 1].size += size;
		if (at < gpu_free_count && address + size == gpu_free[at].start) {
			gpu_free[at - 1].size += gpu_free[at].size;
			memmove(&gpu_free[at], &gpu_free[at + 1], (--gpu_free_count - at) * sizeof(gpu_free[0]));
		}
	} else if (at < gpu_free_count && address + size == gpu_free[at].start) {
		gpu_free[at].start = address;
		gpu_free[at].size += size;
	} else if (gpu_free_count < sizeof(gpu_free) / sizeof(gpu_free[0])) {
		memmove(&gpu_free[at + 1], &gpu_free[at], (gpu_free_count++ - at) * sizeof(gpu_free[0]));
		gpu_free[at].start = address;
		gpu_free[at].size = size;
	}
	/* (else these addresses are lost: there is no room to remember the hole, and to hand them out unremembered is worse) */
}

/* One 4K page of ours at an address of the 3D core's; after the last of a buffer's, hakios_gpu_mapped(). */
bool
hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return space_map(&core_space, address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}

void
hakios_gpu_mapped(void)
{
	space_commit(&core_space);
}

void
hakios_gpu_unmap(uint64_t address, uint64_t size)
{
	uint32_t context = smmu_context(&core_space);

	for (uint64_t offset = 0; offset < size; offset += 4096) {
		space_map(&core_space, address + offset, 0, 0);
	}
	space_commit(&core_space);
	wr(smmu.base + context + SMMU_CB_TLBIASID, core_space.context);
	wr(smmu.base + context + SMMU_CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(smmu.base + context + SMMU_CB_TLBSTATUS) & 1); i++) {
	}
}

/* Memory that the 3D core and a program both see: out of this processor's cache. */
void
hakios_gpu_to_memory(void *start, uint64_t size)
{
	to_memory((vm_offset_t)start, (vm_size_t)size);
}

/* How far the 3D core has got. */
bool
hakios_gpu_done(uint32_t fence)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);

	to_memory((vm_offset_t)word, 4);
	return (int32_t)(*word - fence) >= 0;
}

uint32_t
hakios_gpu_last(void)
{
	return gpu_fence;
}

/*
 * Command buffers for the 3D core to run, in order: each an address and a count of words, put
 * in the ring as linux a6xx_submit puts them. Returns the submission's number; 0: the ring is
 * full and the core is not reading it.
 */
uint32_t
hakios_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t wanted = 16 + 4 * count, room = 0;

	for (int i = 0; i < 20000; i++) {
		room = (rd(gpu.base + CP_RB_RPTR) + RING_WORDS - ring_at - 1) % RING_WORDS;
		if (room >= wanted) {
			break;
		}
		sleep_us(100);
	}
	if (room < wanted) {
		return 0;
	}
	if (++gpu_fence == 0) {
		gpu_fence = 1;
	}
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_INVALIDATE_DEPTH);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_INVALIDATE_COLOR);
	for (uint32_t i = 0; i < count; i++) {
		ring_command(CP_INDIRECT_BUFFER, 3);
		ring((uint32_t)addresses[i]);
		ring((uint32_t)(addresses[i] >> 32));
		ring(words[i]);
	}
	ring_register(CP_SCRATCH_REG2, gpu_fence);
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_FLUSH_TS);
	ring(CORE_SCRATCH_ADDRESS + GPU_FENCE_AT);
	ring(0);
	ring(gpu_fence);
	ring_send();
	return gpu_fence;
}

/* A submission does not come back: what the 3D core and its SMMU context say. */
void
hakios_gpu_report(const char *why)
{
	printf("adreno: %s: submission %u is the last, the core has done %u; ring read up to %u, written up to %u; status "
	    "%#x, interrupts %#x; command processor: fault %#x, interrupts %#x, protection %#x; in the command buffer at %#llx, "
	    "%u words to go\n", why, gpu_fence, *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT),
	    rd(gpu.base + CP_RB_RPTR), ring_at, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
	    rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS),
	    (uint64_t)rd(gpu.base + CP_IB1_BASE + 4) << 32 | rd(gpu.base + CP_IB1_BASE), rd(gpu.base + CP_IB1_REM_SIZE));
	space_faulted(&core_space);
}

/* After step 21, with adreno_keep=1: the GPU is the system's from here on. */
static bool
gpu_keep(void)
{
	if (!core_runs) {
		return false;
	}
	gpu_free[0].start = GPU_ADDRESSES_START;
	gpu_free[0].size = GPU_ADDRESSES_SIZE;
	gpu_free_count = 1;
	bzero((void *)core_scratch.memory, 4096);
	to_memory(core_scratch.memory, 4096);
	step = 0;
	newest_step = ~0u;
	quiet = true;
	gpu_kept = true;
	printf("adreno: THE GPU STAYS ON for the system to draw with: the service HakiosGPU\n");
	HakiosGPUStart();
	return true;
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static bool(*const steps[])(void) = {
	step_outside, step_fuses, step_gpucc, step_cx_power, step_clocks, step_first_words, step_rscc, step_smmu,
	step_smmu_context, step_gmu_memory, step_trustzone, step_zap_signature, step_gmu_space, step_gmu_firmware,
	step_gmu_start, step_gmu_queues, step_core_power, step_core_space, step_zap_load, step_core_start, step_core_commands,
};
#define STEPS                   ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/* Switch off, in reverse, what the steps switched on: the GPU (gpu_core_stop), the clocks, the PLL vote, then CX. */
static void
gpu_block_stop(void)
{
	step = 0;
	newest_step = ~0u;
	gpu_core_stop();
	for (uint32_t i = undo_count; i-- > 0;) {
		put(undo[i].b, undo[i].name, undo[i].offset, rd(undo[i].b->base + undo[i].offset) & ~undo[i].bits);
	}
	undo_count = 0;
	if (cx_switched_on) {
		put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, rd(gpucc.base + GPUCC_CX_GDSCR) | GDSC_SW_COLLAPSE);
		for (int i = 0; i < 1000 && cx_is_on(); i++) {
			sleep_us(100);
		}
		get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
		get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
		cx_switched_on = false;
	}
}

static void
adreno_thread(void *arg, wait_result_t result)
{
	uint32_t done = 0;

	(void)arg;
	(void)result;
	printf("adreno: steps up to %u, without mask %#x, starting in %u s; step %u is the new one and waits %u ms %s\n",
	    last_step, skip_steps, start_delay_s, newest_step, pause_ms, fine ? "before every access" : "after each announcement");
	sleep_ms(start_delay_s * 1000);
	for (step = 1; step <= last_step; step++) {
		if (skip_steps & 1u << step) {
			continue;
		}
		if (!steps[step - 1]()) {
			printf("adreno: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("adreno: step %u: done\n", step);
		done = step;
	}
	if (keep && done == STEPS && gpu_keep()) {
		(void)thread_terminate(current_thread());
		thread_block(THREAD_CONTINUE_NULL);
	}
	/* A reset for a write may come late: keep the end of the log saying where things stand. */
	for (uint32_t second = 1; second <= 20; second++) {
		sleep_ms(1000);
		printf("adreno: alive %u s after step %u; %s\n", second, done, last_write);
	}
	if (keep) {
		printf("adreno: over; left as the steps left it (adreno_keep)\n");
	} else {
		gpu_block_stop();
		printf("adreno: over; what the steps switched on is off again\n");
	}
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
hakios_adreno_init(void)
{
	static bool started;
	uint32_t only = 0;
	thread_t thread;

	if (started) {
		return;
	}
	if (!PE_parse_boot_argn("adreno", &last_step, sizeof(last_step))) {
		uint32_t gpu = 0;

		if (!PE_parse_boot_argn("gpu", &gpu, sizeof(gpu)) || gpu == 0) {
			return;
		}
		last_step = 21;
		keep = 1;
		pause_ms = 0;
		zap_fresh = 1;
		skip_steps = 1u << 12;
		start_delay_s = 0;
	}
	if (last_step == 0) {
		return;
	}
	started = true;
	PE_parse_boot_argn("adreno_skip", &skip_steps, sizeof(skip_steps));
	if (PE_parse_boot_argn("adreno_only", &only, sizeof(only)) && only > 0 && only <= STEPS) {
		last_step = only;
		skip_steps = ~(1u << only);
	}
	PE_parse_boot_argn("adreno_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("adreno_fine", &fine, sizeof(fine));
	PE_parse_boot_argn("adreno_delay", &start_delay_s, sizeof(start_delay_s));
	PE_parse_boot_argn("adreno_keep", &keep, sizeof(keep));
	PE_parse_boot_argn("adreno_zap", &zap_wanted, sizeof(zap_wanted));
	PE_parse_boot_argn("adreno_trust", &trust_wanted, sizeof(trust_wanted));
	PE_parse_boot_argn("adreno_zap_fresh", &zap_fresh, sizeof(zap_fresh));
	PE_parse_boot_argn("adreno_zap_meta", &zap_meta, sizeof(zap_meta));
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	for (newest_step = last_step; newest_step > 0 && (skip_steps & 1u << newest_step); newest_step--) {
	}
	if (kernel_thread_start(adreno_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("adreno: kernel_thread_start failed\n");
	}
}

/* ---- what HakiOS's kernel calls and the old tree had no name for ------------------------------------ */

uint32_t gpu_log;                       /* boot-arg gpu_log=1 of hakios.gpu.cpp */
extern void HakiosGPUStop(void);        /* takes the clients' lock, then calls hakios_gpu_stop() */
void hakios_gpu_stop(void);
void hakios_adreno_quiesce(void);
void hakios_adreno_stop(void);
void hakios_adreno_minute(void);
void hakios_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);

/*
 * The end of a run in order: the GPU given back as the steps' own ending does, so the next kernel finds the block as the
 * phone's start leaves it. It first waits for what the core was given: half a second at the most.
 */
void
hakios_gpu_stop(void)
{
	if (gpu_kept) {
		uint32_t waited = 0;

		gpu_kept = false;
		quiet = false;
		while (!hakios_gpu_done(gpu_fence) && waited < 5000) {
			delay(100);
			waited++;
		}
		printf("adreno: the run is about to end: the GPU is given back (%s after %u us)\n",
		    hakios_gpu_done(gpu_fence) ? "it had finished what it was given" : "STILL AT WORK", 100 * waited);
		gpu_block_stop();
	}
}

void
hakios_adreno_stop(void)
{
	if (gpu_kept) {
		HakiosGPUStop();
	}
}

/*
 * No time for asking: a panic, or the way back to the loader. What keeps the GMU and the core from memory that is no longer
 * theirs is the block's own stop, which waits on the GMU; so only said here. NOT DONE yet for this chip: after a panic, the
 * next kernel in the same session of the loader is not expected to work, and a reset is needed (it is here anyway).
 */
void
hakios_adreno_quiesce(void)
{
}

void
hakios_adreno_minute(void)
{
}

/* How long the core has worked: not measured on this chip yet. */
void
hakios_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	*microseconds = 0;
	*submissions = 0;
	bzero(by, 7 * sizeof(by[0]));
}
