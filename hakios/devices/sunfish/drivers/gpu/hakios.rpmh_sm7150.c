/*
 * SWEET PM6150L L11C bring-up; see report.md for DT and source evidence.
 * Sole XNU apps-DRV owner, called from thread context with deep idle disabled.
 * No sleep/wake programming, SPMI writes, GIC handler, or timeout recovery.
 *
 * Also the memory's bandwidth vote (hakios_rpmh_ddr_vote, sunfish, boot-arg ddr): as HackiOS
 * (OOSEX GANG, hk_cpufreq.c ddr_raise(), "metal" branch, APSL 2.0), the most bandwidth there is on
 * the bus clock managers of the memory controller (MC0) and of the shared hub in front of it (SH0),
 * and the memory kept on for the application processor (ACV bit 3), as Linux's interconnect driver
 * votes for a busy GPU (icc-bcm-voter.c: commit, valid, 14-bit average and peak). The BCM takes the
 * highest of its levels the vote reaches. The same ACTIVE TCS sender as the L11C vote.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
int hakios_rpmh_vreg_enable(const char *res, unsigned mv);
int hakios_rpmh_vreg_set(const char *res, unsigned mv);
int hakios_rpmh_vreg_off(const char *res);
int hakios_rpmh_rfclk_enable(const char *res);
int hakios_rpmh_ddr_vote(uint32_t vote, bool send);
int hakios_rpmh_crypto_vote(uint32_t vote);
int hakios_rpmh_arc_votes(const char *const *res, const uint32_t *level, unsigned n);

/* Live downstream DT has two positional reg entries, no reg-names. */
#define RH_RSC_PA       0x18220000u
#define RH_TCS_PA       0x18220d00u
#define RH_TCS_SIZE     0x3000u
#define RH_DICT_PA      0x0c3f000cu
#define RH_DRV          2u
#define RH_ACTIVE_N     2u       /* DT order: ACTIVE 2, SLEEP 3, WAKE 3 */
#define RH_NORMAL_N     8u       /* hidden CONTROL TCS is not in this bank */
#define RH_TCS_STRIDE   0x2a0u
#define RH_CMD_STRIDE   0x14u
#define RH_IRQ_EN       0x00u
#define RH_IRQ_STATUS   0x04u
#define RH_IRQ_CLEAR    0x08u
#define RH_WAIT         0x10u
#define RH_CONTROL      0x14u
#define RH_STATUS       0x18u
#define RH_ENABLE       0x1cu
#define RH_MSGID        0x30u
#define RH_ADDR         0x34u
#define RH_DATA         0x38u
#define RH_CMD_STATUS   0x3cu
#define RH_AMC_EN       (1u << 16)
#define RH_TRIGGER      (1u << 24)
#define RH_ISSUED       (1u << 8)
#define RH_COMPLETE     (1u << 16)
#define RH_WRITE_ACK    (8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC     0x0c0330dbu
#define RH_DB_HEADER    144u
#define RH_DB_ENTRY     24u
#define RH_DB_MAX       0x100000u /* policy bound, not a DT size */
#define RH_ARC          3u
#define RH_VRM          4u
#define RH_BCM          5u
#define RH_TIMEOUT_US   100000u

static vm_offset_t rh_rsc, rh_tcs, rh_dict, rh_db;
static uint32_t rh_db_size;
static unsigned rh_ncpt;
static unsigned rh_lock;
static bool rh_poisoned;

static uint32_t
rh_read(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
rh_write(vm_offset_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
rh_sync(uint32_t off, uint32_t value)
{
	rh_write(rh_tcs, off, value);
	for (unsigned i = 0; i < 1000; ++i) {
		if (rh_read(rh_tcs, off) == value) {
			return true;
		}
		delay(1);
	}
	printf("rpmh: readback timeout off=%#x want=%#x got=%#x\n",
	    off, value, rh_read(rh_tcs, off));
	return false;
}

/* DB is little endian; avoid packed structs and unaligned Device loads. */
static uint16_t
rh_db16(uint32_t off)
{
	uint32_t v = rh_read(rh_db, off & ~3u);
	return (uint16_t)(v >> (8u * (off & 3u)));
}

static bool
rh_range(uint32_t off, uint32_t size)
{
	return off <= rh_db_size && size <= rh_db_size - off;
}

static int
rh_map(void)
{
	if (!rh_rsc) {
		rh_rsc = ml_io_map(RH_RSC_PA, 0x100);
	}
	if (!rh_tcs) {
		rh_tcs = ml_io_map(RH_TCS_PA, RH_TCS_SIZE);
	}
	if (!rh_dict) {
		rh_dict = ml_io_map(RH_DICT_PA, 8);
	}
	if (!rh_rsc || !rh_tcs || !rh_dict) {
		printf("rpmh: RSC/locator mapping failed\n");
		return ENOMEM;
	}
	uint32_t id = rh_read(rh_rsc, 0);
	uint32_t solver = rh_read(rh_rsc, 4);
	uint32_t config = rh_read(rh_rsc, 0x0c);
	unsigned major = (id >> 16) & 0xffu;
	unsigned count = (config >> (6u * RH_DRV)) & 0x3fu;
	rh_ncpt = (config >> 27) & 0x1fu;
	printf("rpmh: rsc=%#x tcs=%#x DRV=%u id=%#x solver=%#x config=%#x TCS=%u ncpt=%u\n",
	    RH_RSC_PA, RH_TCS_PA, RH_DRV, id, solver, config, count, rh_ncpt);
	/* This client implements the pre-v3 layout only; do not guess v3 offsets. */
	if ((major != 1 && major != 2) || config == 0xffffffffu ||
	    solver == 0xffffffffu || count < RH_NORMAL_N ||
	    count > RH_TCS_SIZE / RH_TCS_STRIDE || rh_ncpt < 3 || rh_ncpt > 16) {
		printf("rpmh: unsupported RSC geometry/revision\n");
		return ENODEV;
	}
	if (!rh_db) {
		uint32_t pa = rh_read(rh_dict, 0);
		uint32_t size = rh_read(rh_dict, 4);
		printf("rpmh: cmd-db locator=%#x pa=%#x size=%#x\n", RH_DICT_PA, pa, size);
		/* Firmware supplies the reserved RAM address; never scan physical RAM. */
		if (pa < 0x80000000u || (pa & 3u) || (size & 3u) ||
		    size < RH_DB_HEADER || size > RH_DB_MAX ||
		    (uint64_t)pa + size > 0x100000000ULL) {
			printf("rpmh: invalid cmd-db locator\n");
			return EINVAL;
		}
		rh_db = ml_io_map(pa, size);
		if (!rh_db) {
			return ENOMEM;
		}
		rh_db_size = size;
	}
	uint32_t db_version = rh_read(rh_db, 0);
	uint32_t magic = rh_read(rh_db, 4);
	uint32_t flags = rh_read(rh_db, 140);
	printf("rpmh: cmd-db version=%#x magic=%#x checksum=%#x flags=%#x\n",
	    db_version, magic, rh_read(rh_db, 136), flags);
	/* Linux only checks magic; version 1 is an intentionally stricter policy. */
	if (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u)) {
		printf("rpmh: unsupported/standalone cmd-db\n");
		return EINVAL;
	}
	return 0;
}

static int
rh_lookup(const char *res, unsigned type, uint32_t *address)
{
	uint32_t key[2] = {0, 0};
	for (unsigned i = 0; i < 8 && res[i]; ++i) {
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	}
	unsigned matches = 0;
	for (unsigned i = 0; i < 8; ++i) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave) {
			break;
		}
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		uint32_t aux = RH_DB_HEADER + rh_db16(h + 4);
		unsigned count = rh_db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3u) ||
		    !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(aux, 0)) {
			printf("rpmh: malformed cmd-db slave header %u\n", i);
			return EINVAL;
		}
		for (unsigned j = 0; j < count; ++j) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (rh_read(rh_db, e) != key[0] || rh_read(rh_db, e + 4) != key[1]) {
				continue;
			}
			uint32_t addr = rh_read(rh_db, e + 16);
			if (slave != type || (addr >> 16) != type || (addr & (type == RH_VRM ? 15u : 3u)) ||
			    !rh_range(aux + rh_db16(e + 22), rh_db16(e + 20))) {
				printf("rpmh: invalid %s entry for %s\n", type == RH_VRM ? "VRM" : "BCM", res);
				return EINVAL;
			}
			*address = addr;
			++matches;
			printf("rpmh: %s addr=%#x slave=%u aux-version=%#x priority=%#x:%#x\n",
			    res, addr, slave, rh_db16(h + 8),
			    rh_read(rh_db, e + 8), rh_read(rh_db, e + 12));
		}
	}
	if (matches != 1) {
		printf("rpmh: %s lookup matches=%u; refusing\n", res, matches);
		return ENODEV;
	}
	return 0;
}

static bool
rh_free(unsigned t)
{
	uint32_t b = t * RH_TCS_STRIDE;
	/* Idle alone is insufficient: a sleep/wake payload can be armed and idle. */
	return rh_read(rh_tcs, b + RH_STATUS) != 0 &&
	    rh_read(rh_tcs, b + RH_STATUS) != 0xffffffffu &&
	    rh_read(rh_tcs, b + RH_CONTROL) == 0 &&
	    rh_read(rh_tcs, b + RH_ENABLE) == 0 &&
	    rh_read(rh_tcs, b + RH_WAIT) == 0 &&
	    !(rh_read(rh_tcs, RH_IRQ_STATUS) & (1u << t));
}

/* Up to rh_ncpt writes with response through one free ACTIVE TCS; waits for RPMh to complete them. */
static int
rh_send(const uint32_t *addresses, const uint32_t *values, unsigned n, const char *what)
{
	int chosen = -1;

	if (n == 0 || n > rh_ncpt || n > 3) {
		return EINVAL;
	}
	for (unsigned t = 0; t < RH_ACTIVE_N; ++t) {
		uint32_t b = t * RH_TCS_STRIDE;
		uint32_t enabled = rh_read(rh_tcs, b + RH_ENABLE);
		printf("rpmh: ACTIVE TCS%u idle=%#x control=%#x enable=%#x wait=%#x\n",
		    t, rh_read(rh_tcs, b + RH_STATUS), rh_read(rh_tcs, b + RH_CONTROL),
		    enabled, rh_read(rh_tcs, b + RH_WAIT));
		if (enabled & ~((1u << rh_ncpt) - 1u)) {
			return EIO;
		}
		for (unsigned c = 0; c < rh_ncpt; ++c) {
			for (unsigned k = 0; k < n; ++k) {
				if ((enabled & (1u << c)) &&
				    (rh_read(rh_tcs, b + RH_ADDR + c * RH_CMD_STRIDE) & ~3u) == (addresses[k] & ~3u)) {
					printf("rpmh: conflicting inherited %s request in TCS%u\n", what, t);
					return EBUSY;
				}
			}
		}
		if (chosen < 0 && rh_free(t)) {
			chosen = (int)t;
		}
	}
	if (chosen < 0) {
		printf("rpmh: no empty idle ACTIVE TCS; leaving inherited state intact\n");
		return EBUSY;
	}
	unsigned t = (unsigned)chosen;
	uint32_t b = t * RH_TCS_STRIDE, bit = 1u << t;
	uint32_t irq_en = rh_read(rh_tcs, RH_IRQ_EN);
	if (irq_en == 0xffffffffu || !rh_free(t)) {
		return EBUSY;
	}
	/* Only this TCS's source is masked. Other sources/CPU IRQs remain intact. */
	if (!rh_sync(RH_IRQ_EN, irq_en & ~bit)) {
		rh_poisoned = true;
		return EIO;
	}
	if (!rh_free(t)) {
		/* No payload was touched. Do not acknowledge somebody else's IRQ. */
		if (!rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
			rh_poisoned = true;
		}
		return EBUSY;
	}
	/* From here an uncertain write/timeout quarantines our slot for this boot. */
	rh_poisoned = true;
	uint32_t mask = (1u << n) - 1u;
	printf("rpmh: TCS%u ACTIVE %s: %u commands, irq-enable=%#x\n", t, what, n, irq_en);
	for (unsigned c = 0; c < n; ++c) {
		uint32_t cmd = b + c * RH_CMD_STRIDE;
		printf("rpmh:   addr=%#x data=%#x\n", addresses[c], values[c]);
		if (!rh_sync(cmd + RH_MSGID, RH_WRITE_ACK) ||
		    !rh_sync(cmd + RH_ADDR, addresses[c]) ||
		    !rh_sync(cmd + RH_DATA, values[c])) {
			return EIO;
		}
	}
	if (!rh_sync(b + RH_WAIT, mask) || !rh_sync(b + RH_ENABLE, mask) ||
	    !rh_sync(b + RH_CONTROL, 0) || !rh_sync(b + RH_CONTROL, RH_AMC_EN)) {
		return EIO;
	}
	rh_write(rh_tcs, b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	uint32_t irq = 0, idle = 0, status[3] = {0, 0, 0};
	bool done = false, progress = false;
	for (unsigned us = 0; us < RH_TIMEOUT_US; ++us) {
		irq = rh_read(rh_tcs, RH_IRQ_STATUS);
		idle = rh_read(rh_tcs, b + RH_STATUS);
		bool complete = true;
		for (unsigned c = 0; c < n; ++c) {
			status[c] = rh_read(rh_tcs, b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
			complete &= status[c] != 0xffffffffu &&
			    (status[c] & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE);
		}
		/* A fresh IRQ or observed execution transition excludes stale CMD_STATUS. */
		progress |= idle == 0 || !complete;
		if (idle && idle != 0xffffffffu && irq != 0xffffffffu &&
		    ((irq & bit) || progress) && complete) {
			done = true;
			break;
		}
		delay(1);
	}
	printf("rpmh: TCS%u done=%d irq=%#x idle=%#x control=%#x cmd-status=%#x/%#x/%#x\n",
	    t, done, irq, idle, rh_read(rh_tcs, b + RH_CONTROL), status[0], status[1], status[2]);
	if (!done) {
		printf("rpmh: timeout; TCS quarantined, IRQ remains masked; no reset/retry\n");
		return ETIMEDOUT;
	}
	/* Proven completion: clear trigger before mode, then free only our commands. */
	if (!rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_ENABLE, 0) || !rh_sync(b + RH_WAIT, 0)) {
		return EIO;
	}
	rh_write(rh_tcs, RH_IRQ_CLEAR, bit); /* W1C: never clear other TCS bits */
	if ((rh_read(rh_tcs, RH_IRQ_STATUS) & bit) ||
	    !rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
		return EIO;
	}
	rh_poisoned = false;
	printf("rpmh: %s ACTIVE vote complete\n", what);
	return 0;
}

int
hakios_rpmh_vreg_enable(const char *res, unsigned mv)
{
	/* Board-specific helper: do not apply PMIC5-LDO policy to arbitrary resources. */
	if (!res || strcmp(res, "ldoc11") || mv < 2950 || mv > 3400) {
		printf("rpmh: expected ldoc11 and 2950..3400 mV\n");
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = EIO;
	uint32_t address = 0;
	if (rh_poisoned) {
		printf("rpmh: previous uncertain transaction; refusing further access\n");
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup(res, RH_VRM, &address);
	}
	if (!error) {
		/* HPM (raw 7) is supported by this PMIC5 LDO; enable comes last. */
		const uint32_t addresses[3] = {address, address + 8, address + 4};
		const uint32_t values[3] = {mv, 7, 1};
		error = rh_send(addresses, values, 3, "L11C");
	}
out:
	if (error) {
		printf("rpmh: enable failed errno=%d\n", error);
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/*
 * The same PMIC5 LDO vote for any LDO (the WCN3990's rails: Wi-Fi died after a minute or two without the host
 * owning them): voltage, HPM, enable. Names are cmd-db's ("ldoc1", "ldoc2", "ldoc10", "ldoc11", ...).
 */
int
hakios_rpmh_vreg_set(const char *res, unsigned mv)
{
	int error = EIO;
	uint32_t address = 0;

	if (!res || strncmp(res, "ldo", 3) || mv < 500 || mv > 3400) {
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	if (rh_poisoned) {
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup(res, RH_VRM, &address);
	}
	if (!error) {
		const uint32_t addresses[3] = {address, address + 8, address + 4};
		const uint32_t values[3] = {mv, 7, 1};

		error = rh_send(addresses, values, 3, res);
	}
out:
	printf("rpmh: %s %u mV: %s (errno %d)\n", res, mv, error ? "FAILED" : "on", error);
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/* the opposite: just the enable word at +4 set to 0 (the WCN3990's rails are power cycled before a modem restart) */
int
hakios_rpmh_vreg_off(const char *res)
{
	int error = EIO;
	uint32_t address = 0;

	if (!res || strncmp(res, "ldo", 3)) {
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	if (rh_poisoned) {
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup(res, RH_VRM, &address);
	}
	if (!error) {
		const uint32_t addresses[1] = {address + 4};
		const uint32_t values[1] = {0};

		error = rh_send(addresses, values, 1, res);
	}
out:
	printf("rpmh: %s off: %s (errno %d)\n", res, error ? "FAILED" : "done", error);
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/* a clk-rpmh VRM clock (the WCN3990's RF reference clock "rfclka2"): value 1 at the resource's address + 4 */
int
hakios_rpmh_rfclk_enable(const char *res)
{
	int error = EIO;
	uint32_t address = 0;

	if (!res || strncmp(res, "rfclk", 5)) {
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	if (rh_poisoned) {
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup(res, RH_VRM, &address);
	}
	if (!error) {
		const uint32_t addresses[1] = {address + 4};
		const uint32_t values[1] = {1};

		error = rh_send(addresses, values, 1, res);
	}
out:
	printf("rpmh: clock %s: %s (errno %d)\n", res, error ? "FAILED" : "on", error);
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/*
 * Rail (ARC) votes, as Linux's rpmhpd does for a remote processor's power domains (qcom_q6v5_pas
 * adsp_pds_enable): the resource's cmd-db address gets the level (the "vlvl" of the resource: 64
 * low SVS, 128 SVS, 192 SVS L1, 256 nominal, ...). Up to three resources in one ACTIVE request.
 */
int
hakios_rpmh_arc_votes(const char *const *res, const uint32_t *level, unsigned n)
{
	uint32_t addresses[3];
	int error;

	if (n == 0 || n > 3) {
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	error = EIO;
	if (rh_poisoned) {
		printf("rpmh: previous uncertain transaction; refusing further access\n");
		goto out;
	}
	error = rh_map();
	for (unsigned i = 0; !error && i < n; i++) {
		error = rh_lookup(res[i], RH_ARC, &addresses[i]);
	}
	if (!error) {
		error = rh_send(addresses, level, n, "rail levels");
	}
out:
	printf("rpmh: rail votes (%s %u, ...): %s (errno %d)\n", res[0], level[0], error ? "FAILED" : "ok", error);
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

#define BCM_COMMIT      (1u << 30)
#define BCM_VALID       (1u << 29)
#define BCM_VOTE(x, y)  (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fffu) << 14 | ((uint32_t)(y) & 0x3fffu))
#define BCM_ACV_APPS    8u

/*
 * The memory bandwidth vote: `vote` (1..16383, in the BCMs' own units; 16383 the most) as average and
 * peak on MC0 and SH0, and ACV's bit for the application processor. send=false only looks the BCMs up
 * in cmd-db and says where they are.
 */
int
hakios_rpmh_ddr_vote(uint32_t vote, bool send)
{
	uint32_t mc0 = 0, sh0 = 0, acv = 0;
	int error;

	if (vote == 0 || vote > 0x3fffu) {
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	error = EIO;
	if (rh_poisoned) {
		printf("rpmh: previous uncertain transaction; refusing further access\n");
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup("MC0", RH_BCM, &mc0);
	}
	if (!error) {
		error = rh_lookup("SH0", RH_BCM, &sh0);
	}
	if (!error) {
		error = rh_lookup("ACV", RH_BCM, &acv);
	}
	if (!error && send) {
		const uint32_t addresses[3] = {mc0, sh0, acv};
		const uint32_t values[3] = {BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(0, BCM_ACV_APPS)};
		error = rh_send(addresses, values, 3, "DDR bandwidth");
	}
out:
	printf("rpmh: DDR bandwidth %s %u on MC0 %#x, SH0 %#x, ACV %#x: %s (errno %d)\n",
	    send ? "vote" : "look-up for a vote of", vote, mc0, sh0, acv, error ? "FAILED" : "ok", error);
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}
/*
 * The crypto engine's path to memory, which TrustZone uses for the PAS calls (zap shader): linux
 * qcom_scm.c votes it around every one (qcom_scm_bw_enable: icc_set_bw(path, 0, UINT_MAX) on the
 * sm7150.dtsi scm "interconnects", MASTER_CRYPTO_CORE_0 -> SLAVE_EBI_CH0). In sm7150.c that path's
 * BCMs are CE0 (qxm_crypto), SN11 (the aggre2 NoC to the system NoC), SN0 (to the GEM NoC), then
 * SH0, MC0 and ACV, which hakios_rpmh_ddr_vote() votes. Without it, our PAS_MEM_SETUP came back
 * -3145798 while the same call works for Linux.
 */
int
hakios_rpmh_crypto_vote(uint32_t vote)
{
	uint32_t ce0 = 0, sn11 = 0, sn0 = 0;
	int error;

	if (vote == 0 || vote > 0x3fffu) {
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	error = EIO;
	if (rh_poisoned) {
		printf("rpmh: previous uncertain transaction; refusing further access\n");
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup("CE0", RH_BCM, &ce0);
	}
	if (!error) {
		error = rh_lookup("SN11", RH_BCM, &sn11);
	}
	if (!error) {
		error = rh_lookup("SN0", RH_BCM, &sn0);
	}
	if (!error) {
		const uint32_t addresses[3] = {ce0, sn11, sn0};
		const uint32_t values[3] = {BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(vote, vote)};
		error = rh_send(addresses, values, 3, "crypto bandwidth");
	}
out:
	printf("rpmh: crypto bandwidth vote %u on CE0 %#x, SN11 %#x, SN0 %#x: %s (errno %d)\n", vote, ce0, sn11, sn0,
	    error ? "FAILED" : "ok", error);
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}
