/*
 * For HakiOS: bsd/dev/sweet_mpss.c of ios-sunfish, where this chain ran on this phone started by ABL, up to pings over Wi-Fi.
 * Boot-arg mpss=1. Status: RUN ON THE PHONE through U-Boot, iOS 16.5, 2026-11-05 (logs/054549-run.log, mpss=1 wlanprobe=8):
 * modem up, WLFW, a scan of 34 access points, a WPA2 join asked for in Settings (4-way handshake, keys), DHCP, en0 up with an
 * address, traffic both ways. What made it go was taking the 16 MB host pool early (hakios_mpss_sm7150_go).
 * 2026-11-05 later: en0 now made by drivers/net/hakios.net.c (this file is its link: transmit, input, link state), the IOKit
 * objects by HakiosWLAN (hakios.wifi_sm7150.cpp), DHCP and the route by configd (wlan_kdhcp=1 brings back the in-kernel DHCP).
 * Run logs/060524-run.log: link up, 22k frames / 30 MB received over en0 by the end. Not seen by me: what the user saw in
 * Settings and Safari (asked), a join after a reboot (the root is in RAM: nothing is kept), other wlanprobe levels.
 */
/*
 * The modem (MPSS, a Hexagon DSP) of the SM7150, started through TrustZone. The Pixel 4a's Wi-Fi
 * (WCN3990) has its firmware run on the modem, so nothing of Wi-Fi works until this does.
 *
 * Linux does it with remoteproc qcom_q6v5_pas ("qcom,sm7150-mpss-pas": no MBA, unlike older
 * parts): drivers/remoteproc/qcom_q6v5_pas.c adsp_start -> qcom_mdt_pas_load -> PAS init_image
 * (the .mdt header), mem_setup (the reserved region), the segments copied into it, then
 * auth_and_reset. Same three TrustZone calls as the GPU's zap shader (sweet_adreno.c, image 13);
 * the modem is image 4. This file does only that; the power domains (CX, MX, MSS), the
 * crypto/AOSS votes and SMP2P/SMEM come next, as the log shows what TrustZone wants.
 *
 * Nothing happens unless boot-arg mpss=1. The firmware is read from the iOS root, /fw/modem/
 * (modem.mdt, modem.b00 ... modem.b30: the phone's modem partition, FAT, image/ directory).
 * Nothing is ever written to the modem's file systems (modemst1/2, fsg, persist).
 */
#include <sys/param.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/kauth.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>

extern kern_return_t kernel_thread_start_priority(thread_continue_t, void *, integer_t, thread_t *);
#define SWEET_WLAN_PRI 92       /* above the iOS start-up load: the host side has to answer the firmware's rings on time */
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>  /* xnu-12377: the KMA flags and kmem_alloc moved to the internal header */
#include "board.h"            /* hakios_board_has(): only on a board that lists wifi-wcn3990 */
#include "log.h"              /* hakios_say(): to the loader's log as well, read while the phone runs */
#include <sys/sysctl.h>
#include <sys/random.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <net/ether_if_module.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
#include <net/if_media.h>
#include <net/dlil.h>
#include <sys/kern_event.h>
#include <sys/board/net.h>
#include "sm7150_wlan_util.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern int hakios_rpmh_vreg_set(const char *res, unsigned mv);
extern int hakios_rpmh_rfclk_enable(const char *res);
extern int hakios_rpmh_vreg_off(const char *res);
extern int hakios_rpmh_crypto_vote(uint32_t vote);
extern int hakios_rpmh_arc_votes(const char *const *res, const uint32_t *level, unsigned n);
extern int hakios_rpmh_ddr_vote(uint32_t vote, bool send);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t);
void hakios_mpss_sm7150_start(void);
static void ew_if_create(void);

/* sm7150-google-sunfish.dts: mpss_mem */
#define MPSS_MEMORY             0x8b000000ull
#define MPSS_MEMORY_SIZE        0x9800000u
#define PAS_MODEM               4
#define WIMG_WCOMB              0x6u    /* VM_WIMG_WCOMB: not cacheable | coherent (osfmk/arm/memory_types.h) */
#define FW_DIRECTORY            "/hakios/fw/modem/"
#define CHUNK                   0x100000

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1
/* qcom_scm_assign_mem(): QCOM_SCM_SVC_MP (0xc), QCOM_SCM_MP_ASSIGN (0x16), 7 arguments: RO, VAL, RO, VAL, RO, VAL, VAL */
#define SCM_MP_ASSIGN           SCM_CALL(0xc, 0x16)
#define SCM_ARGS_ASSIGN         (7u | 1u << 4 | 1u << 8 | 1u << 12)
#define VMID_HLOS               3u
#define VMID_MSS_MSA            0xfu
#define SCM_PERM_RW             6u

#define ELF_PT_LOAD             1
#define MDT_TYPE_HASH           (2u << 24)

/* What happened, repeated in the "alive" lines: the live log starts after this thread has begun. */
static char report[2048];
static size_t report_length;

static void
note(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	if (report_length < sizeof(report)) {
		report_length += (size_t)vsnprintf(report + report_length, sizeof(report) - report_length, format, arguments);
	}
	va_end(arguments);
}

/* Events with the second of the modem's life they happened at (read with sysctl kern.sweet_mpss_events). */
static char g_events[16384];
static volatile bool g_modem_crashed;      /* the modem reported a crash: do not touch its memory any more */
static volatile bool g_wlan_crashed;       /* the WLAN protection domain crashed: the modem as a whole is restarted (it escalates the crash) */
static volatile uint32_t g_wlan_thread_running;   /* set by wlan_start_thread (compare-and-swap), so a second start cannot run a second service thread */
static volatile bool g_wlan_halt;          /* a restart is under way: the service thread must not touch the chip's registers (its rails go off) */
static char g_last_crash[128];
static bool g_crash_seen;
static size_t g_events_length;
static uint32_t g_events_second;

static uint32_t g_ev_dump;       /* boot-arg mpss_dump=N: the event ring every N seconds */
static uint32_t g_ev_log;        /* boot-arg mpss_log=1: every event in the kernel's log as well */
static uint32_t g_wifi_ps = 1;   /* boot-arg wifi_ps=0: the station's power save stays off (as before 2026-11-20) */

static void
ev(const char *format, ...)
{
	if (g_ev_log) {
		va_list copy;
		char line[160];

		va_start(copy, format);
		vsnprintf(line, sizeof(line), format, copy);
		va_end(copy);
		hakios_say("mpss-ev: %s\n", line);
	}
	va_list arguments;
	size_t room = sizeof(g_events) - g_events_length;

	if (room < 256) {
		/* Discard the oldest half of events to keep the log rolling */
		size_t half = sizeof(g_events) / 2;
		char *nl = (char *)memchr(g_events + half, '\n', sizeof(g_events) - half);
		if (nl != NULL) {
			size_t keep = g_events_length - (size_t)(nl + 1 - g_events);
			memmove(g_events, nl + 1, keep);
			g_events_length = keep;
			g_events[g_events_length] = 0;
			room = sizeof(g_events) - g_events_length;
		} else {
			g_events_length = 0;
			g_events[0] = 0;
			room = sizeof(g_events);
		}
	}
	g_events_length += (size_t)snprintf(g_events + g_events_length, room, "%us ", g_events_second);
	va_start(arguments, format);
	g_events_length += (size_t)vsnprintf(g_events + g_events_length, sizeof(g_events) - g_events_length, format, arguments);
	va_end(arguments);
	if (g_events_length < sizeof(g_events) - 1) {
		g_events[g_events_length++] = '\n';
		g_events[g_events_length] = 0;
	}
}

struct elf32_phdr {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

static struct elf32_phdr mpss_phdrs[64];

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, 1000 * 1000);
}

/* The fifth SMC argument (x5) of the next scm() call: a call with more than 4 arguments passes the rest in a buffer. */
static uint64_t scm_fifth;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer)
{
	uint64_t session = 0, fifth = scm_fifth;

	scm_fifth = 0;

	hakios_say("mpss: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", what, function, first,
	    second, third);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = fifth;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			hakios_say("mpss: TrustZone returns %d, answer %#llx\n", (int32_t)x0, x1);
			note("[%#llx -> %d,%#llx]", function & 0xff, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	hakios_say("mpss: TrustZone keeps saying the call was interrupted\n");
	return -1;
}

/* Bytes [offset, offset + length) of a firmware file into buffer; the file's size if size != NULL. */
static int
fw_read(const char *name, off_t offset, void *buffer, int length, off_t *size)
{
	vfs_context_t context = vfs_context_kernel();
	char path[64];
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	int resid = 0, error;

	snprintf(path, sizeof(path), FW_DIRECTORY "%s", name);
	error = vnode_open(path, FREAD, 0, 0, &vnode, context);
	if (error != 0) {
		return error;
	}
	if (size != NULL) {
		VATTR_INIT(&attributes);
		VATTR_WANTED(&attributes, va_data_size);
		error = vnode_getattr(vnode, &attributes, context);
		*size = (off_t)attributes.va_data_size;
	}
	if (error == 0 && length > 0) {
		error = vn_rdwr(UIO_READ, vnode, (caddr_t)buffer, length, offset, UIO_SYSSPACE, 0, vfs_context_ucred(context),
		    &resid, vfs_context_proc(context));
		if (error == 0 && resid != 0) {
			error = EIO;
		}
	}
	vnode_close(vnode, FREAD, context);
	return error;
}

/* Bytes of the modem's memory: a window of at most CHUNK is mapped at a time (the whole region will not map). */
static void
mpss_write(uint64_t offset, const uint8_t *data, uint64_t length)
{
	while (length != 0) {
		uint64_t phys = MPSS_MEMORY + offset;
		uint64_t base = phys & ~(uint64_t)PAGE_MASK;
		uint64_t n = MIN(length, (uint64_t)CHUNK);
		volatile uint8_t *window;

		n = MIN(n, base + CHUNK - phys);
		window = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)base, CHUNK, WIMG_WCOMB);
		if (window == NULL) {
			hakios_say("mpss: cannot map %#llx\n", base);
			return;
		}
		if (data != NULL) {
			for (uint64_t b = 0; b < n; b++) {
				window[phys - base + b] = data[b];
			}
		} else {
			uint64_t rem = n;
			uint64_t off = phys - base;
			while ((off & 7) && rem > 0) {
				window[off++] = 0;
				rem--;
			}
			volatile uint64_t *w64 = (volatile uint64_t *)(window + off);
			uint64_t words = rem / 8;
			for (uint64_t w = 0; w < words; w++) {
				w64[w] = 0;
			}
			for (uint64_t b = 0; b < (rem & 7); b++) {
				window[off + words * 8 + b] = 0;
			}
		}
		ml_io_unmap((vm_offset_t)window, CHUNK);
		offset += n;
		length -= n;
		if (data != NULL) {
			data += n;
		}
	}
}

/* The first 8 bytes of the modem's memory, to see that the copy landed. */
static uint64_t
mpss_first_word(void)
{
	volatile uint8_t *window = (volatile uint8_t *)ml_io_map_unmappable(MPSS_MEMORY, CHUNK, WIMG_WCOMB);
	uint64_t word = 0;

	if (window != NULL) {
		for (int b = 7; b >= 0; b--) {
			word = word << 8 | window[b];
		}
		ml_io_unmap((vm_offset_t)window, CHUNK);
	}
	return word;
}

static bool
loadable(const struct elf32_phdr *segment)
{
	return segment->type == ELF_PT_LOAD && (segment->flags & (7u << 24)) != MDT_TYPE_HASH && segment->memsz != 0;
}

/*
 * The memory the modem's remote file system service (RMTFS, below) shares with the modem: TrustZone's
 * memory protection lets only its owners reach it, and the modem faults on it (Hexagon fs:0x19e, a bus
 * error) unless it is shared with the modem's VMID. As Linux does (rmtfs_mem.c: qcom,vmid =
 * MSS_MSA; qcom_scm.c qcom_scm_assign_mem()): owned by HLOS only -> HLOS and MSS_MSA, read/write.
 */
#define RMTFS_ASSIGN_PHYS       0xfde01000ull
#define RMTFS_ASSIGN_SIZE       0x600000ull

static bool
mpss_assign_rmtfs(void)
{
	vm_offset_t page = 0;
	uint8_t *p;
	uint64_t answer = ~0ull, phys;
	int32_t result;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	p = (uint8_t *)page;
	phys = kvtophys(page);
	/* +0: the source VM list; +64: the memory (address, size); +128: the new owners; +256: arguments 4 to 7 */
	*(uint32_t *)(p + 0) = VMID_HLOS;
	*(uint64_t *)(p + 64) = RMTFS_ASSIGN_PHYS;
	*(uint64_t *)(p + 72) = RMTFS_ASSIGN_SIZE;
	/* struct qcom_scm_current_perm_info: vmid, perm, ctx (u64), ctx_size, unused */
	*(uint32_t *)(p + 128) = VMID_HLOS;
	*(uint32_t *)(p + 132) = SCM_PERM_RW;
	*(uint32_t *)(p + 152) = VMID_MSS_MSA;
	*(uint32_t *)(p + 156) = SCM_PERM_RW;
	*(uint64_t *)(p + 256) = 4;             /* source list size */
	*(uint64_t *)(p + 264) = phys + 128;    /* new owners */
	*(uint64_t *)(p + 272) = 48;            /* their size: 2 x 24 */
	*(uint64_t *)(p + 280) = 0;
	for (int o = 0; o < 512; o += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (p + o) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	scm_fifth = phys + 256;
	result = scm("to share the rmtfs memory with the modem", SCM_64 | SCM_SIP | SCM_MP_ASSIGN, SCM_ARGS_ASSIGN,
	    phys + 64, 16, phys, &answer);        /* the (address, size) record and its size; the source list */
	note("[assign_mem %d,%#llx]", result, answer);
	return result == 0;
}

static bool
mpss_start(void)
{
	struct elf32_phdr segment;
	vm_offset_t page = 0, chunk = 0;
	uint8_t *mdt;
	uint64_t answer = ~0ull, low = ~0ull, high = 0;
	off_t mdt_size = 0;
	uint32_t segments, loaded = 0;

	if (kmem_alloc(kernel_map, &page, 4 * PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &chunk, CHUNK, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		hakios_say("mpss: out of memory\n");
		return false;
	}
	mdt = (uint8_t *)page;
	/* The root is mounted a while after this thread starts: wait for the header. */
	for (int tries = 0;; tries++) {
		int error = fw_read("modem.mdt", 0, NULL, 0, &mdt_size);

		if (error == 0) {
			break;
		}
		if (tries % 20 == 0) {
			hakios_say("mpss: waiting for " FW_DIRECTORY "modem.mdt (error %d, try %d)\n", error, tries);
		}
		if (tries == 300) {
			hakios_say("mpss: no " FW_DIRECTORY "modem.mdt (error %d): copy the phone's modem partition image/ there\n", error);
			return false;
		}
		sleep_ms(1000);
	}
	if (mdt_size < 52 + 32 * 2 || mdt_size > 3 * PAGE_SIZE || fw_read("modem.mdt", 0, mdt, (int)mdt_size, NULL) != 0) {
		hakios_say("mpss: modem.mdt unreadable or odd (%lld bytes)\n", (long long)mdt_size);
		return false;
	}
	segments = mdt[44] | mdt[45] << 8;
	if (memcmp(mdt, "\177ELF\1", 5) != 0 || segments < 2 || segments > 64 || mdt_size < 52 + 32 * segments) {
		hakios_say("mpss: modem.mdt is not an ELF32 header with segments (%u)\n", segments);
		return false;
	}
	for (uint32_t i = 0; i < segments; i++) {
		memcpy(&mpss_phdrs[i], mdt + 52 + 32 * i, sizeof(struct elf32_phdr));
		if (loadable(&mpss_phdrs[i])) {
			low = MIN(low, mpss_phdrs[i].paddr);
			high = MAX(high, (uint64_t)mpss_phdrs[i].paddr + mpss_phdrs[i].memsz);
		}
	}
	note("[%u segs %#llx..%#llx]", segments, low, high);
	hakios_say("mpss: %u segments, loadable span %#llx..%#llx (reserved %#llx at %#llx)\n", segments, low, high,
	    (uint64_t)MPSS_MEMORY_SIZE, MPSS_MEMORY);
	if (high <= low || high - low > MPSS_MEMORY_SIZE) {
		hakios_say("mpss: does not fit the reserved memory\n");
		return false;
	}

	/* The modem's power domains (CX and MSS), as qcom_q6v5_pas votes them (max) and msm-4.14 sdmmagpie.dtsi pil_modem (TURBO). */
	{
		static const char *const rails[2] = { "cx.lvl", "mss.lvl" };
		static const uint32_t levels[2] = { 384, 384 };

		int rail_error = hakios_rpmh_arc_votes(rails, levels, 2);

		hakios_say("mpss: rail votes %d\n", rail_error);
		note("[rails %d]", rail_error);
	}
	/* Linux's qcom_scm_bw_enable() around every PAS call: the crypto engine's path to memory (see sweet_rpmh.c). */
	{
		int crypto_error = hakios_rpmh_crypto_vote(0x3fff), memory_error = hakios_rpmh_ddr_vote(0x3fff, true);

		hakios_say("mpss: crypto vote %d, memory vote %d\n", crypto_error, memory_error);
		note("[crypto %d ddr %d]", crypto_error, memory_error);
	}

	/*
	 * (a fast call first, answered at once by its nature: whether TrustZone has the shutdown call at all. Under HakiOS common
	 * the slow call below never came back on this phone, where under the 16.5 kernel it answered at once; which core asks
	 * is said, and boot-arg mpss_no_shutdown=1 leaves the slow one out)
	 */
	{
		uint64_t mpidr, daif;
		uint32_t no_shutdown = 0;

		__asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
		__asm__ volatile ("mrs %0, daif" : "=r" (daif));
		hakios_say("mpss: TrustZone calls from core %#llx, DAIF %#llx\n", mpidr & 0xffffff, daif);
		(void)scm("whether it has the shutdown call (fast)", (1ull << 31) | SCM_64 | SCM_SIP | SCM_CALL(6, 1), SCM_ONE_VALUE,
		    SCM_SIP | SCM_PAS_SHUTDOWN, 0, 0, &answer);
		PE_parse_boot_argn("mpss_no_shutdown", &no_shutdown, sizeof(no_shutdown));
		if (no_shutdown) {
			hakios_say("mpss: mpss_no_shutdown=1: the shutdown call left out\n");
			goto skip_shutdown;
		}
	}
	/*
	 * (boot-arg mpss_shutdown_after=S: not before S seconds of uptime. Under the 16.5 kernel this call came 33 s in -- the
	 * firmware appeared only once init had mounted it -- and answered at once; here, seconds in, it never came back)
	 */
	{
		uint32_t after = 0;
		uint64_t now_ns;

		PE_parse_boot_argn("mpss_shutdown_after", &after, sizeof(after));
		absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
		if (after != 0 && now_ns / NSEC_PER_SEC < after) {
			hakios_say("mpss: the shutdown call waits until %u s of uptime (now %llu s)\n", after, now_ns / NSEC_PER_SEC);
			delay_for_interval((uint32_t)(after - now_ns / NSEC_PER_SEC), NSEC_PER_SEC);
		}
	}
	/* A modem image left by an earlier boot is given up first. */
	(void)scm("to give up a modem image it may hold", SCM_64 | SCM_SIP | SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_MODEM, 0, 0,
	    &answer);
skip_shutdown:
	memcpy((void *)(page + 2 * PAGE_SIZE), mdt, (size_t)mdt_size);
	if (scm("to take the modem's header", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_MODEM,
	    kvtophys(page + 2 * PAGE_SIZE), 0, &answer) != 0 || answer != 0) {
		hakios_say("mpss: TrustZone does not take the modem's header (%#llx)\n", answer);
		return false;
	}
	if (scm("where the modem image will be", SCM_64 | SCM_SIP | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_MODEM, MPSS_MEMORY,
	    high - low, &answer) != 0 || answer != 0) {
		hakios_say("mpss: TrustZone does not take the place (%#llx)\n", answer);
		return false;
	}
	note("[first word before %#llx]", mpss_first_word());
	for (uint32_t i = 0; i < segments; i++) {
		char name[24];
		off_t size = 0, done = 0;

		segment = mpss_phdrs[i];
		if (!loadable(&segment)) {
			continue;
		}
		if (segment.filesz == 0) {
			mpss_write(segment.paddr - low, NULL, segment.memsz);
			continue;
		}
		snprintf(name, sizeof(name), "modem.b%02u", i);
		if (fw_read(name, 0, NULL, 0, &size) != 0 || size < segment.filesz || segment.filesz > segment.memsz) {
			hakios_say("mpss: %s missing or short (%lld, need %u)\n", name, (long long)size, segment.filesz);
			return false;
		}
		loaded++;
		hakios_say("mpss: %u bytes of %s at %#llx\n", segment.filesz, name, MPSS_MEMORY + segment.paddr - low);
		while (done < segment.filesz) {
			int n = (int)MIN((off_t)CHUNK, (off_t)segment.filesz - done);
			int error = fw_read(name, done, (void *)chunk, n, NULL);

			if (error != 0) {
				hakios_say("mpss: cannot read %s at %lld (error %d)\n", name, (long long)done, error);
				return false;
			}
			mpss_write(segment.paddr - low + done, (uint8_t *)chunk, (uint64_t)n);
			done += n;
		}
		if (segment.memsz > segment.filesz) {
			mpss_write(segment.paddr - low + segment.filesz, NULL, segment.memsz - segment.filesz);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	note("[%u segments copied, first word after %#llx]", loaded, mpss_first_word());
	hakios_say("mpss: rmtfs memory shared with the modem: %s\n", mpss_assign_rmtfs() ? "yes" : "NO");
	if (scm("to authenticate the modem and start it", SCM_64 | SCM_SIP | SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_MODEM,
	    0, 0, &answer) != 0 || answer != 0) {
		hakios_say("mpss: TrustZone does not authenticate/start the modem (%#llx)\n", answer);
		return false;
	}
	hakios_say("mpss: TRUSTZONE HAS AUTHENTICATED AND STARTED THE MODEM\n");
	return true;
}


#define SMEM_MEMORY             0x86000000ull
#define SMEM_SIZE               0x200000u

struct smem_proc_comm {
	uint32_t command, status, params[2];
};

struct smem_global_entry {
	uint32_t allocated, offset, size, aux_base;
};

struct smem_header {
	struct smem_proc_comm proc_comm[4];
	uint32_t version[32];
	uint32_t initialized;
	uint32_t free_offset;
	uint32_t available;
	uint32_t reserved;
	struct smem_global_entry toc[512];
};

struct smem_ptable_entry {
	uint32_t offset;
	uint32_t size;
	uint32_t flags;
	uint16_t host0;
	uint16_t host1;
	uint32_t cacheline;
	uint32_t reserved[7];
};

struct smem_ptable {
	uint8_t magic[4]; /* "$TOC" */
	uint32_t version;
	uint32_t num_entries;
	uint32_t reserved[5];
	struct smem_ptable_entry entry[];
};

struct smem_partition_header {
	uint8_t magic[4]; /* "$PRT" */
	uint16_t host0;
	uint16_t host1;
	uint32_t size;
	uint32_t offset_free_uncached;
	uint32_t offset_free_cached;
	uint32_t reserved[3];
};

struct smem_private_entry {
	uint16_t canary; /* 0xa5a5 */
	uint16_t item;
	uint32_t size;
	uint16_t padding_data;
	uint16_t padding_hdr;
	uint32_t reserved;
};

struct smp2p_smem_item {
	uint32_t magic; /* 0x504d5324 "$SMP" */
	uint8_t version;
	uint32_t features : 24;
	uint16_t local_pid;
	uint16_t remote_pid;
	uint16_t total_entries;
	uint16_t valid_entries;
	uint32_t flags;
	struct {
		uint8_t name[16];
		uint32_t value;
	} entries[16];
} __attribute__((packed));


static void
sweet_apcs_kick(uint32_t bit)
{
	volatile uint32_t *apcs = (volatile uint32_t *)ml_io_map_unmappable(0x17c00000, PAGE_SIZE, WIMG_WCOMB);
	if (apcs) {
		apcs[3] = (1u << bit);
		__asm__ volatile ("dsb sy" ::: "memory");
		ml_io_unmap((vm_offset_t)apcs, PAGE_SIZE);
	}
}

static void *
sweet_smem_get_entry(struct smem_partition_header *phdr, uint32_t part_size, uint16_t item, uint32_t *out_size)
{
	uint32_t cur = sizeof(*phdr);
	while (cur + sizeof(struct smem_private_entry) <= phdr->offset_free_uncached &&
	       cur + sizeof(struct smem_private_entry) <= part_size) {
		struct smem_private_entry *e = (struct smem_private_entry *)((uint8_t *)phdr + cur);
		if (e->canary != 0xa5a5) break;
		if (e->item == item) {
			if (out_size) *out_size = e->size - e->padding_data;
			return (void *)((uint8_t *)phdr + cur + sizeof(*e) + e->padding_hdr);
		}
		cur += sizeof(*e) + e->padding_hdr + e->size;
	}
	return NULL;
}

static void *
sweet_smem_alloc_entry(struct smem_partition_header *phdr, uint32_t part_size, uint16_t item, uint32_t size)
{
	uint32_t existing_sz = 0;
	void *existing = sweet_smem_get_entry(phdr, part_size, item, &existing_sz);
	if (existing) return existing;

	uint32_t alloc_off = phdr->offset_free_uncached;
	size_t aligned_sz = (size + 7) & ~7;
	size_t total_alloc = sizeof(struct smem_private_entry) + aligned_sz;

	if (alloc_off + total_alloc > phdr->offset_free_cached ||
	    alloc_off + total_alloc > part_size) {
		hakios_say("SWEET SMEM: out of space in partition for item %u (sz %u)\n", item, size);
		return NULL;
	}

	struct smem_private_entry *e = (struct smem_private_entry *)((uint8_t *)phdr + alloc_off);
	e->canary = 0xa5a5;
	e->item = item;
	e->size = (uint32_t)aligned_sz;
	e->padding_data = (uint16_t)(aligned_sz - size);
	e->padding_hdr = 0;
	e->reserved = 0;

	void *ptr = (void *)(e + 1);
	memset(ptr, 0, aligned_sz);

	__asm__ volatile ("dsb sy" ::: "memory");
	phdr->offset_free_uncached += (uint32_t)total_alloc;
	__asm__ volatile ("dsb sy" ::: "memory");

	hakios_say("SWEET SMEM: allocated item %u (size %u) at off %#x\n", item, size, alloc_off);
	note("[alloc item %u @ %#x]", item, alloc_off);
	return ptr;
}

static void
sweet_smp2p_setup_outbound(struct smem_partition_header *phdr, uint32_t part_size)
{
	struct smp2p_smem_item *out = (struct smp2p_smem_item *)sweet_smem_alloc_entry(phdr, part_size, 428, sizeof(struct smp2p_smem_item));
	if (out) {
		out->magic = 0x504d5324; /* "$SMP" */
		out->local_pid = 0;      /* APPS */
		out->remote_pid = 1;     /* MODEM */
		out->total_entries = 16;
		out->valid_entries = 2;
		out->features = 1;       /* SMP2P_FEATURE_SSR_ACK */
		memcpy(out->entries[0].name, "master-kernel", 14);
		out->entries[0].value = 0;
		memcpy(out->entries[1].name, "ipa", 4);
		out->entries[1].value = 0;

		__asm__ volatile ("dsb sy" ::: "memory");
		out->version = 1;
		__asm__ volatile ("dsb sy" ::: "memory");
	}

	/* Kick modem via APCS bit 14 */
	sweet_apcs_kick(14);
}

static void
sweet_smp2p_poll_inbound(struct smem_partition_header *phdr, uint32_t part_size, int tick)
{
	uint32_t ssz = 0;
	struct smp2p_smem_item *smp = (struct smp2p_smem_item *)sweet_smem_get_entry(phdr, part_size, 435, &ssz);
	if (!smp) return;

	/*
	 * SSR ack (smp2p.c qcom_smp2p_check_ssr / do_ssr_ack): when the modem restarts it toggles RESTART_DONE (flags bit 0) in its
	 * item; the AP answers by making RESTART_ACK (flags bit 1) of its own item equal to it. Without that a restarted modem's SMP2P
	 * is not usable (its WLAN domain asserted in platform_main_task at every restart).
	 */
	{
		uint32_t osz = 0;
		struct smp2p_smem_item *out = (struct smp2p_smem_item *)sweet_smem_get_entry(phdr, part_size, 428, &osz);

		if (out && (smp->features & 1)) {
			bool done = (smp->flags & 1) != 0, ack = (out->flags & 2) != 0;

			if (done != ack) {
				out->flags = (out->flags & ~2u) | (done ? 2u : 0u);
				__asm__ volatile ("dsb sy" ::: "memory");
				sweet_apcs_kick(14);
				ev("SMP2P: restart ack %u (modem flags %#x)", done, smp->flags);
			}
		}
	}

	static uint16_t last_smp_valid = 0xffff;
	if (tick == 1 || smp->valid_entries != last_smp_valid || tick % 200 == 0) {
		hakios_say("SWEET SMP2P 435: mag %#x, ver %u, %u->%u, valid %u/%u, flags %#x\n",
		    smp->magic, smp->version, smp->local_pid, smp->remote_pid,
		    smp->valid_entries, smp->total_entries, smp->flags);
		if (tick == 1 || smp->valid_entries != last_smp_valid) {
			note("[smp435: %u->%u v%u valid %u/%u]",
			    smp->local_pid, smp->remote_pid, smp->version, smp->valid_entries, smp->total_entries);
		}
		last_smp_valid = smp->valid_entries;
	}
	static uint32_t last_vals[16];
	for (uint32_t v = 0; v < smp->valid_entries && v < 16; v++) {
		char ename[17] = {0};
		memcpy(ename, smp->entries[v].name, 16);
		if (tick == 1 || smp->entries[v].value != last_vals[v] || tick % 600 == 0) {
			if (ename[0] || smp->entries[v].value) {
				hakios_say("SWEET SMP2P: entry %u '%s' = %#x (valid %u)\n",
				    v, ename, smp->entries[v].value, v < smp->valid_entries);
				last_vals[v] = smp->entries[v].value;
				note("[SMP2P '%s'=%#x]", ename, smp->entries[v].value);
			}
		}
	}
}

static void
sweet_smem_check_crash(struct smem_partition_header *phdr, uint32_t part_size)
{
	uint32_t csz = 0;
	char *cmsg = (char *)sweet_smem_get_entry(phdr, part_size, 421, &csz);
	if (cmsg && cmsg[0]) {
		if (strncmp(g_last_crash, cmsg, sizeof(g_last_crash) - 1) != 0) {
			strncpy(g_last_crash, cmsg, sizeof(g_last_crash) - 1);
			static unsigned ncrash;

			hakios_say("SWEET MPSS CRASH: %s\n", cmsg);
			if (ncrash++ < 3) {
				ev("CRASH %.150s", cmsg);
			}
			if (g_crash_seen && strncmp(cmsg, "SFR Init", 8) != 0) {   /* a first message is left over from the previous boot; "SFR Init" is the modem's own start-up text */
				if (strnstr(cmsg, "wlan_process", 160) != NULL) {
					g_wlan_crashed = true;          /* the modem escalates a user-domain crash: it needs a restart */
				} else {
					g_modem_crashed = true;
				}
			}
			g_crash_seen = true;
			note("[CRASH: %.50s]", cmsg);
		}
	}
}

/* =========================================================================
 * GLINK Native Transport over SMEM & QRTR
 * ========================================================================= */
#define SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR   478
#define SMEM_GLINK_NATIVE_XPRT_FIFO_0       479
#define SMEM_GLINK_NATIVE_XPRT_FIFO_1       480

#define GLINK_CMD_VERSION           0
#define GLINK_CMD_VERSION_ACK       1
#define GLINK_CMD_OPEN              2
#define GLINK_CMD_CLOSE             3
#define GLINK_CMD_OPEN_ACK          4
#define GLINK_CMD_INTENT            5
#define GLINK_CMD_RX_DONE           6
#define GLINK_CMD_RX_INTENT_REQ     7
#define GLINK_CMD_RX_INTENT_REQ_ACK 8
#define GLINK_CMD_TX_DATA           9
#define GLINK_CMD_CLOSE_ACK         11
#define GLINK_CMD_TX_DATA_CONT      12
#define GLINK_CMD_READ_NOTIF        13
#define GLINK_CMD_RX_DONE_W_REUSE   14
#define GLINK_CMD_SIGNALS           15

#define GLINK_VERSION_1             1
#define GLINK_FEATURE_INTENT_REUSE  (1u << 0)

#define QRTR_TYPE_DATA              1
#define QRTR_TYPE_HELLO             2
#define QRTR_TYPE_BYE               3
#define QRTR_TYPE_NEW_SERVER        4
#define QRTR_TYPE_DEL_SERVER        5
#define QRTR_TYPE_DEL_CLIENT        6
#define QRTR_TYPE_RESUME_TX         7
#define QRTR_TYPE_EXIT              8
#define QRTR_TYPE_PING              9
#define QRTR_TYPE_NEW_LOOKUP        10
#define QRTR_TYPE_DEL_LOOKUP        11

#define QRTR_PORT_CTRL              0xfffffffeu

struct glink_msg {
	uint16_t cmd;
	uint16_t param1;
	uint32_t param2;
} __attribute__((packed));

struct glink_msg_intent {
	uint16_t cmd;
	uint16_t lcid;
	uint32_t count;
	uint32_t size;
	uint32_t liid;
} __attribute__((packed));

struct glink_msg_rx_done {
	uint16_t cmd;
	uint16_t lcid;
	uint32_t liid;
} __attribute__((packed));

struct glink_msg_tx_data {
	uint16_t cmd;
	uint16_t lcid;
	uint32_t riid;
	uint32_t chunk_size;
	uint32_t left_size;
} __attribute__((packed));

struct qrtr_hdr_v1 {
	uint32_t version;
	uint32_t type;
	uint32_t src_node_id;
	uint32_t src_port_id;
	uint32_t confirm_rx;
	uint32_t size;
	uint32_t dst_node_id;
	uint32_t dst_port_id;
} __attribute__((packed));

struct qrtr_ctrl_pkt {
	uint32_t cmd;
	union {
		struct {
			uint32_t service;
			uint32_t instance;
			uint32_t node;
			uint32_t port;
		} server;
		struct {
			uint32_t node;
			uint32_t port;
		} client;
	};
} __attribute__((packed));

struct sweet_glink_remote_intent {
	uint32_t size;
	uint32_t iid;
	bool in_use;
};

struct sweet_glink_channel {
	char name[32];
	uint16_t lcid;
	uint16_t rcid;
	bool open;
	bool remote_open;
	uint32_t rx_count;
	uint32_t tx_count;
	struct sweet_glink_remote_intent intents[64];
	uint32_t num_intents;
};

struct sweet_glink_state {
	bool ready;
	bool version_sent;
	bool version_ack_received;
	bool version_ack_sent;
	uint32_t features;
	volatile uint32_t *descs;   /* item 478: [0]=tx_tail, [1]=tx_head, [2]=rx_tail, [3]=rx_head */
	volatile uint8_t *tx_fifo;  /* item 479: 16384 bytes */
	volatile uint8_t *rx_fifo;  /* item 480: 16384 bytes */
	uint32_t tx_size;
	uint32_t rx_size;
	struct sweet_glink_channel channels[8];
	uint32_t num_channels;
};

static struct sweet_glink_state g_glink;

static void
sweet_glink_rx_peek(struct sweet_glink_state *gl, uint32_t tail, void *dst, size_t count)
{
	size_t len = MIN(count, (size_t)(gl->rx_size - tail));
	memcpy(dst, (const void *)(gl->rx_fifo + tail), len);
	if (len < count) {
		memcpy((uint8_t *)dst + len, (const void *)gl->rx_fifo, count - len);
	}
}

static void
sweet_glink_rx_advance(struct sweet_glink_state *gl, size_t count)
{
	uint32_t tail = gl->descs[2];
	tail = (tail + (uint32_t)count) % gl->rx_size;
	__asm__ volatile ("dsb sy" ::: "memory");
	gl->descs[2] = tail;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
sweet_glink_tx_cmd(struct sweet_glink_state *gl, const void *data, size_t len)
{
	uint32_t head = gl->descs[1];
	uint32_t tail = gl->descs[0];
	uint32_t avail;
	if (tail <= head)
		avail = gl->tx_size - head + tail;
	else
		avail = tail - head;

	size_t aligned_len = (len + 7) & ~7;
	if (avail < aligned_len + 16) {
		hakios_say("SWEET GLINK TX: FIFO full (avail %u, need %zu)\n", avail, aligned_len);
		return false;
	}

	size_t first = MIN(len, (size_t)(gl->tx_size - head));
	memcpy((void *)(gl->tx_fifo + head), data, first);
	if (first < len) {
		memcpy((void *)gl->tx_fifo, (const uint8_t *)data + first, len - first);
	}

	/* Zero padding */
	if (aligned_len > len) {
		uint32_t pad_off = (head + (uint32_t)len) % gl->tx_size;
		size_t pad_len = aligned_len - len;
		size_t pfirst = MIN(pad_len, (size_t)(gl->tx_size - pad_off));
		memset((void *)(gl->tx_fifo + pad_off), 0, pfirst);
		if (pfirst < pad_len) {
			memset((void *)gl->tx_fifo, 0, pad_len - pfirst);
		}
	}

	head = (head + (uint32_t)aligned_len) % gl->tx_size;
	__asm__ volatile ("dsb sy" ::: "memory");
	gl->descs[1] = head;
	__asm__ volatile ("dsb sy" ::: "memory");

	/* Kick mailbox 12 */
	sweet_apcs_kick(12);
	return true;
}

static struct sweet_glink_channel *
sweet_glink_get_or_add_channel(struct sweet_glink_state *gl, const char *name)
{
	for (uint32_t i = 0; i < gl->num_channels; i++) {
		if (strncmp(gl->channels[i].name, name, sizeof(gl->channels[i].name)) == 0) {
			return &gl->channels[i];
		}
	}
	if (gl->num_channels < 8) {
		struct sweet_glink_channel *ch = &gl->channels[gl->num_channels++];
		strncpy(ch->name, name, sizeof(ch->name) - 1);
		ch->lcid = (uint16_t)gl->num_channels; /* 1-based local id */
		return ch;
	}
	return NULL;
}

static struct sweet_glink_channel *
sweet_glink_find_by_rcid(struct sweet_glink_state *gl, uint16_t rcid)
{
	for (uint32_t i = 0; i < gl->num_channels; i++) {
		if (gl->channels[i].rcid == rcid) return &gl->channels[i];
	}
	return NULL;
}

static struct sweet_glink_channel *
sweet_glink_find_by_lcid(struct sweet_glink_state *gl, uint16_t lcid)
{
	for (uint32_t i = 0; i < gl->num_channels; i++) {
		if (gl->channels[i].lcid == lcid) return &gl->channels[i];
	}
	return NULL;
}

static bool
sweet_glink_send_data(struct sweet_glink_state *gl, struct sweet_glink_channel *ch, const void *data, size_t len)
{
	struct sweet_glink_remote_intent *intent = NULL;
	for (uint32_t i = 0; i < ch->num_intents; i++) {
		if (!ch->intents[i].in_use && ch->intents[i].size >= len) {
			intent = &ch->intents[i];
			break;
		}
	}
	if (!intent) {
		hakios_say("SWEET GLINK: no intent available for len %zu on channel '%s' (have %u intents)\n",
		    len, ch->name, ch->num_intents);
		return false;
	}

	intent->in_use = true;

	struct {
		struct glink_msg_tx_data hdr;
		uint8_t payload[256];
	} __attribute__((packed)) pkt;

	pkt.hdr.cmd = GLINK_CMD_TX_DATA;
	pkt.hdr.lcid = ch->rcid; /* destination rcid */
	pkt.hdr.riid = intent->iid;
	pkt.hdr.chunk_size = (uint32_t)len;
	pkt.hdr.left_size = 0;

	size_t copy_sz = MIN(len, sizeof(pkt.payload));
	memcpy(pkt.payload, data, copy_sz);

	size_t total_sz = sizeof(pkt.hdr) + copy_sz;
	bool ok = sweet_glink_tx_cmd(gl, &pkt, total_sz);
	if (ok) {
		ch->tx_count++;
	}
	return ok;
}


#define RMTFS_DIRECTORY         "/hakios/fw/rmtfs/"
#define RMTFS_PORT              14
#define RMTFS_QMI_OPEN          1
#define RMTFS_QMI_CLOSE         2
#define RMTFS_QMI_RW_IOVEC      3
#define RMTFS_QMI_ALLOC_BUFF    4
#define RMTFS_QMI_GET_DEV_ERROR 5

#define RMTFS_BUFF_PHYS         0xfde01000ull
#define RMTFS_BUFF_SIZE         0x600000ull

struct qmi_hdr {
	uint8_t type;
	uint16_t txn_id;
	uint16_t msg_id;
	uint16_t msg_len;
} __attribute__((packed));

struct rmtfs_iovec_entry {
	uint32_t sector_addr;
	uint32_t phys_offset;
	uint32_t num_sector;
} __attribute__((packed));

struct rmtfs_caller {
	char path[64];
	char file[32];
	bool active;
};

static struct rmtfs_caller g_rmtfs_callers[16];
static volatile uint8_t *g_rmtfs_map = NULL;
static volatile uint8_t *g_rmtfs_buf = NULL;

static bool
str_contains(const char *haystack, const char *needle)
{
	if (!haystack || !needle) return false;
	size_t nlen = strlen(needle);
	size_t hlen = strlen(haystack);
	if (nlen > hlen) return false;
	for (size_t i = 0; i <= hlen - nlen; i++) {
		if (strncmp(haystack + i, needle, nlen) == 0) return true;
	}
	return false;
}

static void
sweet_qrtr_announce_server(struct sweet_glink_channel *ch, uint32_t dst_node, uint32_t service, uint32_t instance, uint32_t port)
{
	struct {
		struct qrtr_hdr_v1 hdr;
		struct qrtr_ctrl_pkt ctrl;
	} __attribute__((packed)) srv;
	memset(&srv, 0, sizeof(srv));
	srv.hdr.version = 1;
	srv.hdr.type = QRTR_TYPE_NEW_SERVER;
	srv.hdr.src_node_id = 1; /* APPS */
	srv.hdr.src_port_id = port;
	srv.hdr.size = sizeof(struct qrtr_ctrl_pkt);
	srv.hdr.dst_node_id = dst_node;
	srv.hdr.dst_port_id = QRTR_PORT_CTRL;

	srv.ctrl.cmd = QRTR_TYPE_NEW_SERVER;
	srv.ctrl.server.service = service;
	srv.ctrl.server.instance = instance;
	srv.ctrl.server.node = 1;
	srv.ctrl.server.port = port;

	sweet_glink_send_data(&g_glink, ch, &srv, sizeof(srv));
	hakios_say("SWEET QRTR: announced service %u (inst %#x) on port %u\n", service, instance, port);
	note("[QRTR pub s%u p%u]", service, port);
}

static int
rmtfs_read_partition_file(const char *name, off_t offset, void *buffer, int length)
{
	vfs_context_t context = vfs_context_kernel();
	char path[64];
	vnode_t vnode = NULLVP;
	int resid = 0, error;

	snprintf(path, sizeof(path), RMTFS_DIRECTORY "%s", name);
	error = vnode_open(path, FREAD, 0, 0, &vnode, context);
	if (error != 0) {
		hakios_say("SWEET RMTFS: cannot open '%s' (error %d)\n", path, error);
		return error;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)buffer, length, offset, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &resid, vfs_context_proc(context));
	vnode_close(vnode, FREAD, context);
	return (error == 0 && resid == 0) ? 0 : (error ? error : EIO);
}

static void
sweet_rmtfs_handle_qmi(struct sweet_glink_channel *ch, const struct qrtr_hdr_v1 *qhdr,
    const uint8_t *payload, uint32_t len)
{
	if (len < sizeof(struct qmi_hdr)) return;
	const struct qmi_hdr *req_qmi = (const struct qmi_hdr *)payload;
	uint8_t qtype = req_qmi->type;
	uint16_t txn_id = req_qmi->txn_id;
	uint16_t msg_id = req_qmi->msg_id;
	uint16_t msg_len = req_qmi->msg_len;

	hakios_say("SWEET RMTFS: QMI req type %u, txn %u, msg_id %u, len %u\n", qtype, txn_id, msg_id, msg_len);
	note("[RMTFS req m%u t%u]", msg_id, txn_id);

	switch (msg_id) {
	case RMTFS_QMI_OPEN: {
		/* Parse TLV 1 (path) */
		char req_path[128] = {0};
		uint32_t off = sizeof(struct qmi_hdr);
		while (off + 3 <= len) {
			uint8_t t_type = payload[off];
			uint16_t t_len = *(uint16_t *)(payload + off + 1);
			if (off + 3 + t_len > len) break;
			if (t_type == 1) {
				memcpy(req_path, payload + off + 3, MIN(t_len, sizeof(req_path) - 1));
			}
			off += 3 + t_len;
		}

		uint32_t caller_id = 0;
		const char *img_file = NULL;
		if (str_contains(req_path, "fs1") || str_contains(req_path, "modemst1")) {
			caller_id = 1;
			img_file = "modemst1.img";
		} else if (str_contains(req_path, "fs2") || str_contains(req_path, "modemst2")) {
			caller_id = 2;
			img_file = "modemst2.img";
		} else if (str_contains(req_path, "fsg")) {
			caller_id = 3;
			img_file = "fsg.img";
		} else if (str_contains(req_path, "fsc")) {
			caller_id = 4;
			img_file = "fsc.img";
		} else {
			static uint32_t next_cid = 5;
			caller_id = next_cid++;
			img_file = "unknown";
		}

		if (caller_id < 16 && img_file) {
			strncpy(g_rmtfs_callers[caller_id].path, req_path, sizeof(g_rmtfs_callers[caller_id].path) - 1);
			strncpy(g_rmtfs_callers[caller_id].file, img_file, sizeof(g_rmtfs_callers[caller_id].file) - 1);
			g_rmtfs_callers[caller_id].active = true;
		}

		hakios_say("SWEET RMTFS: OPEN path='%s' -> cid %u (%s)\n", req_path, caller_id, img_file);
		note("[RMTFS OPEN '%s'->%u]", req_path, caller_id);

		/* Send OPEN response */
		struct {
			struct qrtr_hdr_v1 qhdr;
			struct qmi_hdr qmi;
			/* TLV 2: Result (7 bytes) */
			uint8_t res_type;
			uint16_t res_len;
			uint16_t result;
			uint16_t error;
			/* TLV 16 (0x10): Caller ID (7 bytes) */
			uint8_t cid_type;
			uint16_t cid_len;
			uint32_t caller_id;
		} __attribute__((packed)) resp;
		memset(&resp, 0, sizeof(resp));
		resp.qhdr.version = 1;
		resp.qhdr.type = QRTR_TYPE_DATA;
		resp.qhdr.src_node_id = 1;
		resp.qhdr.src_port_id = RMTFS_PORT;
		resp.qhdr.dst_node_id = qhdr->src_node_id;
		resp.qhdr.dst_port_id = qhdr->src_port_id;
		resp.qhdr.size = sizeof(resp.qmi) + 7 + 7;

		resp.qmi.type = 2; /* RESPONSE */
		resp.qmi.txn_id = txn_id;
		resp.qmi.msg_id = RMTFS_QMI_OPEN;
		resp.qmi.msg_len = 14;

		resp.res_type = 2;
		resp.res_len = 4;
		resp.result = 0; /* SUCCESS */
		resp.error = 0;

		resp.cid_type = 0x10;
		resp.cid_len = 4;
		resp.caller_id = caller_id;

		sweet_glink_send_data(&g_glink, ch, &resp, sizeof(resp));
		note("[RMTFS ORESP c%u]", caller_id);
		break;
	}
	case RMTFS_QMI_ALLOC_BUFF: {
		uint32_t caller_id = 0;
		uint32_t buff_size = 0;
		uint32_t off = sizeof(struct qmi_hdr);
		while (off + 3 <= len) {
			uint8_t t_type = payload[off];
			uint16_t t_len = *(uint16_t *)(payload + off + 1);
			if (off + 3 + t_len > len) break;
			if (t_type == 1 && t_len >= 4) {
				caller_id = *(uint32_t *)(payload + off + 3);
			} else if (t_type == 2 && t_len >= 4) {
				buff_size = *(uint32_t *)(payload + off + 3);
			}
			off += 3 + t_len;
		}

		hakios_say("SWEET RMTFS: ALLOC_BUFF caller=%u size=%u (mapped buf at %p)\n",
		    caller_id, buff_size, g_rmtfs_buf);
		note("[RMTFS ALLOC c%u sz %u]", caller_id, buff_size);

		struct {
			struct qrtr_hdr_v1 qhdr;
			struct qmi_hdr qmi;
			/* TLV 2: Result (7 bytes) */
			uint8_t res_type;
			uint16_t res_len;
			uint16_t result;
			uint16_t error;
			/* TLV 16 (0x10): Buffer Address (11 bytes) */
			uint8_t addr_type;
			uint16_t addr_len;
			uint64_t buff_address;
		} __attribute__((packed)) resp;
		memset(&resp, 0, sizeof(resp));
		resp.qhdr.version = 1;
		resp.qhdr.type = QRTR_TYPE_DATA;
		resp.qhdr.src_node_id = 1;
		resp.qhdr.src_port_id = RMTFS_PORT;
		resp.qhdr.dst_node_id = qhdr->src_node_id;
		resp.qhdr.dst_port_id = qhdr->src_port_id;
		resp.qhdr.size = sizeof(resp.qmi) + 7 + 11;

		resp.qmi.type = 2; /* RESPONSE */
		resp.qmi.txn_id = txn_id;
		resp.qmi.msg_id = RMTFS_QMI_ALLOC_BUFF;
		resp.qmi.msg_len = 18;

		resp.res_type = 2;
		resp.res_len = 4;
		resp.result = 0; /* SUCCESS */
		resp.error = 0;

		resp.addr_type = 0x10;
		resp.addr_len = 8;
		resp.buff_address = RMTFS_BUFF_PHYS;

		sweet_glink_send_data(&g_glink, ch, &resp, sizeof(resp));
		note("[RMTFS ARESP %#llx]", RMTFS_BUFF_PHYS);
		break;
	}
	case RMTFS_QMI_RW_IOVEC: {
		uint32_t caller_id = 0;
		uint8_t direction = 0;
		uint8_t num_entries = 0;
		const struct rmtfs_iovec_entry *entries = NULL;
		uint32_t off = sizeof(struct qmi_hdr);
		while (off + 3 <= len) {
			uint8_t t_type = payload[off];
			uint16_t t_len = *(uint16_t *)(payload + off + 1);
			if (off + 3 + t_len > len) break;
			if (t_type == 1 && t_len >= 4) {
				caller_id = *(uint32_t *)(payload + off + 3);
			} else if (t_type == 2 && t_len >= 1) {
				direction = payload[off + 3];
			} else if (t_type == 3 && t_len >= 1) {
				num_entries = payload[off + 3];
				entries = (const struct rmtfs_iovec_entry *)(payload + off + 4);
			}
			off += 3 + t_len;
		}

		const char *fname = (caller_id < 16 && g_rmtfs_callers[caller_id].active) ?
		    g_rmtfs_callers[caller_id].file : NULL;

		hakios_say("SWEET RMTFS: RW_IOVEC caller=%u dir=%u num_entries=%u file=%s\n",
		    caller_id, direction, num_entries, fname ? fname : "none");
		note("[RMTFS IOVEC c%u d%u n%u]", caller_id, direction, num_entries);

		if (direction == 0 && entries && fname && g_rmtfs_buf) {
			/* READ from disk into buffer */
			for (uint8_t i = 0; i < num_entries; i++) {
				uint32_t sec_addr = entries[i].sector_addr;
				uint32_t phys_addr = entries[i].phys_offset;
				uint32_t nsec = entries[i].num_sector;
				uint32_t nbytes = nsec * 512;
				off_t foff = (off_t)sec_addr * 512;

				/* Calculate offset relative to RMTFS_BUFF_PHYS */
				uint32_t buf_off = (phys_addr >= (uint32_t)RMTFS_BUFF_PHYS) ?
				    (phys_addr - (uint32_t)RMTFS_BUFF_PHYS) : phys_addr;

				hakios_say("SWEET RMTFS: READ sec %u (foff %#llx) -> phys %#x (boff %#x), sz %u\n",
				    sec_addr, foff, phys_addr, buf_off, nbytes);
				note("[RMTFS RD s%u b%#x sz%u]", sec_addr, buf_off, nbytes);

				/* Read in 4K chunks into shared buffer */
				uint8_t temp[4096];
				uint32_t left = nbytes;
				uint32_t cur_boff = buf_off;
				off_t cur_foff = foff;
				while (left > 0 && cur_boff < RMTFS_BUFF_SIZE) {
					uint32_t chunk = MIN(left, (uint32_t)sizeof(temp));
					chunk = MIN(chunk, (uint32_t)(RMTFS_BUFF_SIZE - cur_boff));
					int r = rmtfs_read_partition_file(fname, cur_foff, temp, chunk);
					if (r != 0) {
						hakios_say("SWEET RMTFS: ERROR reading %s at foff %#llx (err %d)\n", fname, cur_foff, r);
						note("[RMTFS ERR %s r%d]", fname, r);
						memset(temp, 0, chunk);
					} else if (cur_foff == 0) {
						uint32_t w0 = *(uint32_t *)temp;
						hakios_say("SWEET RMTFS: %s first word: %#x\n", fname, w0);
						note("[RMTFS %s w0 %#x]", fname, w0);
					}
					for (uint32_t b = 0; b < chunk; b++) {
						g_rmtfs_buf[cur_boff + b] = temp[b];
					}
					left -= chunk;
					cur_foff += chunk;
					cur_boff += chunk;
				}
			}
			__asm__ volatile ("dsb sy" ::: "memory");
		} else if (direction == 1) {
			hakios_say("SWEET RMTFS: WRITE ignored (read-only safe mode)\n");
			note("[RMTFS WR ign]");
		}

		/* Send RW_IOVEC response */
		struct {
			struct qrtr_hdr_v1 qhdr;
			struct qmi_hdr qmi;
			/* TLV 2: Result (7 bytes) */
			uint8_t res_type;
			uint16_t res_len;
			uint16_t result;
			uint16_t error;
		} __attribute__((packed)) resp;
		memset(&resp, 0, sizeof(resp));
		resp.qhdr.version = 1;
		resp.qhdr.type = QRTR_TYPE_DATA;
		resp.qhdr.src_node_id = 1;
		resp.qhdr.src_port_id = RMTFS_PORT;
		resp.qhdr.dst_node_id = qhdr->src_node_id;
		resp.qhdr.dst_port_id = qhdr->src_port_id;
		resp.qhdr.size = sizeof(resp.qmi) + 7;

		resp.qmi.type = 2; /* RESPONSE */
		resp.qmi.txn_id = txn_id;
		resp.qmi.msg_id = RMTFS_QMI_RW_IOVEC;
		resp.qmi.msg_len = 7;

		resp.res_type = 2;
		resp.res_len = 4;
		resp.result = 0; /* SUCCESS */
		resp.error = 0;

		sweet_glink_send_data(&g_glink, ch, &resp, sizeof(resp));
		note("[RMTFS IRESP ok]");
		break;
	}
	case RMTFS_QMI_CLOSE: {
		uint32_t caller_id = 0;
		uint32_t off = sizeof(struct qmi_hdr);
		while (off + 3 <= len) {
			uint8_t t_type = payload[off];
			uint16_t t_len = *(uint16_t *)(payload + off + 1);
			if (off + 3 + t_len > len) break;
			if (t_type == 1 && t_len >= 4) {
				caller_id = *(uint32_t *)(payload + off + 3);
			}
			off += 3 + t_len;
		}
		if (caller_id < 16) g_rmtfs_callers[caller_id].active = false;
		hakios_say("SWEET RMTFS: CLOSE caller=%u\n", caller_id);
		note("[RMTFS CLOSE c%u]", caller_id);

		struct {
			struct qrtr_hdr_v1 qhdr;
			struct qmi_hdr qmi;
			uint8_t res_type;
			uint16_t res_len;
			uint16_t result;
			uint16_t error;
		} __attribute__((packed)) resp;
		memset(&resp, 0, sizeof(resp));
		resp.qhdr.version = 1;
		resp.qhdr.type = QRTR_TYPE_DATA;
		resp.qhdr.src_node_id = 1;
		resp.qhdr.src_port_id = RMTFS_PORT;
		resp.qhdr.dst_node_id = qhdr->src_node_id;
		resp.qhdr.dst_port_id = qhdr->src_port_id;
		resp.qhdr.size = sizeof(resp.qmi) + 7;

		resp.qmi.type = 2;
		resp.qmi.txn_id = txn_id;
		resp.qmi.msg_id = RMTFS_QMI_CLOSE;
		resp.qmi.msg_len = 7;

		resp.res_type = 2;
		resp.res_len = 4;
		resp.result = 0;
		resp.error = 0;

		sweet_glink_send_data(&g_glink, ch, &resp, sizeof(resp));
		note("[RMTFS CRESP ok]");
		break;
	}
	case RMTFS_QMI_GET_DEV_ERROR: {
		hakios_say("SWEET RMTFS: GET_DEV_ERROR\n");
		struct {
			struct qrtr_hdr_v1 qhdr;
			struct qmi_hdr qmi;
			uint8_t res_type;
			uint16_t res_len;
			uint16_t result;
			uint16_t error;
			uint8_t st_type;
			uint16_t st_len;
			uint8_t status;
		} __attribute__((packed)) resp;
		memset(&resp, 0, sizeof(resp));
		resp.qhdr.version = 1;
		resp.qhdr.type = QRTR_TYPE_DATA;
		resp.qhdr.src_node_id = 1;
		resp.qhdr.src_port_id = RMTFS_PORT;
		resp.qhdr.dst_node_id = qhdr->src_node_id;
		resp.qhdr.dst_port_id = qhdr->src_port_id;
		resp.qhdr.size = sizeof(resp.qmi) + 7 + 4;

		resp.qmi.type = 2;
		resp.qmi.txn_id = txn_id;
		resp.qmi.msg_id = RMTFS_QMI_GET_DEV_ERROR;
		resp.qmi.msg_len = 11;

		resp.res_type = 2;
		resp.res_len = 4;
		resp.result = 0;
		resp.error = 0;

		resp.st_type = 0x10;
		resp.st_len = 1;
		resp.status = 0;

		sweet_glink_send_data(&g_glink, ch, &resp, sizeof(resp));
		note("[RMTFS ERESP ok]");
		break;
	}
	default:
		hakios_say("SWEET RMTFS: unhandled QMI msg_id %u\n", msg_id);
		note("[RMTFS unh msg %u]", msg_id);
		break;
	}
}

static void
sweet_glink_setup(struct smem_partition_header *phdr, uint32_t part_size)
{
	if (g_glink.ready) return;

	uint32_t desc_sz = 0;
	void *dptr = sweet_smem_get_entry(phdr, part_size, SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR, &desc_sz);
	if (!dptr || desc_sz < 16) {
		return;
	}

	void *tx_ptr = sweet_smem_alloc_entry(phdr, part_size, SMEM_GLINK_NATIVE_XPRT_FIFO_0, 16384);
	if (!tx_ptr) {
		hakios_say("SWEET GLINK: cannot allocate item 479 (TX FIFO)\n");
		return;
	}

	uint32_t rx_sz = 0;
	void *rx_ptr = sweet_smem_get_entry(phdr, part_size, SMEM_GLINK_NATIVE_XPRT_FIFO_1, &rx_sz);
	if (!rx_ptr || rx_sz < 16384) {
		return;
	}

	g_glink.descs = (volatile uint32_t *)dptr;
	g_glink.tx_fifo = (volatile uint8_t *)tx_ptr;
	g_glink.rx_fifo = (volatile uint8_t *)rx_ptr;
	g_glink.tx_size = 16384;
	g_glink.rx_size = 16384;
	g_glink.ready = true;

	hakios_say("SWEET GLINK: mapped descs=%p [tx_tail=%u tx_head=%u rx_tail=%u rx_head=%u]\n",
	    g_glink.descs, g_glink.descs[0], g_glink.descs[1], g_glink.descs[2], g_glink.descs[3]);
	note("[GLINK init d0=%u d1=%u d2=%u d3=%u]",
	    g_glink.descs[0], g_glink.descs[1], g_glink.descs[2], g_glink.descs[3]);

	/* Initialize TX head and RX tail to 0 */
	g_glink.descs[1] = 0; /* tx_head */
	g_glink.descs[2] = 0; /* rx_tail */
	__asm__ volatile ("dsb sy" ::: "memory");

	/* Send GLINK_CMD_VERSION */
	struct glink_msg vmsg = {
		.cmd = GLINK_CMD_VERSION,
		.param1 = GLINK_VERSION_1,
		.param2 = GLINK_FEATURE_INTENT_REUSE,
	};
	sweet_glink_tx_cmd(&g_glink, &vmsg, sizeof(vmsg));
	g_glink.version_sent = true;
	hakios_say("SWEET GLINK: sent GLINK_CMD_VERSION (v1, feat %#x)\n", GLINK_FEATURE_INTENT_REUSE);
	note("[GLINK sent v1]");
}

/* ---- TFTP over QRTR (what tqftpserv does): the modem fetches its firmware here and checks that a server exists ---- */

/*
 * QRTR service 4096 (instance 1): a request (RRQ/WRQ) arrives at port 4096 from some client port; the
 * transfer then continues from a port of ours of its own (4097 ...), as tqftpserv gives each client a socket,
 * with the same options (blksize, timeoutms, tsize, wsize, rsize, seek). Reads are served from /fw/tftp/ (the
 * /readonly/... names the modem uses are mapped there; see tqftpserv translate.c). Writes (/readwrite/...,
 * the modem checks for a server with /readwrite/server_check.txt and keeps mcfg.tmp) are kept in memory only, so
 * the modem can read them back: nothing is ever written to a disk.
 */
#define TFTP_PORT               4096
#define TFTP_XFERS              4
#define TFTP_DIR                "/hakios/fw/tftp/"
#define TFTP_BUF                8192
#define TFTP_RRQ                1
#define TFTP_WRQ                2
#define TFTP_DATA               3
#define TFTP_ACK                4
#define TFTP_ERROR              5
#define TFTP_OACK               6
#define TFTP_IDLE_PUMPS         12000   /* of 5 ms: a minute */

/* The files the modem writes under /readwrite/ and reads back: kept in memory only, never on a disk. */
#define TFTP_RAM_FILES          8
#define TFTP_RAM_CAP            (256 * 1024)
struct tftp_ramfile {
	char path[64];
	uint8_t *data;
	uint32_t size;
	bool used;
};
static struct tftp_ramfile g_tftp_ram[TFTP_RAM_FILES];

struct tftp_xfer {
	struct tftp_ramfile *ram;
	bool used, writer;
	uint32_t cnode, cport;
	char path[96];
	uint64_t fsize, seek, rsize, data_len, total_blocks;
	uint32_t blksize, wsize, timeoutms;
	uint32_t next, limit, expected, idle;
	bool stalled;
	uint32_t last_request, requests, sent, acks, last_ack;
};

static struct tftp_xfer g_tftp[TFTP_XFERS];
static uint8_t g_tftp_buf[TFTP_BUF];
static struct sweet_glink_channel *g_tftp_ch;

/* Receive buffers asked of the remote (qcom_glink_request_intent) and not yet offered. */
static uint32_t g_big_pending, g_pump_clock, g_last_req_clock;

/* Why the last sweet_glink_send_vec() failed: 1 no intent big enough (ask the remote for one), 2 the FIFO is full. */
static int g_send_failure;

/* One GLINK data message made of two pieces (a QRTR header and its payload), any size an intent and the FIFO take. */
static bool
sweet_glink_send_vec(struct sweet_glink_state *gl, struct sweet_glink_channel *ch, const void *a, size_t alen,
    const void *b, size_t blen)
{
	struct sweet_glink_remote_intent *intent = NULL;
	struct glink_msg_tx_data hdr;
	size_t len = alen + blen, total = sizeof(hdr) + len, aligned = (total + 7) & ~(size_t)7;
	uint32_t head = gl->descs[1], tail = gl->descs[0], avail;
	const void *piece[3] = { &hdr, a, b };
	size_t piece_len[3] = { sizeof(hdr), alen, blen };

	for (uint32_t i = 0; i < ch->num_intents; i++) {
		if (!ch->intents[i].in_use && ch->intents[i].size >= len) {
			intent = &ch->intents[i];
			break;
		}
	}
	if (!intent) {
		g_send_failure = 1;
		return false;
	}
	avail = tail <= head ? gl->tx_size - head + tail : tail - head;
	if (avail < aligned + 16) {
		g_send_failure = 2;
		return false;
	}
	intent->in_use = true;
	hdr.cmd = GLINK_CMD_TX_DATA;
	hdr.lcid = ch->rcid;
	hdr.riid = intent->iid;
	hdr.chunk_size = (uint32_t)len;
	hdr.left_size = 0;
	for (int k = 0; k < 3; k++) {
		size_t first = MIN(piece_len[k], (size_t)(gl->tx_size - head));

		memcpy((void *)(gl->tx_fifo + head), piece[k], first);
		if (first < piece_len[k]) {
			memcpy((void *)gl->tx_fifo, (const uint8_t *)piece[k] + first, piece_len[k] - first);
		}
		head = (head + (uint32_t)piece_len[k]) % gl->tx_size;
	}
	if (aligned > total) {
		size_t pad = aligned - total, first = MIN(pad, (size_t)(gl->tx_size - head));

		memset((void *)(gl->tx_fifo + head), 0, first);
		if (first < pad) {
			memset((void *)gl->tx_fifo, 0, pad - first);
		}
		head = (head + (uint32_t)pad) % gl->tx_size;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	gl->descs[1] = head;
	__asm__ volatile ("dsb sy" ::: "memory");
	ch->tx_count++;
	sweet_apcs_kick(12);
	return true;
}

static bool
tftp_send_to(uint32_t cnode, uint32_t cport, uint32_t lport, const uint8_t *p, size_t len)
{
	struct qrtr_hdr_v1 h;

	if (g_tftp_ch == NULL) {
		return false;
	}
	memset(&h, 0, sizeof(h));
	h.version = 1;
	h.type = QRTR_TYPE_DATA;
	h.src_node_id = 1;
	h.src_port_id = lport;
	h.size = (uint32_t)len;
	h.dst_node_id = cnode;
	h.dst_port_id = cport;
	return sweet_glink_send_vec(&g_glink, g_tftp_ch, &h, sizeof(h), p, len);
}

/* QRTR flow control (af_qrtr.c qrtr_send_resume_tx): a packet with confirm_rx set wants a RESUME_TX back, else the sender stops. */
static void
sweet_qrtr_resume_tx(struct sweet_glink_channel *ch, const struct qrtr_hdr_v1 *qh)
{
	struct qrtr_hdr_v1 h;
	struct qrtr_ctrl_pkt c;
	static uint32_t sent;

	memset(&h, 0, sizeof(h));
	memset(&c, 0, sizeof(c));
	h.version = 1;
	h.type = QRTR_TYPE_RESUME_TX;
	h.src_node_id = qh->dst_node_id;
	h.src_port_id = qh->dst_port_id;
	h.size = sizeof(c);
	h.dst_node_id = qh->src_node_id;
	h.dst_port_id = QRTR_PORT_CTRL;
	c.cmd = QRTR_TYPE_RESUME_TX;
	c.client.node = qh->dst_node_id;
	c.client.port = qh->dst_port_id;
	if (sweet_glink_send_vec(&g_glink, ch, &h, sizeof(h), &c, sizeof(c))) {
		if (++sent <= 10) {
			ev("QRTR RESUME_TX sent for port %u (to %u:%u)", qh->dst_port_id, qh->src_node_id, qh->src_port_id);
		}
	} else {
		ev("QRTR RESUME_TX could not be sent (reason %d)", g_send_failure);
	}
}

static uint32_t
tftp_lport(const struct tftp_xfer *x)
{
	return TFTP_PORT + 1 + (uint32_t)(x - g_tftp);
}

static void
tftp_error(uint32_t cnode, uint32_t cport, uint32_t lport, unsigned code, const char *msg)
{
	uint8_t pkt[96];
	size_t n = strlen(msg);

	n = MIN(n, sizeof(pkt) - 5);
	pkt[0] = 0;
	pkt[1] = TFTP_ERROR;
	pkt[2] = (uint8_t)(code >> 8);
	pkt[3] = (uint8_t)code;
	memcpy(pkt + 4, msg, n);
	pkt[4 + n] = 0;
	(void)tftp_send_to(cnode, cport, lport, pkt, 5 + n);
}

static uint64_t
tftp_number(const char *s)
{
	uint64_t v = 0;

	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (uint64_t)(*s++ - '0');
	}
	return v;
}

/* A file's size and (optionally) bytes [offset, offset + length): 0, or an errno. */
static int
tftp_file(const char *path, uint64_t offset, void *buffer, int length, uint64_t *size)
{
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	int resid = 0, error;

	error = vnode_open(path, FREAD, 0, 0, &vnode, context);
	if (error != 0) {
		return error;
	}
	if (size != NULL) {
		VATTR_INIT(&attributes);
		VATTR_WANTED(&attributes, va_data_size);
		error = vnode_getattr(vnode, &attributes, context);
		*size = (uint64_t)attributes.va_data_size;
	}
	if (error == 0 && length > 0) {
		error = vn_rdwr(UIO_READ, vnode, (caddr_t)buffer, length, (off_t)offset, UIO_SYSSPACE, 0, vfs_context_ucred(context),
		    &resid, vfs_context_proc(context));
		if (error == 0 && resid != 0) {
			error = EIO;
		}
	}
	vnode_close(vnode, FREAD, context);
	return error;
}

/* The local file for a name the modem asks for; false for a name we do not serve. */
static bool
tftp_map(const char *request, char *out, size_t size)
{
	static const char *const prefixes[] = { "/readonly/firmware/image/", "/readonly/vendor/firmware_mnt/image/",
		                                "/readonly/vendor/firmware/", "/readonly/" };

	for (const char *s = request; *s; s++) {
		if (s[0] == '.' && s[1] == '.') {
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t n = strlen(prefixes[i]);

		if (strncmp(request, prefixes[i], n) == 0 && request[n] != 0) {
			snprintf(out, size, TFTP_DIR "%s", request + n);
			return true;
		}
	}
	return false;
}

static struct tftp_ramfile *
tftp_ram_find(const char *path, bool create)
{
	struct tftp_ramfile *free_slot = NULL;

	for (unsigned i = 0; i < TFTP_RAM_FILES; i++) {
		if (g_tftp_ram[i].used && strcmp(g_tftp_ram[i].path, path) == 0) {
			return &g_tftp_ram[i];
		}
		if (!g_tftp_ram[i].used && free_slot == NULL) {
			free_slot = &g_tftp_ram[i];
		}
	}
	if (!create || free_slot == NULL) {
		return NULL;
	}
	if (free_slot->data == NULL) {
		vm_offset_t memory = 0;

		if (kmem_alloc(kernel_map, &memory, TFTP_RAM_CAP, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return NULL;
		}
		free_slot->data = (uint8_t *)memory;
	}
	strlcpy(free_slot->path, path, sizeof(free_slot->path));
	free_slot->size = 0;
	free_slot->used = true;
	return free_slot;
}

static void
tftp_free(struct tftp_xfer *x)
{
	x->used = false;
}

static size_t
tftp_oack(uint8_t *p, const struct tftp_xfer *x, bool want_tsize)
{
	size_t at = 2;
	struct {
		const char *name;
		uint64_t value;
		bool present;
	} opts[] = {
		{ "blksize", x->blksize, true }, { "timeoutms", x->timeoutms, true }, { "tsize", x->fsize, want_tsize },
		{ "wsize", x->wsize, true }, { "rsize", x->rsize, x->rsize != 0 }, { "seek", x->seek, x->seek != 0 },
	};

	p[0] = 0;
	p[1] = TFTP_OACK;
	for (unsigned i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
		if (opts[i].present) {
			at += (size_t)snprintf((char *)p + at, 64, "%s", opts[i].name) + 1;
			at += (size_t)snprintf((char *)p + at, 24, "%llu", opts[i].value) + 1;
		}
	}
	return at;
}

static void
tftp_request(struct sweet_glink_channel *ch, const struct qrtr_hdr_v1 *qh, const uint8_t *p, uint32_t len, bool writer)
{
	struct tftp_xfer *x = NULL;
	const char *name = (const char *)p + 2, *end = (const char *)p + len, *s;
	bool options = false, want_tsize = false;
	uint32_t largest = 0;
	uint64_t size = 0;
	int error;

	(void)ch;
	for (unsigned i = 0; i < TFTP_XFERS; i++) {
		if (!g_tftp[i].used) {
			x = &g_tftp[i];
			break;
		}
	}
	s = name;
	while (s < end && *s) {
		s++;
	}
	if (s >= end || x == NULL) {
		hakios_say("SWEET TFTP: %s request dropped (%s)\n", writer ? "write" : "read", x == NULL ? "no free slot" : "bad name");
		return;
	}
	memset(x, 0, sizeof(*x));
	g_big_pending = 0;
	x->cnode = qh->src_node_id;
	x->cport = qh->src_port_id;
	x->writer = writer;
	x->blksize = 512;
	x->wsize = 1;
	x->timeoutms = 1000;
	strlcpy(x->path, name, sizeof(x->path));
	s++;                                    /* the mode ("octet") */
	while (s < end && *s) {
		s++;
	}
	s++;
	while (s < end) {                       /* option, value pairs */
		const char *option = s, *value;

		options = true;
		while (s < end && *s) {
			s++;
		}
		value = ++s;
		while (s < end && *s) {
			s++;
		}
		s++;
		if (value >= end) {
			break;
		}
		if (strcmp(option, "blksize") == 0) {
			x->blksize = (uint32_t)MIN(tftp_number(value), (uint64_t)65464);
		} else if (strcmp(option, "timeoutms") == 0) {
			x->timeoutms = (uint32_t)tftp_number(value);
		} else if (strcmp(option, "tsize") == 0) {
			want_tsize = true;
		} else if (strcmp(option, "wsize") == 0) {
			x->wsize = (uint32_t)MAX(tftp_number(value), (uint64_t)1);
		} else if (strcmp(option, "rsize") == 0) {
			x->rsize = tftp_number(value);
		} else if (strcmp(option, "seek") == 0) {
			x->seek = tftp_number(value);
		}
	}
	for (uint32_t i = 0; i < ch->num_intents; i++) {
		largest = MAX(largest, ch->intents[i].size);
	}
	if (largest > 28 + 4 + 16) {
		x->blksize = MIN(x->blksize, MIN(largest - 28 - 4, (uint32_t)TFTP_BUF - 4));
	}
	if (writer) {                          /* what the modem sends us must fit the 4096-byte intents we gave it */
		x->blksize = MIN(x->blksize, 3900u);
	}
	x->blksize = MAX(x->blksize, 8u);
	hakios_say("SWEET TFTP: %s '%s' from %u:%u (blksize %u wsize %u timeoutms %u rsize %llu seek %llu%s; largest intent %u)\n",
	    writer ? "WRQ" : "RRQ", x->path, x->cnode, x->cport, x->blksize, x->wsize, x->timeoutms, x->rsize, x->seek,
	    want_tsize ? " tsize" : "", largest);
	note("[TFTP %s %s]", writer ? "WRQ" : "RRQ", x->path);
	ev("TFTP %s '%s' blksize %u wsize %u rsize %llu seek %llu%s", writer ? "WRQ" : "RRQ", x->path, x->blksize, x->wsize,
	    x->rsize, x->seek, want_tsize ? " tsize" : "");
	x->used = true;
	if (writer) {
		if (strncmp(x->path, "/readwrite/", 11) != 0) {
			tftp_error(x->cnode, x->cport, tftp_lport(x), 2, "Access violation");
			tftp_free(x);
			return;
		}
		x->expected = 1;
		x->ram = tftp_ram_find(x->path, true);
		if (x->ram != NULL) {
			x->ram->size = 0;
		}
		if (options) {
			(void)tftp_send_to(x->cnode, x->cport, tftp_lport(x), g_tftp_buf, tftp_oack(g_tftp_buf, x, false));
		} else {
			uint8_t ack[4] = { 0, TFTP_ACK, 0, 0 };

			(void)tftp_send_to(x->cnode, x->cport, tftp_lport(x), ack, 4);
		}
		return;
	}
	if (strncmp(x->path, "/readwrite/", 11) == 0) {
		x->ram = tftp_ram_find(x->path, false);
		if (x->ram != NULL) {
			error = 0;
			size = x->ram->size;
			strlcpy((char *)g_tftp_buf, x->path, sizeof(g_tftp_buf));
		} else {
			error = ENOENT;
		}
	} else if (!tftp_map(x->path, (char *)g_tftp_buf, sizeof(g_tftp_buf))) {
		error = ENOENT;
	} else {
		error = tftp_file((const char *)g_tftp_buf, 0, NULL, 0, &size);
	}
	if (error != 0) {
		hakios_say("SWEET TFTP: no file for '%s'\n", x->path);
		note("[TFTP ENOENT]");
		ev("TFTP no file for '%s'", x->path);
		tftp_error(x->cnode, x->cport, tftp_lport(x), 1, "file not found");
		tftp_free(x);
		return;
	}
	strlcpy(x->path, (const char *)g_tftp_buf, sizeof(x->path));       /* now the local path */
	x->fsize = size;
	x->data_len = size > x->seek ? size - x->seek : 0;
	if (x->rsize != 0 && x->rsize < x->data_len) {
		x->data_len = x->rsize;
	}
	x->total_blocks = x->data_len / x->blksize + 1;
	x->next = 1;
	x->limit = options ? 0 : x->wsize;
	hakios_say("SWEET TFTP: serving %s: %llu bytes, %llu blocks of %u\n", x->path, size, x->total_blocks, x->blksize);
	ev("TFTP serving %s: %llu bytes, %llu blocks of %u", x->path, size, x->total_blocks, x->blksize);
	if (options) {
		(void)tftp_send_to(x->cnode, x->cport, tftp_lport(x), g_tftp_buf, tftp_oack(g_tftp_buf, x, want_tsize));
	}
}

/* Send the blocks of a reader's window that have not gone yet (retried each tick when the FIFO or the intents are full). */
static void
tftp_pump_reader(struct tftp_xfer *x)
{
	/* Keep a window's worth of receive buffers of the remote ready (and asked for) for the blocks to come. */
	{
		uint32_t need = 4 + x->blksize + 28, have = 0, want = MIN(x->wsize, 8u);

		for (uint32_t i = 0; i < g_tftp_ch->num_intents; i++) {
			if (!g_tftp_ch->intents[i].in_use && g_tftp_ch->intents[i].size >= need) {
				have++;
			}
		}
		if (have == 0 && g_pump_clock - g_last_req_clock > 8) {
			g_big_pending = 0;      /* nothing offered for 40 ms: assume the requests were lost and ask again */
		}
		while (have + g_big_pending < want && x->next <= x->total_blocks) {
			struct glink_msg req = { .cmd = GLINK_CMD_RX_INTENT_REQ, .param1 = g_tftp_ch->lcid, .param2 = need };

			sweet_glink_tx_cmd(&g_glink, &req, sizeof(req));
			g_big_pending++;
			g_last_req_clock = g_pump_clock;
			x->requests++;
			have++;
		}
	}
	while (x->next <= x->limit && x->next <= x->total_blocks) {
		uint64_t offset = (uint64_t)(x->next - 1) * x->blksize;
		uint32_t n = offset >= x->data_len ? 0 : (uint32_t)MIN((uint64_t)x->blksize, x->data_len - offset);
		int error = 0;

		g_tftp_buf[0] = 0;
		g_tftp_buf[1] = TFTP_DATA;
		g_tftp_buf[2] = (uint8_t)(x->next >> 8);
		g_tftp_buf[3] = (uint8_t)x->next;
		if (n != 0 && x->ram != NULL) {
			memcpy(g_tftp_buf + 4, x->ram->data + x->seek + offset, n);
		} else if (n != 0) {
			error = tftp_file(x->path, x->seek + offset, g_tftp_buf + 4, (int)n, NULL);
		}
		if (error != 0) {
			hakios_say("SWEET TFTP: cannot read %s at %llu (error %d)\n", x->path, x->seek + offset, error);
			tftp_error(x->cnode, x->cport, tftp_lport(x), 0, "Read error");
			tftp_free(x);
			return;
		}
		if (!tftp_send_to(x->cnode, x->cport, tftp_lport(x), g_tftp_buf, 4 + n)) {
			if (!x->stalled) {
				char intents[96];
				size_t at = 0;

				x->stalled = true;
				for (uint32_t i = 0; i < g_tftp_ch->num_intents && at < sizeof(intents) - 16; i++) {
					at += (size_t)snprintf(intents + at, sizeof(intents) - at, " %u%s", g_tftp_ch->intents[i].size,
					    g_tftp_ch->intents[i].in_use ? "*" : "");
				}
				ev("TFTP send of block %u stalled (reason %d): intents%s (* in use); fifo head %u tail %u", x->next, g_send_failure, intents,
				    g_glink.descs[1], g_glink.descs[0]);
			}
			return;
		}
		x->sent++;
		if (x->sent <= 12 || x->sent % 50 == 0) {
			ev("TFTP block %u sent (%u in all)", x->next, x->sent);
		}
		x->next++;
	}
}

static void
sweet_tftp_handle(struct sweet_glink_channel *ch, const struct qrtr_hdr_v1 *qh, const uint8_t *p, uint32_t len)
{
	struct tftp_xfer *x;
	unsigned op, block;

	g_tftp_ch = ch;
	if (len < 2) {
		return;
	}
	op = (unsigned)p[0] << 8 | p[1];
	if (qh->dst_port_id == TFTP_PORT) {
		if (op == TFTP_RRQ || op == TFTP_WRQ) {
			tftp_request(ch, qh, p, len, op == TFTP_WRQ);
		} else {
			hakios_say("SWEET TFTP: unhandled op %u from %u:%u\n", op, qh->src_node_id, qh->src_port_id);
		}
		return;
	}
	x = &g_tftp[qh->dst_port_id - TFTP_PORT - 1];
	if (!x->used || x->cnode != qh->src_node_id || x->cport != qh->src_port_id) {
		return;
	}
	x->idle = 0;
	if (op == TFTP_ERROR) {
		hakios_say("SWEET TFTP: client error %u on %s\n", len >= 4 ? (unsigned)p[2] << 8 | p[3] : 0, x->path);
		ev("TFTP client error %u on %s", len >= 4 ? (unsigned)p[2] << 8 | p[3] : 0, x->path);
		tftp_free(x);
	} else if (op == TFTP_ACK && !x->writer && len >= 4) {
		block = (unsigned)p[2] << 8 | p[3];
		x->acks++;
		x->last_ack = block;
		if (x->acks <= 12 || x->acks % 20 == 0) {
			ev("TFTP ack %u (%u acks, next %u)", block, x->acks, x->next);
		}
		if (block >= x->total_blocks) {
			hakios_say("SWEET TFTP: %s sent (%u blocks)\n", x->path, block);
			note("[TFTP done %u]", block);
			ev("TFTP sent %s (%u blocks)", x->path, block);
			tftp_free(x);
		} else {
			x->next = block + 1;
			x->limit = block + x->wsize;
			tftp_pump_reader(x);
		}
	} else if (op == TFTP_DATA && x->writer && len >= 4) {
		uint32_t payload = len - 4;

		block = (unsigned)p[2] << 8 | p[3];
		if (block != x->expected) {
			tftp_error(x->cnode, x->cport, tftp_lport(x), 4, "Block number out of sequence");
			tftp_free(x);
			return;
		}
		x->expected++;
		if (x->ram != NULL && x->ram->size + payload <= TFTP_RAM_CAP) {
			memcpy(x->ram->data + x->ram->size, p + 4, payload);
			x->ram->size += payload;
		}
		if (payload < x->blksize || block % x->wsize == 0) {
			uint8_t ack[4] = { 0, TFTP_ACK, (uint8_t)(block >> 8), (uint8_t)block };

			(void)tftp_send_to(x->cnode, x->cport, tftp_lport(x), ack, 4);
		}
		if (payload < x->blksize) {
			hakios_say("SWEET TFTP: write '%s' kept in memory (%u blocks, %u bytes)\n", x->path, block, x->ram != NULL ? x->ram->size : 0);
			note("[TFTP wr done]");
			tftp_free(x);
		}
	}
}

static void sweet_wlfw_pump(void);

static void
sweet_tftp_pump(void)
{
	g_pump_clock++;
	sweet_wlfw_pump();
	for (unsigned i = 0; i < TFTP_XFERS; i++) {
		struct tftp_xfer *x = &g_tftp[i];

		if (!x->used) {
			continue;
		}
		if (++x->idle > TFTP_IDLE_PUMPS) {
			hakios_say("SWEET TFTP: '%s' timed out\n", x->path);
			ev("TFTP '%s' timed out: sent %u, acks %u (last %u), next %u, intent requests %u", x->path, x->sent, x->acks,
			    x->last_ack, x->next, x->requests);
			tftp_free(x);
		} else if (!x->writer) {
			tftp_pump_reader(x);
		}
	}
}

/* ---- SERVREG_LOC (what pd-mapper does): which protection domain of the modem hosts a service ---- */

/*
 * QRTR service 64, version 1 (instance field 0x101 = version | instance << 8, as pd-mapper publishes it). The modem
 * asks where a service runs ("wlan/fw", "kernel/elf_loader", "tms/servreg" ...); the answer is the table of
 * qcom_pd_mapper.c for the sm7150: domain name, QMI instance id; every domain also hosts "tms/servreg".
 */
#define SERVREG_LOC_PORT        64
#define SERVREG_LOC_GET_DOMAINS 0x21

static const struct {
	const char *domain;
	uint32_t instance;
	const char *services[3];
} g_pdm[] = {
	{ "msm/adsp/audio_pd", 4, { NULL } },
	{ "msm/adsp/root_pd", 4, { NULL } },
	{ "msm/adsp/sensor_pd", 74, { NULL } },
	{ "msm/cdsp/root_pd", 76, { NULL } },
	{ "msm/modem/root_pd", 180, { "gps/gps_service", NULL } },
	{ "msm/modem/wlan_pd", 180, { "kernel/elf_loader", "wlan/fw", NULL } },
};

static bool
pdm_hosts(unsigned domain, const char *service)
{
	if (strcmp(service, "tms/servreg") == 0) {
		return true;
	}
	for (unsigned i = 0; g_pdm[domain].services[i] != NULL; i++) {
		if (strcmp(g_pdm[domain].services[i], service) == 0) {
			return true;
		}
	}
	return false;
}

static void
sweet_servreg_loc_handle(struct sweet_glink_channel *ch, const struct qrtr_hdr_v1 *qh, const uint8_t *p, uint32_t len)
{
	char name[66] = { 0 };
	uint32_t offset = 0, total = 0, listed = 0;
	uint8_t *out = g_tftp_buf, *count_at;
	uint16_t msg_len, txn, msg;
	uint32_t at = 7;

	g_tftp_ch = ch;
	if (len < 7) {
		return;
	}
	ev("SERVREG_LOC raw %u bytes: %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", len,
	    p[0], p[1], p[2], p[3], p[4], p[5], p[6], len > 7 ? p[7] : 0, len > 8 ? p[8] : 0, len > 9 ? p[9] : 0, len > 10 ? p[10] : 0,
	    len > 11 ? p[11] : 0, len > 12 ? p[12] : 0, len > 13 ? p[13] : 0, len > 14 ? p[14] : 0, len > 15 ? p[15] : 0,
	    len > 16 ? p[16] : 0);
	txn = (uint16_t)(p[1] | p[2] << 8);
	msg = (uint16_t)(p[3] | p[4] << 8);
	msg_len = (uint16_t)(p[5] | p[6] << 8);
	if (msg == 0x24) {
		/* PFR_REQ: "a protection domain failed, because ...": pd-mapper just acknowledges it */
		out[0] = 2; out[1] = (uint8_t)txn; out[2] = (uint8_t)(txn >> 8); out[3] = (uint8_t)msg; out[4] = (uint8_t)(msg >> 8);
		out[5] = 7; out[6] = 0;
		out[7] = 0x02; out[8] = 4; out[9] = 0; out[10] = 0; out[11] = 0; out[12] = 0; out[13] = 0;
		ev("SERVREG_LOC pfr acknowledged");
		(void)tftp_send_to(qh->src_node_id, qh->src_port_id, SERVREG_LOC_PORT, out, 14);
		return;
	}
	if (msg != SERVREG_LOC_GET_DOMAINS) {
		hakios_say("SWEET SERVREG_LOC: unhandled message %#x from %u:%u\n", msg, qh->src_node_id, qh->src_port_id);
		ev("SERVREG_LOC unhandled msg %#x", msg);
		return;
	}
	while (at + 3 <= len && at + 3 <= 7u + msg_len) {       /* TLVs: type, length, value */
		uint8_t type = p[at];
		uint16_t tlen = (uint16_t)(p[at + 1] | p[at + 2] << 8);

		if (at + 3 + tlen > len) {
			break;
		}
		if (type == 1 && tlen >= 1) {                   /* the service name: u8 length, characters */
			/* Prefixed by its length (qmi_encoding.c) or, as the modem seems to send it, just the characters. */
			bool prefixed = p[at + 3] == tlen - 1;
			uint32_t first = prefixed ? 4 : 3, n = MIN((uint32_t)tlen - (first - 3), (uint32_t)sizeof(name) - 1);

			memcpy(name, p + at + first, n);
			name[n] = 0;
		} else if (type == 0x10 && tlen >= 4) {
			offset = (uint32_t)(p[at + 3] | p[at + 4] << 8 | p[at + 5] << 16 | (uint32_t)p[at + 6] << 24);
		}
		at += 3 + tlen;
	}
	/* The response: result, total_domains, db_revision, the domain list. */
	out[0] = 2;                                             /* QMI response */
	out[1] = (uint8_t)txn;
	out[2] = (uint8_t)(txn >> 8);
	out[3] = (uint8_t)msg;
	out[4] = (uint8_t)(msg >> 8);
	at = 7;
	out[at++] = 0x02; out[at++] = 4; out[at++] = 0; out[at++] = 0; out[at++] = 0; out[at++] = 0; out[at++] = 0;
	count_at = out + at;                                    /* total_domains: filled in below */
	out[at++] = 0x10; out[at++] = 2; out[at++] = 0; out[at++] = 0; out[at++] = 0;
	out[at++] = 0x11; out[at++] = 2; out[at++] = 0; out[at++] = 1; out[at++] = 0;   /* db_revision 1 */
	{
		uint32_t list_at = at;
		uint32_t matched = 0;

		out[at++] = 0x12;
		at += 2;                                        /* the TLV length, below */
		out[at++] = 0;                                  /* the entry count, below */
		for (unsigned i = 0; i < sizeof(g_pdm) / sizeof(g_pdm[0]); i++) {
			if (!pdm_hosts(i, name)) {
				continue;
			}
			if (total++ >= offset && listed < 32) {
				uint32_t n = (uint32_t)strlen(g_pdm[i].domain);

				out[at++] = (uint8_t)n;
				memcpy(out + at, g_pdm[i].domain, n);
				at += n;
				out[at++] = (uint8_t)g_pdm[i].instance; out[at++] = (uint8_t)(g_pdm[i].instance >> 8);
				out[at++] = 0; out[at++] = 0;
				out[at++] = 0;                          /* service_data_valid */
				out[at++] = 0; out[at++] = 0; out[at++] = 0; out[at++] = 0;
				listed++;
			}
			matched++;
		}
		if (matched == 0) {
			at = list_at;                           /* no domain list TLV for an unknown service */
		} else {
			uint32_t tlv_len = at - list_at - 3;

			out[list_at + 1] = (uint8_t)tlv_len;
			out[list_at + 2] = (uint8_t)(tlv_len >> 8);
			out[list_at + 3] = (uint8_t)listed;
		}
	}
	count_at[3] = (uint8_t)total;
	count_at[4] = (uint8_t)(total >> 8);
	out[5] = (uint8_t)(at - 7);
	out[6] = (uint8_t)((at - 7) >> 8);
	hakios_say("SWEET SERVREG_LOC: get_domain_list '%s' offset %u -> %u of %u domains\n", name, offset, listed, total);
	note("[SERVREG '%s' %u/%u]", name, listed, total);
	ev("SERVREG_LOC get_domain_list '%s' offset %u -> %u of %u", name, offset, listed, total);
	(void)tftp_send_to(qh->src_node_id, qh->src_port_id, SERVREG_LOC_PORT, out, at);
}

/* ---- WLFW: the QMI service of the modem's WLAN firmware, and the host's side of its start-up (ath10k qmi.c) ---- */

/*
 * Service 69 appears once the modem has booted wlanmdsp.mbn. The host then registers for indications, tells it
 * its capabilities, gives it the memory it may use (the MSA region, handed over to the modem's and the WLAN's
 * VMIDs through TrustZone), asks for the chip's capabilities, downloads the board data, reports calibration,
 * sends the copy-engine configuration the host will use and finally the mode. All steps are requests with
 * a response; the state machine goes on from each response. Message layouts: ath10k qmi_wlfw_v01.c.
 */
#define WLFW_SERVICE            69
#define WLFW_CLIENT_PORT        100
#define WLFW_MSA_ADDR           0x98d00000ull
#define WLFW_MSA_SIZE           0x200000u
#define WLFW_CLIENT_ID          0x4b4e454cu
#define WLFW_BDF_PATH           "/hakios/fw/wlan/bdwlan-sunfish.bin"
#define WLFW_BDF_MAX            32768u
#define WLFW_CHUNK              6144u
#define WLFW_OUT_MAX            6400u
#define VMID_WLAN               0x18u
#define VMID_WLAN_CE            0x19u

#define WLFW_IND_REGISTER       0x20
#define WLFW_FW_READY_IND       0x21
#define WLFW_WLAN_MODE          0x22
#define WLFW_WLAN_CFG           0x23
#define WLFW_CAP                0x24
#define WLFW_BDF_DOWNLOAD       0x25
#define WLFW_CAL_REPORT         0x26
#define WLFW_MSA_READY_IND      0x2b
#define WLFW_MSA_INFO           0x2d
#define WLFW_MSA_READY          0x2e
#define WLFW_INI                0x2f
#define WLFW_HOST_CAP           0x34

enum { WL_IDLE, WL_IND_REG, WL_HOST_CAP, WL_MSA_INFO, WL_MSA_READY, WL_CAP, WL_BDF, WL_CAL, WL_WAIT_READY, WL_CFG, WL_MODE, WL_DONE, WL_FAILED };

static struct {
	int state;
	bool known, msa_ind, cap_done, pending, fw_ready;
	uint32_t node, port, out_len, bdf_len, bdf_sent, bdf_seg;
	uint16_t txn;
	uint8_t out[WLFW_OUT_MAX];
} g_wlfw;
static uint8_t g_wlfw_bdf[WLFW_BDF_MAX];
static uint32_t g_wlan_probe;            /* boot-arg wlanprobe=1: read the chip's copy-engine registers once the mode is set */

#define WCN_MMIO                0x18800000ull
#define WCN_MMIO_SIZE           0x800000u
#define WCN_CE_BASE             0x240000u               /* ce0; each next copy engine 0x1000 on */
#define WIMG_IO                 0x7u                    /* VM_WIMG_IO: guarded, coherent, not cacheable */

/*
 * wlanprobe=2 (read-only; a register WRITE to a copy engine hangs the bus): the firmware has already programmed
 * its copy-engine rings. Dump CE0's registers and the first descriptors with the memory they point to, if that
 * is inside the WLAN MSA region (0x98d00000...), to see whether the HTC ready message is already there.
 */
#define DMA_NBUF        8
#define DMA_BUFSZ       2048

/*
 * The firmware's rings sit in the MSA (taken from HLOS: reading them hangs), so point CE0's destination ring at host
 * memory instead and see whether the HTC ready message lands there: that answers "can the chip DMA into HLOS memory".
 * Descriptors are ath10k's ce_desc_64: addr (u64), nbytes (u16), flags (u16), toeplitz (u32).
 */
static void
wlfw_dma_test(volatile uint32_t *regs)
{
	vm_offset_t page = 0;
	uint8_t *p;
	uint64_t ring_pa, buf_pa;
	volatile uint32_t *ce = regs + WCN_CE_BASE / 4;      /* copy engine 0 */
	unsigned i, waited;

	if (kmem_alloc(kernel_map, &page, 4 * PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		ev("WLAN DMA: no memory");
		return;
	}
	p = (uint8_t *)page;
	ring_pa = kvtophys(page);
	buf_pa = kvtophys(page + PAGE_SIZE);
	memset(p + PAGE_SIZE, 0xa5, DMA_NBUF * DMA_BUFSZ);
	for (i = 0; i < DMA_NBUF; i++) {
		((uint64_t *)(p + 16 * i))[0] = buf_pa + (uint64_t)i * DMA_BUFSZ;
	}
	for (vm_offset_t o = 0; o < 4 * PAGE_SIZE; o += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (p + o) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	ev("WLAN DMA: ring pa %#llx buffers pa %#llx; before: dr_base %#x/%#x dr_size %#x dst_wr %#x drri %#x",
	    ring_pa, buf_pa, ce[3], ce[4], ce[5], ce[0x40 / 4], ce[0x48 / 4]);
	ev("WLAN DMA: writing dr_base");
	ce[3] = (uint32_t)ring_pa;
	ce[4] = (uint32_t)(ring_pa >> 32);
	__asm__ volatile ("dsb sy" ::: "memory");
	ev("WLAN DMA: dr_base now %#x/%#x; writing dst_wr", ce[3], ce[4]);
	ce[0x40 / 4] = DMA_NBUF;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (waited = 0; waited < 3000; waited += 100) {
		sleep_ms(100);
		if (ce[0x48 / 4] != 0) {
			break;
		}
	}
	ev("WLAN DMA: after %u ms: dst_wr %#x drri %#x", waited, ce[0x40 / 4], ce[0x48 / 4]);
	for (vm_offset_t o = 0; o < 4 * PAGE_SIZE; o += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (p + o) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (i = 0; i < 2; i++) {
		uint64_t *d = (uint64_t *)(p + 16 * i);
		uint8_t *bf = p + PAGE_SIZE + i * DMA_BUFSZ;

		ev("WLAN DMA: desc%u addr %#llx nbytes %#x flags %#x", i, d[0] & 0x1fffffffffull, (unsigned)(d[1] & 0xffff),
		    (unsigned)((d[1] >> 16) & 0xffff));
		ev("WLAN DMA: buf%u %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", i,
		    bf[0], bf[1], bf[2], bf[3], bf[4], bf[5], bf[6], bf[7], bf[8], bf[9], bf[10], bf[11], bf[12], bf[13], bf[14], bf[15]);
	}
}

/*
 * wlanprobe=3: read-only look at the apps SMMU (sm7150.dtsi: apps_smmu 0x15000000) for the WLAN stream
 * (iommus = <&apps_smmu 0x240 0x1>): global control, and every valid stream match entry with its S2CR.
 */
#define APPS_SMMU       0x15000000ull
#define APPS_SMMU_SIZE  0x10000u

static void
wlfw_smmu_dump(bool setup)
{
	volatile uint32_t *g = (volatile uint32_t *)ml_io_map_unmappable(APPS_SMMU, APPS_SMMU_SIZE, WIMG_IO);
	uint32_t idr0, nsmr, valid = 0, free_smr = ~0u;

	if (g == NULL) {
		ev("WLAN SMMU: cannot map");
		return;
	}
	idr0 = g[0x20 / 4];
	nsmr = idr0 & 0xff;
	ev("WLAN SMMU: scr0 %#x idr0 %#x idr1 %#x sgfsr %#x sgfar %#x", g[0], idr0, g[0x24 / 4], g[0x48 / 4], g[0x50 / 4]);
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = g[(0x800 + 4 * i) / 4], s2cr = g[(0xc00 + 4 * i) / 4];

		if (!(smr >> 31) && free_smr == ~0u) {
			free_smr = i;
		}
		if (smr >> 31) {
			valid++;
			if (valid <= 40 || (smr & 0x7fff) == 0x240) {
				ev("WLAN SMMU: smr%u %#x s2cr%u %#x", i, smr, i, s2cr);
			}
		}
	}
	ev("WLAN SMMU: %u stream match registers, %u valid", nsmr, valid);
	if (setup && free_smr != ~0u) {
		/* wlanprobe=4: stream 0x240/0x241 unmatched faults (scr0.USFCFG): give it a bypass entry (S2CR type 1) */
		g[(0xc00 + 4 * free_smr) / 4] = 0x10000;
		g[(0x800 + 4 * free_smr) / 4] = 0x80000000u | (0x1u << 16) | 0x240;
		__asm__ volatile ("dsb sy" ::: "memory");
		ev("WLAN SMMU: wrote smr%u -> reads %#x s2cr %#x, sgfsr %#x", free_smr, g[(0x800 + 4 * free_smr) / 4],
		    g[(0xc00 + 4 * free_smr) / 4], g[0x48 / 4]);
	}
}

/*
 * wlanprobe=5: an identity-mapped SMMU context bank for the WLAN streams, then host destination rings on CE1/CE2
 * (the host's ends; the rings the dump above showed on CE0/3/4 are the TARGET's, in the MSA) and a wait for the
 * firmware's HTC ready message. The recipe is what the forks' OnePlus 7T and Beryllium work found for the same
 * hypervisor: bypass is refused (S2CR reads back as fault), a translating stage-1 bank with a fixed 1 GiB-block
 * identity table is accepted. Every register is read back; a bank that does not take is put back.
 */
#define CB_SCTLR        0x00
#define CB_TCR2         0x10
#define CB_TTBR0        0x20
#define CB_TTBR1        0x28
#define CB_TCR          0x30
#define CB_MAIR0        0x38
#define CB_MAIR1        0x3c
#define CB_FSR          0x58
#define CB_TLBIALL      0x618
#define CB_TLBSYNC      0x7f0
#define CB_TLBSTATUS    0x7f4

static volatile uint32_t *g_smmu;
static int g_smmu_bank = -1;
static volatile uint64_t *g_smmu_tab;           /* the bank's four 4 KiB tables: +0 identity L1, +0x1000 L0, +0x2000 sink L1, +0x3000 sink L2 */
static uint64_t g_smmu_tab_pa;
static uint8_t *g_sink;                         /* 2 MiB of host memory standing in for whatever the firmware writes to */
static uint64_t g_sink_pa, g_sink_far;

static uint64_t
smmu_rd64(uint32_t off)
{
	return g_smmu[off / 4] | ((uint64_t)g_smmu[off / 4 + 1] << 32);
}

static void
smmu_wr(uint32_t off, uint32_t v)
{
	g_smmu[off / 4] = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
wlfw_smmu_bank_setup(uint32_t page, uint32_t cbo, uint32_t cb, uint32_t pasize, uint64_t table_pa)
{
	uint64_t ttbr0 = table_pa | ((uint64_t)cb << 48);
	uint32_t sctlr = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 12);      /* M TRE AFE CFRE ASIDPNE */
	bool ok;

	smmu_wr(cbo + CB_SCTLR, 0);
	smmu_wr(page + 0x800 + 4 * cb, 1u);                                             /* CBA2R: VA64 */
	smmu_wr(page + 4 * cb, (1u << 16) | (3u << 8) | (0xfu << 12));                  /* CBAR: stage 1, S2 bypass */
	smmu_wr(cbo + CB_TCR2, (pasize & 7u) | (7u << 15));
	smmu_wr(cbo + CB_TTBR0, (uint32_t)ttbr0);
	smmu_wr(cbo + CB_TTBR0 + 4, (uint32_t)(ttbr0 >> 32));
	smmu_wr(cbo + CB_TTBR1, 0);
	smmu_wr(cbo + CB_TTBR1 + 4, 0);
	smmu_wr(cbo + CB_TCR, 16u | (1u << 23));                                        /* T0SZ 16 (48 bits), 4K, non-cacheable walks, EPD1 */
	smmu_wr(cbo + CB_MAIR0, 0x44u);                                                 /* attr 0: normal non-cacheable */
	smmu_wr(cbo + CB_MAIR1, 0);
	smmu_wr(cbo + CB_FSR, g_smmu[(cbo + CB_FSR) / 4]);
	smmu_wr(cbo + CB_TLBIALL, 0);
	smmu_wr(cbo + CB_TLBSYNC, 0);
	for (int i = 0; i < 1000 && (g_smmu[(cbo + CB_TLBSTATUS) / 4] & 1u); i++) {
		sleep_ms(1);
	}
	smmu_wr(cbo + CB_SCTLR, sctlr);
	ok = smmu_rd64(cbo + CB_TTBR0) == ttbr0 && (g_smmu[(cbo + CB_SCTLR) / 4] & 1u) && (g_smmu[(cbo + CB_TCR) / 4] & 0x3fu) == 16u;
	ev("WLAN SMMU: bank %u CBAR %#x TCR %#x TTBR0 %#llx SCTLR %#x: %s", cb, g_smmu[(page + 4 * cb) / 4], g_smmu[(cbo + CB_TCR) / 4],
	    smmu_rd64(cbo + CB_TTBR0), g_smmu[(cbo + CB_SCTLR) / 4], ok ? "taken" : "NOT taken");
	if (!ok) {
		smmu_wr(cbo + CB_SCTLR, 0);
	}
	return ok;
}

static bool
wlfw_smmu_identity(uint32_t sid)
{
	uint32_t idr0, idr1, idr2, nsmr, ncb, page, cb_base, pasize, smr_i = ~0u, free_smr = ~0u;
	uint8_t used[128] = { 0 };
	int cb = g_smmu_bank;

	if (g_smmu == NULL) {
		g_smmu = (volatile uint32_t *)ml_io_map_unmappable(APPS_SMMU, 0x100000, WIMG_IO);
		if (g_smmu == NULL) {
			return false;
		}
	}
	idr0 = g_smmu[0x20 / 4];
	idr1 = g_smmu[0x24 / 4];
	idr2 = g_smmu[0x28 / 4];
	nsmr = MIN(idr0 & 0xffu, 128u);
	ncb = MIN(idr1 & 0xffu, 128u);
	page = (idr1 >> 31) ? 0x10000 : 0x1000;
	cb_base = (1u << (((idr1 >> 28) & 7u) + 1u)) * page;
	pasize = MIN((idr2 >> 4) & 0xfu, 2u);
	for (uint32_t i = 0; i < nsmr; i++) {
		uint32_t smr = g_smmu[(0x800 + 4 * i) / 4], s2cr = g_smmu[(0xc00 + 4 * i) / 4];

		if (!(smr & (1u << 31))) {
			if (free_smr == ~0u) {
				free_smr = i;
			}
			continue;
		}
		if (((s2cr >> 16) & 3u) == 0 && (s2cr & 0xffu) < 128) {
			used[s2cr & 0xffu] = 1;
		}
		if ((((sid ^ smr) & ~(smr >> 16)) & 0x7fffu) == 0) {
			smr_i = i;
		}
	}
	if (smr_i == ~0u) {
		smr_i = free_smr;
	}
	if (smr_i == ~0u) {
		ev("WLAN SMMU: no free SMR for %#x", sid);
		return false;
	}
	if (cb < 0) {
		vm_offset_t pt = 0;
		volatile uint64_t *l1;
		unsigned tries = 0;

		if (kmem_alloc(kernel_map, &pt, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
		g_smmu_tab = (volatile uint64_t *)pt;
		g_smmu_tab_pa = kvtophys(pt);
		l1 = (volatile uint64_t *)pt;
		g_smmu_tab[0x1000 / 8] = g_smmu_tab_pa | 3;      /* L0[0] -> the identity L1 */
		for (unsigned i = 2; i < 16; i++) {      /* 1 GiB blocks, 0x80000000..0x3ffffffff: PA | AF | SH outer | AP RW | attr 0 | block */
			l1[i] = ((uint64_t)i << 30) | (1u << 10) | (2u << 8) | (1u << 6) | 1u;
		}
		for (int o = 0; o < 0x4000; o += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" ((uint8_t *)pt + o) : "memory");
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		for (int n = (int)ncb - 1; n >= 0 && tries < 3; n--) {
			uint32_t cbo = cb_base + (uint32_t)n * page;

			if (used[n] || cbo + page > 0x100000) {
				continue;
			}
			ev("WLAN SMMU: trying bank %d (sctlr at %#x)", n, cbo);
			if (g_smmu[(cbo + CB_SCTLR) / 4] & 1u) {
				ev("WLAN SMMU: bank %d is in use", n);
				continue;
			}
			tries++;
			if (wlfw_smmu_bank_setup(page, cbo, (uint32_t)n, pasize, g_smmu_tab_pa + 0x1000)) {
				cb = n;
				break;
			}
		}
		if (cb < 0) {
			ev("WLAN SMMU: no context bank taken");
			return false;
		}
		g_smmu_bank = cb;
	}
	smmu_wr(0xc00 + 4 * smr_i, (uint32_t)cb);                                       /* S2CR: translate, bank */
	smmu_wr(0x800 + 4 * smr_i, (1u << 31) | sid);                                   /* SMR: valid, exact */
	ev("WLAN SMMU: sid %#x -> smr%u %#x s2cr %#x (bank %d)", sid, smr_i, g_smmu[(0x800 + 4 * smr_i) / 4], g_smmu[(0xc00 + 4 * smr_i) / 4], cb);
	return (g_smmu[(0x800 + 4 * smr_i) / 4] == ((1u << 31) | sid)) && ((g_smmu[(0xc00 + 4 * smr_i) / 4] >> 16) & 3u) == 0;
}

/*
 * For the audio DSP's driver (hakios.adsp_sm7150.c): a translating bank of its own for one stream, `pages' 4 KiB pages
 * of host memory at `pa' shown at `iova' (below 4 GiB, inside one 2 MiB stretch). The DSP is given 32-bit addresses and
 * this kernel's memory lies above 4 GiB, so the WLAN's identity bank is no use to it. The same recipe as that bank:
 * stage 1, tables of 4 KiB (four of them in one 16 KiB page: +0 L0, +0x1000 L1, +0x2000 L2, +0x3000 L3).
 */
bool hakios_sm7150_smmu_pages(uint32_t sid, uint64_t iova, uint64_t pa, uint32_t pages);
bool
hakios_sm7150_smmu_pages(uint32_t sid, uint64_t iova, uint64_t pa, uint32_t pages)
{
	uint32_t idr0, idr1, idr2, nsmr, ncb, page, cb_base, pasize, smr_i = ~0u, free_smr = ~0u;
	uint8_t used[128] = { 0 };
	vm_offset_t pt = 0;
	volatile uint64_t *t;
	uint64_t pt_pa;
	int cb = -1;
	unsigned tries = 0;

	if (pages == 0 || pages > 512 - ((iova >> 12) & 511) || ((iova | pa) & 0xfff) != 0 || iova >= (1ull << 32)) {
		return false;
	}
	if (g_smmu == NULL) {
		g_smmu = (volatile uint32_t *)ml_io_map_unmappable(APPS_SMMU, 0x100000, WIMG_IO);
		if (g_smmu == NULL) {
			return false;
		}
	}
	idr0 = g_smmu[0x20 / 4];
	idr1 = g_smmu[0x24 / 4];
	idr2 = g_smmu[0x28 / 4];
	nsmr = MIN(idr0 & 0xffu, 128u);
	ncb = MIN(idr1 & 0xffu, 128u);
	page = (idr1 >> 31) ? 0x10000 : 0x1000;
	cb_base = (1u << (((idr1 >> 28) & 7u) + 1u)) * page;
	pasize = MIN((idr2 >> 4) & 0xfu, 2u);
	for (uint32_t i = 0; i < nsmr; i++) {
		uint32_t smr = g_smmu[(0x800 + 4 * i) / 4], s2cr = g_smmu[(0xc00 + 4 * i) / 4];

		if (!(smr & (1u << 31))) {
			if (free_smr == ~0u) {
				free_smr = i;
			}
			continue;
		}
		if (((s2cr >> 16) & 3u) == 0 && (s2cr & 0xffu) < 128) {
			used[s2cr & 0xffu] = 1;
		}
		if ((smr & 0x80007fffu) == ((1u << 31) | sid)) {
			smr_i = i;              /* an exact entry for this stream already */
		}
	}
	if (smr_i == ~0u) {
		smr_i = free_smr;
	}
	if (smr_i == ~0u || kmem_alloc_contig(kernel_map, &pt, 0x4000, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		ev("SMMU: no stream match register or no memory for %#x", sid);
		return false;
	}
	t = (volatile uint64_t *)pt;
	pt_pa = kvtophys(pt);
	t[(iova >> 39) & 511] = (pt_pa + 0x1000) | 3;                           /* L0 -> L1 */
	t[0x1000 / 8 + ((iova >> 30) & 511)] = (pt_pa + 0x2000) | 3;            /* L1 -> L2 */
	t[0x2000 / 8 + ((iova >> 21) & 511)] = (pt_pa + 0x3000) | 3;            /* L2 -> L3 */
	for (uint32_t i = 0; i < pages; i++) {                                  /* pages: PA | AF | SH outer | AP RW | attr 0 | page */
		t[0x3000 / 8 + ((iova >> 12) & 511) + i] = (pa + ((uint64_t)i << 12)) | (1u << 10) | (2u << 8) | (1u << 6) | 3u;
	}
	for (int o = 0; o < 0x4000; o += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" ((uint8_t *)pt + o) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int n = (int)ncb - 1; n >= 0 && tries < 3; n--) {
		uint32_t cbo = cb_base + (uint32_t)n * page;

		if (used[n] || n == g_smmu_bank || cbo + page > 0x100000 || (g_smmu[(cbo + CB_SCTLR) / 4] & 1u)) {
			continue;
		}
		tries++;
		if (wlfw_smmu_bank_setup(page, cbo, (uint32_t)n, pasize, pt_pa)) {
			cb = n;
			break;
		}
	}
	if (cb < 0) {
		ev("SMMU: no context bank taken for %#x", sid);
		return false;
	}
	smmu_wr(0xc00 + 4 * smr_i, (uint32_t)cb);                               /* S2CR: translate, bank */
	smmu_wr(0x800 + 4 * smr_i, (1u << 31) | sid);                           /* SMR: valid, exact */
	ev("SMMU: sid %#x -> smr%u %#x s2cr %#x (bank %d), %u pages at iova %#llx -> pa %#llx", sid, smr_i, g_smmu[(0x800 + 4 * smr_i) / 4],
	    g_smmu[(0xc00 + 4 * smr_i) / 4], cb, pages, iova, pa);
	return (g_smmu[(0x800 + 4 * smr_i) / 4] == ((1u << 31) | sid)) && ((g_smmu[(0xc00 + 4 * smr_i) / 4] >> 16) & 3u) == 0;
}

/*
 * For the camera driver (hakios.camera_sunfish.c): like hakios_sm7150_smmu_pages, but for a device that writes on
 * several stream IDs it does not tell us exactly (the IFE: downstream msm_cam_smmu_ife lists masked groups). One
 * translating bank with `pages' 4 KiB pages of `pa' at a 32-bit `iova' (up to eight 2 MiB stretches), and one MASKED SMR per
 * (sid, mask) pair, all pointing at that bank. A group that overlaps a valid SMR someone else owns is skipped (a
 * double match would be a stream-match conflict fault). Returns the bank, or -1. hakios_sm7150_smmu_status() reads
 * that bank's fault state and the global fault syndrome back.
 */
static uint32_t g_cam_cbo;
#define CAM_SMMU_L3_MAX 8                       /* L3 tables: 8 x 2 MiB = 16 MiB of IOVA */
static int
smmu_cam_map(const uint32_t *sids, const uint32_t *masks, unsigned n, uint64_t iova, const uint64_t *pas, unsigned nchunks,
    uint32_t chunk_pages)
{
	uint32_t idr0, idr1, idr2, nsmr, ncb, page, cb_base, pasize;
	uint32_t smr_for[8];
	uint8_t used[128] = { 0 }, taken_smr[128] = { 0 };
	vm_offset_t pt = 0;
	volatile uint64_t *t;
	uint64_t pt_pa;
	int cb = -1;
	unsigned tries = 0, mapped = 0;

	uint32_t pages = nchunks * chunk_pages, l3n = (uint32_t)((((iova >> 12) & 511) + pages + 511) / 512);

	if (n == 0 || n > 8 || nchunks == 0 || chunk_pages == 0 || l3n > CAM_SMMU_L3_MAX || (iova & 0xfff) != 0 ||
	    iova >= (1ull << 32) || ((iova >> 21) & 511) + l3n > 512) {
		return -1;
	}
	for (unsigned c = 0; c < nchunks; c++) {
		if (pas[c] & 0xfff) {
			return -1;
		}
	}
	if (g_smmu == NULL) {
		g_smmu = (volatile uint32_t *)ml_io_map_unmappable(APPS_SMMU, 0x100000, WIMG_IO);
		if (g_smmu == NULL) {
			return -1;
		}
	}
	idr0 = g_smmu[0x20 / 4];
	idr1 = g_smmu[0x24 / 4];
	idr2 = g_smmu[0x28 / 4];
	nsmr = MIN(idr0 & 0xffu, 128u);
	ncb = MIN(idr1 & 0xffu, 128u);
	page = (idr1 >> 31) ? 0x10000 : 0x1000;
	cb_base = (1u << (((idr1 >> 28) & 7u) + 1u)) * page;
	pasize = MIN((idr2 >> 4) & 0xfu, 2u);
	/* every group: an SMR of its own, or skipped if a valid SMR already covers any of its streams */
	for (unsigned k = 0; k < n; k++) {
		uint32_t id = sids[k] & 0x7fffu, mk = masks[k] & 0x7fffu, conflict = ~0u, mine = ~0u, free_i = ~0u;

		smr_for[k] = ~0u;
		for (uint32_t i = 0; i < nsmr; i++) {
			uint32_t smr = g_smmu[(0x800 + 4 * i) / 4], s2cr = g_smmu[(0xc00 + 4 * i) / 4];
			uint32_t sid_i = smr & 0x7fffu, mask_i = (smr >> 16) & 0x7fffu;

			if (!(smr & (1u << 31))) {
				if (free_i == ~0u && !taken_smr[i]) {
					free_i = i;
				}
				continue;
			}
			if (((s2cr >> 16) & 3u) == 0 && (s2cr & 0xffu) < 128) {
				used[s2cr & 0xffu] = 1;
			}
			if (sid_i == id && mask_i == mk) {
				mine = i;                       /* the same group already has an entry: take it over */
			} else if ((((id ^ sid_i) & ~(mk | mask_i)) & 0x7fffu) == 0) {
				conflict = i;
			}
		}
		if (mine != ~0u) {
			smr_for[k] = mine;
		} else if (conflict != ~0u) {
			ev("SMMU cam: group %#x/%#x overlaps smr%u %#x s2cr %#x - skipped", id, mk, conflict,
			    g_smmu[(0x800 + 4 * conflict) / 4], g_smmu[(0xc00 + 4 * conflict) / 4]);
		} else if (free_i != ~0u) {
			smr_for[k] = free_i;
			taken_smr[free_i] = 1;
		} else {
			ev("SMMU cam: no free SMR for %#x/%#x", id, mk);
		}
	}
	/* L0, L1, L2, then the L3 tables back to back (16 KiB pages: 0xc000 holds 3 + 8 tables of 4 KiB) */
	if (kmem_alloc_contig(kernel_map, &pt, 0xc000, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return -1;
	}
	t = (volatile uint64_t *)pt;
	pt_pa = kvtophys(pt);
	t[(iova >> 39) & 511] = (pt_pa + 0x1000) | 3;                           /* L0 -> L1 */
	t[0x1000 / 8 + ((iova >> 30) & 511)] = (pt_pa + 0x2000) | 3;            /* L1 -> L2 */
	for (uint32_t l = 0; l < l3n; l++) {
		t[0x2000 / 8 + ((iova >> 21) & 511) + l] = (pt_pa + 0x3000 + 0x1000 * l) | 3;   /* L2 -> L3 (tables contiguous) */
	}
	for (uint32_t i = 0; i < pages; i++) {
		uint64_t pa = pas[i / chunk_pages] + ((uint64_t)(i % chunk_pages) << 12);
		t[0x3000 / 8 + ((iova >> 12) & 511) + i] = pa | (1u << 10) | (2u << 8) | (1u << 6) | 3u;
	}
	for (int o = 0; o < 0xc000; o += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" ((uint8_t *)pt + o) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int b = (int)ncb - 1; b >= 0 && tries < 3; b--) {
		uint32_t cbo = cb_base + (uint32_t)b * page;
		if (used[b] || b == g_smmu_bank || cbo + page > 0x100000 || (g_smmu[(cbo + CB_SCTLR) / 4] & 1u)) {
			continue;
		}
		tries++;
		if (wlfw_smmu_bank_setup(page, cbo, (uint32_t)b, pasize, pt_pa)) {
			cb = b;
			g_cam_cbo = cbo;
			break;
		}
	}
	if (cb < 0) {
		ev("SMMU cam: no context bank taken");
		return -1;
	}
	for (unsigned k = 0; k < n; k++) {
		uint32_t i = smr_for[k], want = (1u << 31) | ((masks[k] & 0x7fffu) << 16) | (sids[k] & 0x7fffu);

		if (i == ~0u) {
			continue;
		}
		smmu_wr(0xc00 + 4 * i, (uint32_t)cb);                                   /* S2CR: translate, bank */
		smmu_wr(0x800 + 4 * i, want);                                           /* SMR: valid, masked */
		ev("SMMU cam: %#x/%#x -> smr%u %#x s2cr %#x (bank %d)", sids[k], masks[k], i, g_smmu[(0x800 + 4 * i) / 4],
		    g_smmu[(0xc00 + 4 * i) / 4], cb);
		if (g_smmu[(0x800 + 4 * i) / 4] == want && ((g_smmu[(0xc00 + 4 * i) / 4] >> 16) & 3u) == 0) {
			mapped++;
		}
	}
	ev("SMMU cam: bank %d, %u of %u stream groups mapped, %u pages iova %#llx in %u chunks", cb, mapped, n, pages, iova, nchunks);
	return mapped ? cb : -1;
}

int hakios_sm7150_smmu_pages_masked(const uint32_t *sids, const uint32_t *masks, unsigned n, uint64_t iova, uint64_t pa, uint32_t pages);
int
hakios_sm7150_smmu_pages_masked(const uint32_t *sids, const uint32_t *masks, unsigned n, uint64_t iova, uint64_t pa, uint32_t pages)
{
	return smmu_cam_map(sids, masks, n, iova, &pa, 1, pages);
}

/* the same with `nchunks' physically separate pieces of `chunk_pages' each, back to back from `iova' (at most 16 MiB) */
int hakios_sm7150_smmu_chunks_masked(const uint32_t *sids, const uint32_t *masks, unsigned n, uint64_t iova, const uint64_t *pas,
    unsigned nchunks, uint32_t chunk_pages);
int
hakios_sm7150_smmu_chunks_masked(const uint32_t *sids, const uint32_t *masks, unsigned n, uint64_t iova, const uint64_t *pas,
    unsigned nchunks, uint32_t chunk_pages)
{
	return smmu_cam_map(sids, masks, n, iova, pas, nchunks, chunk_pages);
}

/* the camera bank's fault status (FSR, FAR) and the global fault status + syndrome (GFSYNR1 low half = the stream) */
void hakios_sm7150_smmu_status(uint32_t *fsr, uint64_t *far, uint32_t *gfsr, uint32_t *gfsynr1);
void
hakios_sm7150_smmu_status(uint32_t *fsr, uint64_t *far, uint32_t *gfsr, uint32_t *gfsynr1)
{
	*fsr = 0;
	*far = 0;
	*gfsr = 0;
	*gfsynr1 = 0;
	if (g_smmu == NULL) {
		return;
	}
	if (g_cam_cbo != 0) {
		*fsr = g_smmu[(g_cam_cbo + CB_FSR) / 4];
		*far = smmu_rd64(g_cam_cbo + 0x60);
	}
	*gfsr = g_smmu[0x48 / 4];
	*gfsynr1 = g_smmu[0x54 / 4];
}


#define CE_RX_BUF       2048

/*
 * AOSS QMP (drivers/soc/qcom/qcom_aoss.c): the always-on processor's mailbox in message RAM at 0xc300000, doorbell
 * apss_shared 0x17c00000 + 12 (bit 0). The SM8150 forks found the WCN3990 firmware needs AOSS's "qdss" clock; its
 * WCSS QDSS (CoreSight) block is read by the firmware's tasks and a gated clock starves them (our "Task starvation:
 * WLAN RT/BE" crashes and a repeating write fault at a garbage address).
 */
#define AOP_MSGRAM      0x0c300000ull
#define AOP_UP          0xffffu

static volatile uint32_t *g_aop, *g_apcs;

static bool
aop_wait(uint32_t off, uint32_t value)
{
	for (unsigned t = 0; t < 1000; t++) {
		if (g_aop[off / 4] == value) {
			return true;
		}
		sleep_ms(1);
	}
	return false;
}

static void
aop_kick(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	g_apcs[12 / 4] = 1;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
aop_send(const char *msg)
{
	uint32_t offset, size;
	char buf[64];

	if (g_aop == NULL) {
		g_aop = (volatile uint32_t *)ml_io_map_unmappable(AOP_MSGRAM, 0x1000, WIMG_IO);
		g_apcs = (volatile uint32_t *)ml_io_map_unmappable(0x17c00000, 0x1000, WIMG_IO);
		if (g_aop == NULL || g_apcs == NULL) {
			g_aop = NULL;
			return false;
		}
	}
	if (g_aop[0] != 0x4d41494c || g_aop[1] != 1) {
		ev("AOP: bad magic %#x version %u", g_aop[0], g_aop[1]);
		return false;
	}
	offset = g_aop[0x38 / 4];
	size = g_aop[0x34 / 4];
	if (size == 0 || offset + 68 > 0x1000) {
		ev("AOP: mailbox size %u offset %#x", size, offset);
		return false;
	}
	if (g_aop[0x28 / 4] != AOP_UP || g_aop[0x30 / 4] != AOP_UP) {
		g_aop[0x10 / 4] = g_aop[0xc / 4];                       /* ack the remote's link state */
		g_aop[0x24 / 4] = AOP_UP;
		aop_kick();
		if (!aop_wait(0x28, AOP_UP)) {
			ev("AOP: ucore did not ack the link");
			return false;
		}
		g_aop[0x2c / 4] = AOP_UP;
		aop_kick();
		if (!aop_wait(0x14, AOP_UP)) {
			ev("AOP: ucore did not open the channel");
			return false;
		}
		g_aop[0x18 / 4] = AOP_UP;
		aop_kick();
		if (!aop_wait(0x30, AOP_UP)) {
			ev("AOP: ucore did not ack the channel");
			return false;
		}
	}
	memset(buf, 0, sizeof(buf));
	strlcpy(buf, msg, sizeof(buf));
	for (unsigned i = 0; i < 16; i++) {
		g_aop[(offset + 4) / 4 + i] = ((const uint32_t *)buf)[i];
	}
	g_aop[offset / 4] = 64;
	(void)g_aop[offset / 4];
	aop_kick();
	if (!aop_wait(offset, 0)) {
		g_aop[offset / 4] = 0;
		ev("AOP: no ack for \"%s\"", msg);
		return false;
	}
	ev("AOP: sent \"%s\"", msg);
	return true;
}

/* For the audio DSP's driver (hakios.adsp_sm7150.c): the mailbox is one, and this file's sender is the only one. */
bool hakios_sm7150_aop_send(const char *msg);
bool
hakios_sm7150_aop_send(const char *msg)
{
	return aop_send(msg);
}

/* map the 2 MiB around a faulting address to a host buffer, to see what the firmware writes there */
static void
wp_sink_map(uint64_t far)
{
	uint32_t cbo = 0x80000u + (uint32_t)g_smmu_bank * 0x1000u;
	unsigned i0 = (unsigned)((far >> 39) & 0x1ff), i1 = (unsigned)((far >> 30) & 0x1ff), i2 = (unsigned)((far >> 21) & 0x1ff);
	vm_offset_t m = 0;

	if (g_smmu_tab == NULL || far < (1ull << 39) || g_sink_far) {
		return;
	}
	if (kmem_alloc_contig(kernel_map, &m, 0x200000, PAGE_MASK, 0, (0x200000 >> PAGE_SHIFT) - 1, KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		ev("SINK: no memory");
		return;
	}
	g_sink = (uint8_t *)m;
	g_sink_pa = kvtophys(m);
	if (g_sink_pa & 0x1fffff) {
		ev("SINK: block at %#llx is not 2 MiB aligned", g_sink_pa);
		return;
	}
	g_smmu_tab[0x1000 / 8 + i0] = (g_smmu_tab_pa + 0x2000) | 3;
	g_smmu_tab[0x2000 / 8 + i1] = (g_smmu_tab_pa + 0x3000) | 3;
	g_smmu_tab[0x3000 / 8 + i2] = g_sink_pa | (1u << 10) | (2u << 8) | (1u << 6) | 1u;
	for (int o = 0; o < 0x4000; o += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" ((uint8_t *)g_smmu_tab + o) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	smmu_wr(cbo + CB_TLBIALL, 0);
	smmu_wr(cbo + CB_TLBSYNC, 0);
	for (int i = 0; i < 1000 && (g_smmu[(cbo + CB_TLBSTATUS) / 4] & 1u); i++) {
		sleep_ms(1);
	}
	g_sink_far = far;
	ev("SINK: %#llx (L0 %u L1 %u L2 %u) now maps host memory at %#llx", far & ~0x1fffffull, i0, i1, i2, g_sink_pa);
}

/* diagnostic: a pending translation fault in our SMMU bank, with where we were when we noticed */
static void
wp_fault_check(const char *where)
{
	uint32_t cbo = 0x80000u + (uint32_t)g_smmu_bank * 0x1000u, fsr;

	if (g_smmu == NULL || g_smmu_bank < 0) {
		return;
	}
	fsr = g_smmu[(cbo + 0x58) / 4];
	if (fsr) {
		uint64_t far = ((uint64_t)g_smmu[(cbo + 0x64) / 4] << 32) | g_smmu[(cbo + 0x60) / 4];

		static unsigned nfaults;

		if (nfaults++ < 2 || far != g_sink_far) {
			ev("FAULT at %s: fsr %#x far %#llx fsynr0 %#x (%u so far)", where, fsr, far, g_smmu[(cbo + 0x68) / 4], nfaults);
		}
		wp_sink_map(far);
		g_smmu[(cbo + 0x58) / 4] = fsr;
	}
}

static void
wlfw_ce_rx_test(volatile uint32_t *regs)
{
	vm_offset_t mem = 0;
	uint8_t *m;
	uint64_t pa;
	/* CE2 gets 64 buffers, CE1 32: rings at +0 / +0x800, buffers at +0x4000 / +0x24000 */
	const struct { unsigned ce, n, ring, bufs; } rx[2] = { { 2, 64, 0, 0x4000 }, { 1, 32, 0x800, 0x24000 } };
	unsigned waited, hits = 0;

	if (!wlfw_smmu_identity(0x240) || !wlfw_smmu_identity(0x241)) {
		ev("WLAN CE: SMMU not set up, stopping");
		return;
	}
	if (kmem_alloc(kernel_map, &mem, 0x40000, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return;
	}
	m = (uint8_t *)mem;
	pa = kvtophys(mem);
	for (unsigned k = 0; k < 2; k++) {
		for (unsigned i = 0; i < rx[k].n; i++) {
			/* kmem_alloc memory is not physically contiguous: each buffer's own address */
			*(uint64_t *)(m + rx[k].ring + 16 * i) = kvtophys((vm_offset_t)(m + rx[k].bufs + (uint64_t)i * CE_RX_BUF)) & 0x1fffffffffull;
		}
	}
	for (vm_offset_t o = 0; o < 0x40000; o += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (m + o) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	ev("WLAN CE: memory pa %#llx", pa);
	for (unsigned k = 0; k < 2; k++) {
		volatile uint32_t *ce = regs + (WCN_CE_BASE + 0x1000 * rx[k].ce) / 4;
		uint64_t rpa = pa + rx[k].ring;

		ce[3] = (uint32_t)rpa;
		ce[4] = (uint32_t)(rpa >> 32) & 0x1f;
		ce[5] = rx[k].n;
		__asm__ volatile ("dsb sy" ::: "memory");
		ce[0x40 / 4] = rx[k].n - 1;
		__asm__ volatile ("dsb sy" ::: "memory");
		ev("WLAN CE%u: dest ring %u entries at %#llx: base %#x/%#x size %#x wr %#x ctrl1 %#x", rx[k].ce, rx[k].n, rpa, ce[3], ce[4], ce[5],
		    ce[0x40 / 4], ce[0x10 / 4 + 0]);
	}
	for (waited = 0; waited < 4000 && hits == 0; waited += 100) {
		sleep_ms(100);
		for (vm_offset_t o = 0; o < 0x1000; o += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (m + o) : "memory");
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		for (unsigned k = 0; k < 2; k++) {
			for (unsigned i = 0; i < 4; i++) {
				uint16_t nb = *(volatile uint16_t *)(m + rx[k].ring + 16 * i + 8);

				if (nb != 0) {
					hits++;
				}
			}
		}
	}
	ev("WLAN CE: after %u ms: %u completed descriptors", waited, hits);
	for (unsigned k = 0; k < 2; k++) {
		for (vm_offset_t o = 0; o < 2 * CE_RX_BUF; o += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (m + rx[k].bufs + o) : "memory");
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (unsigned k = 0; k < 2; k++) {
		volatile uint32_t *ce = regs + (WCN_CE_BASE + 0x1000 * rx[k].ce) / 4;

		ev("WLAN CE%u: drri %#x dst_wr %#x", rx[k].ce, ce[0x48 / 4], ce[0x40 / 4]);
		for (unsigned i = 0; i < 2; i++) {
			uint8_t *d = m + rx[k].ring + 16 * i, *b = m + rx[k].bufs + (uint64_t)i * CE_RX_BUF;

			ev("WLAN CE%u: desc%u nbytes %u flags %#x; buf %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", rx[k].ce, i,
			    *(uint16_t *)(d + 8), *(uint16_t *)(d + 10), b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
			    b[13], b[14], b[15]);
		}
	}
}

/*
 * wlanprobe=6: the host side of HTC and WMI (ath10k htc.c, wmi-tlv.c; ported from the Beryllium fork's 1118/1119).
 * Blocking, run once from the WLFW handler after the mode request, at most ~20 s: SMMU bank, a 4 MiB physically
 * contiguous pool (rings and buffers in the first 1 MiB, the firmware's host memory chunks after it), the four host
 * rings (source CE0 = HTC control tx, CE3 = WMI tx; destination CE1 = HTT rx, CE2 = control/WMI rx), HTC ready,
 * connect HTT and WMI, setup complete, WMI service ready -> init -> ready (the MAC address).
 */
#define WP_POOL         0x1000000u
#define WP_HOSTMEM      0x100000u
#define WP_BUF          2048u
#define WP_NR           10
#define WP_HTT_FRAG     0x400000u
#define WP_HTT_RXRING   0x420000u
#define WP_HTT_ALLOCIDX 0x424000u
#define WP_HTT_RXBUF    0x800000u
#define WP_HTT_RINGSZ   2048u
#define WP_HTT_BUFSZ    2048u
#define WP_HTT_FRAGS    1056u

static const struct wp_ring_cfg {
	unsigned ce, n, ring, bufs;
	bool src;
	uint32_t dmax;
} wp_cfg[WP_NR] = {
	{ 0, 32, 0x000, 0x10000, true, 0x800 },         /* 0: CE0 HTC control tx */
	{ 3, 32, 0x400, 0x20000, true, 0x800 },         /* 1: CE3 WMI tx */
	{ 1, 128, 0x2000, 0xc00000, false, 0 },         /* 2: CE1 HTT/HTC rx */
	{ 2, 64, 0xc00, 0x40000, false, 0 },            /* 3: CE2 WMI rx */
	{ 4, 64, 0x1000, 0x60000, true, 0x100 },        /* 4: CE4 HTT host to target: control messages and TX_FRM gather pairs */
	/* The other target-to-host pipes of ath10k's host_ce_config_wlan (CE5, CE8-CE11): without a destination ring the firmware's transfers
	 * wait for one (an unexplained repeating write fault at a garbage address), and with a small ring it stalls when the host lags.
	 * ath10k uses 512 entries; 128 here, buffers in the pool's last 4 MiB. */
	{ 5, 128, 0x2800, 0xc40000, false, 0 },
	{ 8, 128, 0x3000, 0xc80000, false, 0 },
	{ 9, 128, 0x3800, 0xcc0000, false, 0 },
	{ 10, 128, 0x4000, 0xd00000, false, 0 },
	{ 11, 128, 0x4800, 0xd40000, false, 0 },
};

static struct {
	volatile uint32_t *regs;
	uint8_t *pool;
	uint64_t pa;
	uint32_t wr[WP_NR], sw[WP_NR];
	uint8_t pend[WP_NR];                            /* destination ring slots consumed but not yet posted again (see wp_repost4) */
	uint8_t seq[8], wmi_eid, htt_eid, mac[6];
	bool htc_ready, got_conn, svc_ready, svc_avail, wmi_ready, ack_rssi, ext_addr;
	uint8_t conn_st, conn_eid;
	uint16_t conn_svc, conn_max, credits, credit_size;
	uint32_t wmi_credits, nchunks, svc[32], svc_bytes, msgs;
	bool htt_conf, scan_started, scan_done, scan_setup;
	uint32_t rx_idx;                                /* the HTT RX ring: next slot to post */
	uint32_t echo_sent, echo_ok;
	uint8_t htt_major, htt_minor;
	unsigned beacons, nbss, foreign;
	struct {
		uint8_t bssid[6];
		uint16_t mhz;
		int8_t rssi;
		char ssid[33];
		uint16_t cap, bint, ie_len;
		uint8_t ie[512];
		uint8_t seen, missed;                   /* heard during the scan in progress / scans in a row it was not heard in (see wp_bss_prune) */
	} bss[48];
	struct {
		uint32_t req_id, len;
		uint64_t pa;
	} chunk[16];
	uint32_t req_id[16], req_len[16], nreq;
} wp;

struct wp_desc {
	uint64_t addr;
	uint16_t nbytes, flags;
	uint32_t hash;
};

struct wp_rsrc {
	uint32_t num_vdevs, num_peers, num_offload_peers, num_offload_reorder_bufs, num_peer_keys, num_tids,
	    ast_skid_limit, tx_chain_mask, rx_chain_mask, rx_timeout_pri[4], rx_decap_mode, scan_max_pending_reqs,
	    bmiss_offload_max_vdev, roam_offload_max_vdev, roam_offload_max_ap_profiles, num_mcast_groups,
	    num_mcast_table_elems, mcast2ucast_mode, tx_dbg_log_size, num_wds_entries, dma_burst_size, mac_aggr_delim,
	    rx_skip_defrag_timeout_dup_detection_check, vow_config, gtk_offload_max_vdev, num_msdu_desc,
	    max_frag_entries, num_tdls_vdevs, num_tdls_conn_table_entries, beacon_tx_offload_max_vdev,
	    num_multicast_filter_entries, num_wow_filters, num_keep_alive_pattern, keep_alive_pattern_size,
	    max_tdls_concurrent_sleep_sta, max_tdls_concurrent_buffer_sta, wmi_send_separate, num_ocb_vdevs,
	    num_ocb_channels, num_ocb_schedules, host_capab;
};

static void
wp_sync(const void *p, size_t n)
{
	for (uintptr_t a = (uintptr_t)p & ~63ul; a < (uintptr_t)p + n; a += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (a) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static volatile uint32_t *
wp_ce(unsigned ce)
{
	return wp.regs + (WCN_CE_BASE + 0x1000 * ce) / 4;
}

static struct wp_desc *
wp_desc(unsigned r, unsigned i)
{
	return (struct wp_desc *)(wp.pool + wp_cfg[r].ring) + i;
}

static void
wp_repost(unsigned r)
{
	struct wp_desc *d = wp_desc(r, wp.wr[r]);

	d->addr = (wp.pa + wp_cfg[r].bufs + (uint64_t)wp.wr[r] * WP_BUF) & 0x1fffffffffull;
	d->nbytes = 0;
	wp_sync(d, sizeof(*d));
	wp.wr[r] = (wp.wr[r] + 1) & (wp_cfg[r].n - 1);
	wp_ce(wp_cfg[r].ce)[0x40 / 4] = wp.wr[r];
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * A destination ring descriptor is 16 bytes and the CE hardware writes the received length into it, but the cache line is 64 bytes and
 * wp_sync cleans whole lines: posting one slot wrote back stale copies of the three neighbouring slots, and a message the hardware had
 * completed in the meantime was wiped (the ring then waited for it for ever: no HTT rx indications or TX completions any more, while WMI on
 * its own, quiet ring went on). So slots are posted four at a time, a whole aligned line of slots the hardware does not own.
 */
static void
wp_repost4(unsigned r)
{
	unsigned wr = wp.wr[r];

	for (unsigned j = 0; j < 4; j++) {
		struct wp_desc *d = wp_desc(r, wr + j);

		d->addr = (wp.pa + wp_cfg[r].bufs + (uint64_t)(wr + j) * WP_BUF) & 0x1fffffffffull;
		d->nbytes = 0;
	}
	wp_sync(wp_desc(r, wr), 64);
	wp.wr[r] = (wr + 4) & (wp_cfg[r].n - 1);
	wp_ce(wp_cfg[r].ce)[0x40 / 4] = wp.wr[r];
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* ath10k_ce_init_pipe for the four host rings; false if memory is missing */
static bool
wp_rings(void)
{
	vm_offset_t mem = 0;

	if (wp.pool != NULL) {                          /* a restart: the pool is kept, cleaned */
		memset(wp.pool, 0, WP_POOL);
		wp_sync(wp.pool, WP_POOL);
		goto have_pool;
	}
	/* below 4 GiB if possible: some of the firmware's structures may have 32-bit address fields (an unexplained write fault at a
	 * garbage address repeated every few seconds with the pool at 0x10b5...) */
	if (kmem_alloc_contig(kernel_map, &mem, WP_POOL, PAGE_MASK, (0x100000000ull >> PAGE_SHIFT) - 1, 0, KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS &&
	    kmem_alloc_contig(kernel_map, &mem, WP_POOL, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		ev("WMI: no contiguous pool");
		return false;
	}
	wp.pool = (uint8_t *)mem;
	wp.pa = kvtophys(mem);
	ev("WMI: pool pa %#llx", wp.pa);
have_pool:
	for (unsigned r = 0; r < WP_NR; r++) {
		volatile uint32_t *ce = wp_ce(wp_cfg[r].ce);
		uint64_t rpa = wp.pa + wp_cfg[r].ring;
		unsigned n = wp_cfg[r].n;

		if (wp_cfg[r].src) {
			wp.sw[r] = ce[0x44 / 4] & (n - 1);
			wp.wr[r] = ce[0x3c / 4] & (n - 1);
			ce[0] = (uint32_t)rpa;
			ce[1] = (ce[1] & ~0x1fu) | ((uint32_t)(rpa >> 32) & 0x1f);
			ce[2] = n;
			ce[0x18 / 4] = (ce[0x18 / 4] & ~(0xffffu | (1u << 17))) | wp_cfg[r].dmax;     /* dmax, no byte swap */
		} else {
			wp.sw[r] = ce[0x48 / 4] & (n - 1);
			wp.wr[r] = ce[0x40 / 4] & (n - 1);
			ce[3] = (uint32_t)rpa;
			ce[4] = (ce[4] & ~0x1fu) | ((uint32_t)(rpa >> 32) & 0x1f);
			ce[5] = n;
			ce[0x18 / 4] = ce[0x18 / 4] & ~(1u << 18);
			/* a ring that starts on a line boundary keeps one line back, so that every later post is a whole free line */
			for (unsigned k = 0, cnt = (wp.wr[r] & 3) == 0 ? n - 4 : n - 1; k < cnt; k++) {
				wp_repost(r);
			}
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		if (r < 5) {
			ev("WMI: CE%u %s ring %u at %#llx sw %u wr %u ctrl1 %#x", wp_cfg[r].ce, wp_cfg[r].src ? "src" : "dst", n, rpa, wp.sw[r], wp.wr[r],
			    ce[0x18 / 4]);
		}
	}
	return true;
}

static void wp_hist(uint32_t key);
static void wp_hist_dump(void);

/* HTC send on source ring r (0 = CE0, 1 = CE3): 8-byte header + payload; the write index goes through the shadow registers */
static bool
wp_send(unsigned r, uint8_t eid, const void *payload, uint32_t plen)
{
	unsigned mask = wp_cfg[r].n - 1, wr = wp.wr[r];
	uint32_t srri = (g_wlan_halt || g_wlan_crashed || g_modem_crashed || wp.regs == NULL) ? 0 : (wp_ce(wp_cfg[r].ce)[0x44 / 4] & mask);   /* the registers are gone while a restart is under way */
	uint8_t *buf = wp.pool + wp_cfg[r].bufs + (uint64_t)wr * WP_BUF;
	struct wp_desc *d = wp_desc(r, wr);

	if (g_wlan_halt || g_wlan_crashed || g_modem_crashed || wp.regs == NULL) {
		return false;
	}
	uint16_t l = (uint16_t)plen;

	if (8 + plen > WP_BUF || ((srri - 1 - wr) & mask) == 0) {
		return false;
	}
	if (eid != 0 && eid == wp.wmi_eid) {
		if (wp.wmi_credits == 0) {
			return false;
		}
		wp.wmi_credits--;
	}
	wp_hist(0x30000000u | ((uint32_t)eid << 20) | (plen >= 4 ? (*(const uint32_t *)payload & 0xfffff) : 0));
	buf[0] = eid;
	buf[1] = (eid != 0 && eid == wp.wmi_eid) ? 1 : 0;      /* NEED_CREDIT_UPDATE on the credit-flow endpoint */
	memcpy(buf + 2, &l, 2);
	buf[4] = 0;
	buf[5] = wp.seq[eid & 7]++;
	buf[6] = buf[7] = 0;
	memcpy(buf + 8, payload, plen);
	wp_sync(buf, 8 + plen);
	d->addr = (wp.pa + wp_cfg[r].bufs + (uint64_t)wr * WP_BUF) & 0x1fffffffffull;
	d->nbytes = (uint16_t)(8 + plen);
	d->flags = (uint16_t)(((uint32_t)eid << 4) & 0xfff0);   /* transfer id = endpoint (meta data) */
	d->hash = 0;
	wp_sync(d, sizeof(*d));
	wr = (wr + 1) & mask;
	wp.wr[r] = wr;
	wp.regs[(wp_cfg[r].ce == 0 ? 0x32000u : (wp_cfg[r].ce == 3 ? 0x3200cu : 0x32010u)) / 4] = wr;
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}

static bool
wp_svc_on(uint32_t id)
{
	uint32_t len = wp.svc_bytes, words = MIN(len / 4, 32u), w;

	if (id < len) {
		w = id / 4;
		return w < words && ((wp.svc[w] >> (id % 4)) & 1);
	}
	if (id >= 256) {
		return false;
	}
	w = (id - len) / 32;
	return w < words && ((wp.svc[w] >> ((id - len) % 32)) & 1);
}

static bool
wp_connect(uint16_t svc)
{
	uint8_t m[8] = { 2, 0 };
	uint16_t flags = svc == 0x0100 ? (uint16_t)(1u << 8) : (uint16_t)(1u << 3);

	memcpy(m + 2, &svc, 2);
	memcpy(m + 4, &flags, 2);
	wp.got_conn = false;
	wp.conn_svc = svc;
	return wp_send(0, 0, m, sizeof(m));
}

/* WMI_TLV service ready: capabilities, service bitmap, host memory requests (ath10k_wmi_tlv_op_pull_svc_rdy_ev) */
static void
wp_service_ready(const uint8_t *t, uint32_t len)
{
	const uint8_t *evp = NULL, *bmap = NULL, *reqs = NULL;
	uint16_t evl = 0, bmapl = 0, reqsl = 0;
	uint32_t e[22];

	for (uint32_t o = 0; o + 4 <= len;) {
		uint16_t l, tag;

		memcpy(&l, t + o, 2);
		memcpy(&tag, t + o + 2, 2);
		if (o + 4 + l > len) {
			break;
		}
		if (tag == 32) {
			evp = t + o + 4;
			evl = l;
		} else if (tag == 16 && !bmap) {
			bmap = t + o + 4;
			bmapl = l;
		} else if (tag == 18) {
			reqs = t + o + 4;
			reqsl = l;
		}
		o += 4 + l;
	}
	if (!evp || evl < 88 || !bmap || !reqs) {
		ev("WMI: malformed SERVICE_READY");
		return;
	}
	memset(e, 0, sizeof(e));
	memcpy(e, evp, 88);
	memset(wp.svc, 0, sizeof(wp.svc));
	memcpy(wp.svc, bmap, MIN((uint32_t)bmapl, (uint32_t)sizeof(wp.svc)));
	wp.svc_bytes = bmapl;
	ev("WMI: SERVICE_READY fw %#x abi %#x ns %#x %#x phy_cap %#x chains %u ht %#x vht %#x, %u memory requests, service map %u bytes",
	    e[0], e[1], e[3], e[4], e[7], e[9], e[10], e[11], e[18], bmapl);
	wp.nreq = 0;
	for (uint32_t o = 0; o + 4 <= reqsl && wp.nreq < 16;) {
		uint16_t l, tag;
		uint32_t q[4], units, usize;

		memcpy(&l, reqs + o, 2);
		memcpy(&tag, reqs + o + 2, 2);
		if (o + 4 + l > reqsl || tag != 34 || l < 16) {
			break;
		}
		memcpy(q, reqs + o + 4, 16);
		units = q[3];
		usize = (q[1] + 3) & ~3u;
		if (q[2] & 6) {
			units = 33 + 1;                         /* NUM_UNITS_IS_NUM_(ACTIVE_)PEERS */
		} else if (q[2] & 1) {
			units = 4 + 1;                          /* NUM_UNITS_IS_NUM_VDEVS */
		}
		wp.req_id[wp.nreq] = q[0];
		wp.req_len[wp.nreq] = units * usize;
		ev("WMI: memory request %u: id %u, %u units of %u (info %#x) = %u bytes", wp.nreq, q[0], units, q[1], q[2], units * usize);
		wp.nreq++;
		o += 4 + l;
	}
	wp.svc_ready = true;
}

static void
wp_send_init(void)
{
	uint8_t c[4 + 4 + 28 + 4 + sizeof(struct wp_rsrc) + 4 + 16 * 20];
	uint32_t n = 4, cmd = 0x1, off = WP_HOSTMEM;
	uint16_t tl, tg;
	struct wp_rsrc cfg;
	bool reorder = wp_svc_on(65);                   /* WMI_TLV_SERVICE_RX_FULL_REORDER */
	uint32_t abi[7];

	for (unsigned k = 0; k < wp.nreq; k++) {
		if ((uint64_t)off + wp.req_len[k] > WP_HTT_FRAG) {
			ev("WMI: memory request %u does not fit the pool", k);
			return;
		}
		wp.chunk[k].req_id = wp.req_id[k];
		wp.chunk[k].len = wp.req_len[k];
		wp.chunk[k].pa = wp.pa + off;
		off = (off + wp.req_len[k] + 0xfff) & ~0xfffu;
	}
	wp.nchunks = wp.nreq;
	memcpy(c, &cmd, 4);
	tl = 28;
	tg = 74;                                        /* WMI_TLV_TAG_STRUCT_INIT_CMD */
	memcpy(c + n, &tl, 2);
	memcpy(c + n + 2, &tg, 2);
	abi[0] = 0x01000000u; abi[1] = 53; abi[2] = 0x5f414351u; abi[3] = 0x00004c4du; abi[4] = 0; abi[5] = 0; abi[6] = wp.nchunks;
	memcpy(c + n + 4, abi, 28);
	n += 4 + 28;
	memset(&cfg, 0, sizeof(cfg));
	cfg.num_vdevs = 4;
	cfg.num_peers = 33;
	cfg.ast_skid_limit = 16;
	cfg.num_wds_entries = 2;
	cfg.num_offload_peers = reorder ? 4 : 0;
	cfg.num_offload_reorder_bufs = reorder ? 4 : 0;
	cfg.num_peer_keys = 2;
	cfg.num_tids = 33 * 2;
	cfg.tx_chain_mask = 7;
	cfg.rx_chain_mask = 7;
	cfg.rx_timeout_pri[0] = cfg.rx_timeout_pri[1] = cfg.rx_timeout_pri[2] = 0x64;
	cfg.rx_timeout_pri[3] = 0x28;
	cfg.rx_decap_mode = 1;                          /* native wifi */
	cfg.scan_max_pending_reqs = 4;
	cfg.bmiss_offload_max_vdev = 4;
	cfg.roam_offload_max_vdev = 4;
	cfg.roam_offload_max_ap_profiles = 8;
	cfg.tx_dbg_log_size = 0x400;
	cfg.gtk_offload_max_vdev = 2;
	cfg.num_msdu_desc = 1024 + 32;
	cfg.max_frag_entries = 2;
	cfg.num_tdls_vdevs = 1;
	cfg.num_tdls_conn_table_entries = 0x20;
	cfg.beacon_tx_offload_max_vdev = 2;
	cfg.num_multicast_filter_entries = 5;
	cfg.num_wow_filters = 22;
	cfg.num_keep_alive_pattern = 6;
	cfg.max_tdls_concurrent_sleep_sta = 1;
	cfg.max_tdls_concurrent_buffer_sta = 1;
	cfg.host_capab = 1u << 9;
	if (wp.ack_rssi) {
		cfg.host_capab |= 1u << 18;
	}
	tl = sizeof(cfg);
	tg = 75;                                        /* WMI_TLV_TAG_STRUCT_RESOURCE_CONFIG */
	memcpy(c + n, &tl, 2);
	memcpy(c + n + 2, &tg, 2);
	memcpy(c + n + 4, &cfg, sizeof(cfg));
	n += 4 + (uint32_t)sizeof(cfg);
	tl = (uint16_t)(wp.nchunks * 20);
	tg = 18;                                        /* WMI_TLV_TAG_ARRAY_STRUCT */
	memcpy(c + n, &tl, 2);
	memcpy(c + n + 2, &tg, 2);
	n += 4;
	for (unsigned k = 0; k < wp.nchunks; k++) {
		uint32_t ch[4] = { wp.chunk[k].req_id, (uint32_t)wp.chunk[k].pa, wp.chunk[k].len, wp.ext_addr ? (uint32_t)(wp.chunk[k].pa >> 32) : 0 };

		tl = 16;
		tg = 76;                                /* WMI_TLV_TAG_STRUCT_HOST_MEMORY_CHUNK */
		memcpy(c + n, &tl, 2);
		memcpy(c + n + 2, &tg, 2);
		memcpy(c + n + 4, ch, 16);
		n += 20;
	}
	ev("WMI: INIT %u bytes, %u chunks, reorder %u, ack rssi %u, ext addr %u", n, wp.nchunks, reorder, wp.ack_rssi, wp.ext_addr);
	if (!wp_send(1, wp.wmi_eid, c, n)) {
		ev("WMI: INIT not sent");
	}
}



static struct {
	bool active, hs_done, auth_resp, assoc_resp, kicked, start_resp, stopped, vdev_started, vdev_up, peer, peer_mapped, sec_ind;
	bool svc_mgmt_wmi, mgmt_busy, ptk_installed, key_pending[2], connected;
	uint8_t ssid[32], ssid_len, pmk[32], bssid[6], dtim, mgmt_rate, group_type, ptk[16], gtk[16], sta_rsn[32];
	int bss, mgmt_status, eapol_msdu;
	uint16_t mhz, aid, peer_id, auth_status, seq;
	uint32_t start_status, gtk_idx, mgmt_slot, tx_inflight, sta_rsn_len;
	uint64_t tx_frames, tx_eapol, tx_fail, rx_drop, rx_eapol, rx_frames, rx_logged;
	uint8_t txfree[256];
	struct swu_assoc_resp ar;
	struct swu_wpa wpa;
} jn;

static bool jn_mgmt(const uint8_t *fr, uint32_t frl);
static bool jn_wmi_event(uint32_t evid, const uint8_t *t, uint32_t len);
static bool jn_htt(const uint8_t *pl, uint32_t plen);

/* ---- passive scan (the fork's 1120/1123): HTT version + setup, WMI chain masks, channels, regdomain, vdev, start scan ---- */
/*
 * The station's address: the phone's own when the loader passed it on (/chosen hakios-wlan-address, from ABL's
 * wlan_mac1 by way of U-Boot), else this made-up one -- the same in every build, so two phones on one network clashed.
 * Taken once, before the VDEV_CREATE that gives it to the firmware (wp_mac_take); never printed.
 */
static uint8_t wp_mac[6] = { 0x02, 0x00, 0x5e, 0x77, 0xe0, 0x01 };

static void
wp_mac_take(void)
{
	static bool taken;
	DTEntry chosen;
	const void *real = NULL;
	unsigned int size = 0;

	if (taken) {
		return;
	}
	taken = true;
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "hakios-wlan-address", &real, &size) == kSuccess && size == 6) {
		memcpy(wp_mac, real, 6);
		hakios_say("wlan: the address is the phone's own, from its bootloader\n");
	}
}
static const uint16_t wp_chans[] = {
	2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
	5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
	5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700, 5720,     /* channels 100-144: DFS, passive only (home routers use them) */
	5745, 5765, 5785, 5805, 5825,
};
#define WP_NCHAN        ((unsigned)(sizeof(wp_chans) / sizeof(wp_chans[0])))

static void
wp_scan_event(const uint8_t *t, uint32_t len)
{
	uint16_t tl, tag;
	uint32_t e[6];

	if (len < 4 + 24) {
		return;
	}
	memcpy(&tl, t, 2);
	memcpy(&tag, t + 2, 2);
	if (tag != 36 || tl < 24) {
		return;
	}
	memcpy(e, t + 4, 24);
	if (e[0] & 0x8) {
		wp.foreign++;
	}
	if (e[0] & 0x1) {
		wp.scan_started = true;
		ev("WMI: scan STARTED id %#x", e[4]);
	}
	if (e[0] & 0x40) {
		ev("WMI: scan START FAILED reason %u", e[1]);
		wp.scan_done = true;
	}
	if (e[0] & 0x2) {
		ev("WMI: scan COMPLETED reason %u, %u channel visits, %u beacons, %u BSS", e[1], wp.foreign, wp.beacons, wp.nbss);
		wp.scan_done = true;
	}
}

static void
wp_mgmt_rx(const uint8_t *t, uint32_t len)
{
	const uint8_t *h = NULL, *fr = NULL;
	uint32_t frl = 0, hd[6];
	unsigned k;
	uint16_t rxmhz, bssmhz;

	for (uint32_t o = 0; o + 4 <= len;) {
		uint16_t l, tag;

		memcpy(&l, t + o, 2);
		memcpy(&tag, t + o + 2, 2);
		if (o + 4 + l > len) {
			break;
		}
		if (tag == 44 && l >= 40) {
			h = t + o + 4;
		} else if (tag == 17) {
			fr = t + o + 4;
			frl = l;
		}
		o += 4u + l;
	}
	if (!h || !fr) {
		return;
	}
	memcpy(hd, h, 24);
	frl = MIN(frl, hd[4]);
	if (hd[5] & 0x29) {
		return;
	}
	if (jn.active && jn_mgmt(fr, frl)) {
		return;
	}
	if (jn.active && frl >= 36 && fr[0] != 0x80 && fr[0] != 0x50) {
		static unsigned nmg;

		/* frames the host does not answer (block-ack action frames, ...): note the first few, to see what the AP expects */
		if (nmg < 24 && memcmp(fr + 16, jn.bssid, 6) == 0) {
			nmg++;
			ev("MGMT rx fc %02x%02x len %u: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", fr[0], fr[1], frl, fr[24], fr[25], fr[26], fr[27],
			    fr[28], fr[29], fr[30], fr[31], fr[32], fr[33], fr[34], fr[35]);
		}
	}
	if (frl < 36 || (fr[0] != 0x80 && fr[0] != 0x50)) {
		return;
	}
	wp.beacons++;
	/* 2.4 GHz frames are heard on neighbouring channels too: the DS Parameter Set element has the real one */
	rxmhz = hd[0] > 196 ? (uint16_t)hd[0] : (uint16_t)(hd[0] == 14 ? 2484 : hd[0] <= 13 ? 2407 + 5 * hd[0] : 5000 + 5 * hd[0]);
	{
		const uint8_t *ds = swu_ie_find(fr + 36, frl - 36, 3);

		bssmhz = (ds && ds[1] == 1 && ds[2] >= 1 && ds[2] <= 13 && rxmhz < 2500) ? (uint16_t)(2407 + 5 * ds[2]) : rxmhz;
	}
	for (k = 0; k < wp.nbss; k++) {
		if (memcmp(wp.bss[k].bssid, fr + 16, 6) == 0) {
			wp.bss[k].seen = 1;
			if (rxmhz == bssmhz) {
				wp.bss[k].rssi = (int8_t)MAX(-128, MIN(127, (int)hd[1] - 95));
			}
			if (wp.bss[k].ssid[0] == 0) {
				const uint8_t *ssid_ie = swu_ie_find(fr + 36, frl - 36, 0);
				if (ssid_ie && ssid_ie[1] > 0) {
					unsigned n = MIN((unsigned)ssid_ie[1], 32u);
					for (unsigned i = 0; i < n; i++) {
						uint8_t ch = ssid_ie[2 + i];
						wp.bss[k].ssid[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
					}
					wp.bss[k].ssid[n] = 0;
				}
			}
			return;
		}
	}
	if (wp.nbss >= 48) {
		return;
	}
	k = wp.nbss++;
	wp.bss[k].seen = 1;
	wp.bss[k].missed = 0;
	memcpy(wp.bss[k].bssid, fr + 16, 6);
	wp.bss[k].mhz = bssmhz;
	wp.bss[k].rssi = (int8_t)MAX(-128, MIN(127, (int)hd[1] - 95));
	wp.bss[k].ssid[0] = 0;
	wp.bss[k].cap = (uint16_t)(fr[34] | (fr[35] << 8));
	wp.bss[k].bint = (uint16_t)(fr[32] | (fr[33] << 8));
	{
		uint32_t max_ie = (uint32_t)sizeof(wp.bss[k].ie);
		uint32_t valid_ie = 0;
		for (uint32_t o = 36; o + 2u <= frl; ) {
			uint8_t id = fr[o], l = fr[o + 1];
			if (o + 2u + l > frl) {
				break;
			}
			if ((o - 36u) + 2u + l <= max_ie) {
				valid_ie = (o - 36u) + 2u + l;
			}
			if (id == 0 && wp.bss[k].ssid[0] == 0) {
				unsigned n = MIN((unsigned)l, 32u);
				for (unsigned i = 0; i < n; i++) {
					uint8_t ch = fr[o + 2 + i];
					wp.bss[k].ssid[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
				}
				wp.bss[k].ssid[n] = 0;
			}
			o += 2u + l;
		}
		wp.bss[k].ie_len = (uint16_t)valid_ie;
		memcpy(wp.bss[k].ie, fr + 36, valid_ie);
	}
}

static uint32_t
wp_cmd(uint8_t *c, uint32_t id, uint16_t tag, const void *v, uint16_t len)
{
	memcpy(c, &id, 4);
	memcpy(c + 4, &len, 2);
	memcpy(c + 6, &tag, 2);
	memcpy(c + 8, v, len);
	return 8u + len;
}

static void
wp_tlv(uint8_t *c, uint32_t *n, uint16_t tag, const void *v, uint16_t len)
{
	memcpy(c + *n, &len, 2);
	memcpy(c + *n + 2, &tag, 2);
	if (len) {
		memcpy(c + *n + 4, v, len);
	}
	*n += 4u + len;
}

static void wp_poll(void);

/* send one WMI command once the single credit is back (HTC trailers return it) */
static bool
wp_wmi(const char *what, const uint8_t *c, uint32_t n)
{
	for (unsigned t = 0; t < 3000; t += 5) {
		wp_poll();
		if (wp.wmi_credits && wp_send(1, wp.wmi_eid, c, n)) {
			return true;
		}
		sleep_ms(5);
	}
	ev("WMI: %s: no credit", what);
	return false;
}

static bool
wp_htt_setup(void)
{
	uint8_t m[64];
	uint64_t bank = wp.pa + WP_HTT_FRAG, idxpa = wp.pa + WP_HTT_ALLOCIDX, ringpa = wp.pa + WP_HTT_RXRING;
	uint64_t *ring = (uint64_t *)(wp.pool + WP_HTT_RXRING);
	uint16_t maxid = WP_HTT_FRAGS - 1, r16[4] = { WP_HTT_RINGSZ, WP_HTT_BUFSZ, 0xffff, WP_HTT_RINGSZ - 1 };
	static const uint16_t offs[10] = { 74, 90, 27, 37, 4, 26, 7, 12, 1, 2 };
	uint8_t ag[4] = { 5, 64, 3, 0 };

	memset(m, 0, sizeof(m));                        /* FRAG_DESC_BANK_CFG */
	m[0] = 6;
	m[2] = 1;
	m[3] = 72;
	memcpy(m + 4, &bank, 8);
	memcpy(m + 36 + 2, &maxid, 2);
	m[60] = 1;
	if (!wp_send(4, wp.htt_eid, m, 64)) {
		return false;
	}
	for (uint32_t i = 0; i < WP_HTT_RINGSZ; i++) {  /* the RX ring: buffer i at pool + RXBUF + i * 2 KiB, 2047 posted */
		ring[i] = i < WP_HTT_RINGSZ - 1 ? wp.pa + WP_HTT_RXBUF + (uint64_t)i * WP_HTT_BUFSZ : 0;
	}
	*(volatile uint32_t *)(wp.pool + WP_HTT_ALLOCIDX) = WP_HTT_RINGSZ - 1;
	wp_sync(wp.pool + WP_HTT_FRAG, WP_HTT_FRAGS * 72);
	wp_sync(wp.pool + WP_HTT_RXRING, 0x5000);
	wp_sync(wp.pool + WP_HTT_RXBUF, WP_HTT_RINGSZ * WP_HTT_BUFSZ);
	memset(m, 0, sizeof(m));                        /* RX_RING_CFG */
	m[0] = 2;
	m[1] = 1;
	memcpy(m + 4, &idxpa, 8);
	memcpy(m + 12, &ringpa, 8);
	memcpy(m + 20, r16, 8);
	memcpy(m + 28, offs, 20);
	if (!wp_send(4, wp.htt_eid, m, 48) || !wp_send(4, wp.htt_eid, ag, 4)) {
		return false;
	}
	wp.rx_idx = WP_HTT_RINGSZ - 1;
	ev("WMI: HTT setup sent: frag bank %u x 72, RX ring %u", WP_HTT_FRAGS, WP_HTT_RINGSZ);
	return true;
}

static bool
wp_scan_setup(void)
{
	uint8_t c[2048], vm[4] = { 0, 0, 0, 0 };
	uint32_t n, num = WP_NCHAN;

	if (!wp_send(4, wp.htt_eid, vm, 4)) {
		ev("WMI: HTT version request not sent");
		return false;
	}
	for (unsigned t = 0; t < 3000 && !wp.htt_conf; t += 10) {
		wp_poll();
		sleep_ms(10);
	}
	if (!wp.htt_conf || !wp_htt_setup()) {
		ev("WMI: no HTT version / setup: stopping");
		return false;
	}
	for (unsigned k = 0; k < 2; k++) {              /* PDEV_SET_PARAM tx (1) / rx (2) chain mask = 3 */
		uint32_t p[3] = { 0, k ? 2u : 1u, 3 };

		n = wp_cmd(c, 0x4003, 82, p, 12);
		if (!wp_wmi("chain mask", c, n)) {
			return false;
		}
	}
	n = wp_cmd(c, 0x3003, 79, &num, 4);             /* SCAN_CHAN_LIST, every channel passive */
	{
		uint16_t al = (uint16_t)(WP_NCHAN * 28), tg = 18;

		memcpy(c + n, &al, 2);
		memcpy(c + n + 2, &tg, 2);
		n += 4;
	}
	for (unsigned k = 0; k < WP_NCHAN; k++) {
		uint32_t ch[6];
		bool g5 = wp_chans[k] > 5000, dfs = wp_chans[k] >= 5260 && wp_chans[k] <= 5720;

		memset(ch, 0, sizeof(ch));
		ch[0] = wp_chans[k];
		ch[1] = wp_chans[k];
		ch[3] = (g5 ? 0u : 1u) | (1u << 7) | (1u << 11) | (1u << 12) | (dfs ? (1u << 10) : 0);
		ch[4] = 0u | (40u << 8) | (40u << 16);
		ch[5] = 0u | (40u << 8);
		wp_tlv(c, &n, 80, ch, 24);
	}
	if (!wp_wmi("channel list", c, n)) {
		return false;
	}
	{
		uint32_t r[6] = { 0, 0x52, 0x52, 0x52, 0x30, 0x10 };      /* PDEV_SET_REGDOMAIN */

		n = wp_cmd(c, 0x4001, 81, r, 24);
		if (!wp_wmi("regdomain", c, n)) {
			return false;
		}
	}
	{
		uint32_t v[5] = { 0, 2, 0, 0, 0 };                        /* VDEV_CREATE: vdev 0, station */

		wp_mac_take();
		memcpy(&v[3], wp_mac, 6);
		n = wp_cmd(c, 0x5001, 86, v, 20);
		if (!wp_wmi("vdev create", c, n)) {
			return false;
		}
	}
	{
		uint32_t v[3] = { 0, 0x30, 1 };                           /* VDEV_SET_PARAM tx_encap native wifi */

		n = wp_cmd(c, 0x5008, 95, v, 12);
		if (!wp_wmi("vdev encap", c, n)) {
			return false;
		}
	}
	/*
	 * What ath10k configures at vdev create / start and we did not (the research agent's list; the firmware's STA power-save health
	 * monitor and keepalive timers are the best match for the crash 30-150 s after a join): firmware keepalive off, the PS parameters,
	 * and the pdev parameters of ath10k_mac_start.
	 */
	if (wp_svc_on(29)) {                                    /* WMI_SERVICE_STA_KEEP_ALIVE: STA_KEEPALIVE_CMD */
		uint32_t ka[4] = { 0, 1, 1, 0 };                  /* vdev 0, enabled, method NULL frame, interval 0 */
		uint32_t arp[4] = { 0, 0, 0, 0 };

		n = wp_cmd(c, 0x1c005, 127, ka, 16);
		wp_tlv(c, &n, 128, arp, 16);
		(void)wp_wmi("sta keepalive", c, n);
	}
	for (unsigned k = 0; k < 3; k++) {                      /* STA_POWERSAVE_PARAM: rx wake policy 0, tx wake threshold 1, pspoll count 0 */
		uint32_t pp[3] = { 0, k, k == 1 ? 1u : 0u };

		(void)wp_wmi("sta ps param", c, wp_cmd(c, 0x9002, 109, pp, 12));
	}
	{
		static const struct { uint32_t id, val; } pdev[] = {
			{ 0x22, 1 }, { 0x0a, 1 }, { 0x2c, 1 }, { 0x23, 0 }, { 0x25, 1 },     /* pmf qos, dynamic bw, idle ps, arp ac override, ani */
		};

		for (unsigned k = 0; k < sizeof(pdev) / sizeof(pdev[0]); k++) {
			uint32_t pp[3] = { 0, pdev[k].id, pdev[k].val };

			(void)wp_wmi("pdev param", c, wp_cmd(c, 0x4003, 82, pp, 12));
		}
		if (wp_svc_on(37)) {                            /* burst enable off */
			uint32_t pp[3] = { 0, 0x30, 0 };

			(void)wp_wmi("pdev burst", c, wp_cmd(c, 0x4003, 82, pp, 12));
		}
	}
	return true;
}

static void
wp_scan(void)
{
	uint8_t c[1024];
	uint32_t n;

	if (!wp.scan_setup) {
		if (!wp_scan_setup()) {
			return;
		}
		wp.scan_setup = true;
	}
	wp.scan_started = wp.scan_done = false;
	wp.foreign = 0;
	{
		uint32_t s[25];
		uint32_t f[WP_NCHAN];
		uint8_t bc[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0 };

		memset(s, 0, sizeof(s));
		s[0] = 0xa000;
		s[1] = 0xa001;
		s[3] = 1;
		s[4] = 0x11f;
		s[5] = 50;
		s[6] = 150;
		s[7] = 50;
		s[8] = 500;
		s[12] = 20000;
		s[13] = 5;
		s[14] = 0x31;                                             /* CHAN_STAT_EVENT | PASSIVE (inverted probe filter) */
		s[16] = WP_NCHAN;
		s[17] = 1;
		s[20] = 3;
		n = wp_cmd(c, 0x3001, 77, s, 100);
		for (unsigned k = 0; k < WP_NCHAN; k++) {
			f[k] = wp_chans[k];
		}
		wp_tlv(c, &n, 16, f, (uint16_t)sizeof(f));
		wp_tlv(c, &n, 19, NULL, 0);
		wp_tlv(c, &n, 19, bc, 8);
		wp_tlv(c, &n, 17, NULL, 0);
		if (!wp_wmi("start scan", c, n)) {
			return;
		}
	}
	ev("WMI: scan started (passive)");
	for (unsigned t = 0; t < 25000 && !wp.scan_done; t += 10) {
		wp_poll();
		sleep_ms(10);
	}
	for (unsigned k = 0; k < wp.nbss; k++) {
		ev("BSS %02x%02x%02x%02x%02x%02x %u %d \"%s\"", wp.bss[k].bssid[0], wp.bss[k].bssid[1], wp.bss[k].bssid[2], wp.bss[k].bssid[3],
		    wp.bss[k].bssid[4], wp.bss[k].bssid[5], wp.bss[k].mhz, wp.bss[k].rssi, wp.bss[k].ssid);
	}
	ev("WMI: scan %s: %u BSS", wp.scan_done ? "done" : "TIMEOUT", wp.nbss);
}

/* ---- joining a WPA2-PSK network (the fork's 1126/1127 order, with the handshake in sweet_wlan_util.c) ----
 * Credentials: /fw/wlan/net.conf = SSID line, then the 32-byte PMK as 64 hex digits (PBKDF2 done on the PC).
 */
#define JN_MGMT_OFF     0x430000u       /* 8 management frame buffers of 1 KiB */
#define JN_TXBUF_OFF    0x438000u       /* per msdu id: HTC header + HTT TX_FRM (64 bytes) */
#define JN_TXFR_OFF     0x440000u       /* per msdu id: the native-wifi frame (2 KiB) */
#define JN_NTX          256u
#define JN_RXDESC       360u            /* sizeof(struct htt_rx_desc_v2): payload offset in an RX buffer */

static const struct swu_wpa_ops jn_ops;
static void jn_dataframe(const uint8_t *eth, size_t n);

/*
 * A join asked for in Settings (Apple80211 ASSOCIATE): the ioctl fills g_ujoin_new and sets g_ujoin_pending; the station loop moves it to
 * g_ujoin (the network jn_read_conf uses instead of /fw/wlan/net.conf, until a join of it fails) and rejoins. g_join_result is what
 * ASSOCIATE_RESULT (21) answers: 0 while it runs, then Apple80211's number: 1 success, 21 wrong key, 2 anything else.
 */
struct ujoin {
	uint8_t ssid[32];
	uint8_t ssid_len;
	uint8_t pmk[32];
};
static struct ujoin g_ujoin_new, g_ujoin;
static volatile uint32_t g_ujoin_pending, g_ujoin_valid;
static volatile uint32_t g_join_result;

/* PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32) */
static void
ujoin_pmk(const uint8_t *pass, size_t plen, const uint8_t *ssid, size_t slen, uint8_t pmk[32])
{
	for (uint32_t blk = 1; blk <= 2; blk++) {
		uint8_t cnt[4] = { 0, 0, 0, (uint8_t)blk }, u[20], r[20];

		swu_hmac_sha1(pass, plen, ssid, slen, cnt, 4, u);
		memcpy(r, u, 20);
		for (unsigned i = 1; i < 4096; i++) {
			swu_hmac_sha1(pass, plen, u, 20, NULL, 0, u);
			for (unsigned k = 0; k < 20; k++) {
				r[k] ^= u[k];
			}
		}
		memcpy(pmk + (blk - 1) * 20, r, blk == 1 ? 20 : 12);
	}
}

static bool
jn_read_conf(void)
{
	char buf[160];
	off_t size = 0;
	int err;
	unsigned i, l = 0;

	if (g_ujoin_valid) {
		memcpy(jn.ssid, g_ujoin.ssid, g_ujoin.ssid_len);
		jn.ssid_len = g_ujoin.ssid_len;
		memcpy(jn.pmk, g_ujoin.pmk, 32);
		ev("JOIN: network \"%.*s\" asked for in Settings, PMK derived", jn.ssid_len, jn.ssid);
		return true;
	}
	err = tftp_file("/hakios/fw/wlan/net.conf", 0, NULL, 0, &size);

	if (err != 0 || size < 70 || size >= (off_t)sizeof(buf)) {
		ev("JOIN: no usable /fw/wlan/net.conf (error %d, %lld bytes)", err, (long long)size);
		return false;
	}
	if (tftp_file("/hakios/fw/wlan/net.conf", 0, buf, (int)size, NULL) != 0) {
		return false;
	}
	buf[size] = 0;
	while (l < 32 && buf[l] != '\n' && buf[l] != 0) {
		jn.ssid[l] = (uint8_t)buf[l];
		l++;
	}
	jn.ssid_len = (uint8_t)l;
	if (buf[l] != '\n' || l == 0) {
		return false;
	}
	for (i = 0; i < 32; i++) {
		unsigned v = 0;

		for (unsigned k = 0; k < 2; k++) {
			char c = buf[l + 1 + 2 * i + k];

			v = (v << 4) | (unsigned)(c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0));
		}
		jn.pmk[i] = (uint8_t)v;
	}
	ev("JOIN: network \"%.*s\", PMK loaded", jn.ssid_len, jn.ssid);
	return true;
}

/* the best scanned BSS with our SSID and a WPA2-PSK/CCMP element */
static bool
jn_pick(void)
{
	int best = -1, best_rssi = -1000;

	for (unsigned k = 0; k < wp.nbss; k++) {
		const uint8_t *rsn;

		if (strlen(wp.bss[k].ssid) != jn.ssid_len || memcmp(wp.bss[k].ssid, jn.ssid, jn.ssid_len) != 0) {
			continue;
		}
		rsn = swu_ie_find(wp.bss[k].ie, wp.bss[k].ie_len, 48);
		if (!rsn || rsn[1] < 8) {
			continue;
		}
		if (wp.bss[k].rssi > best_rssi) {
			best = (int)k;
			best_rssi = wp.bss[k].rssi;
		}
	}
	if (best < 0) {
		ev("JOIN: no BSS with that SSID and an RSN element in the scan");
		return false;
	}
	jn.bss = best;
	memcpy(jn.bssid, wp.bss[best].bssid, 6);
	jn.mhz = wp.bss[best].mhz;
	{
		const uint8_t *rsn = swu_ie_find(wp.bss[best].ie, wp.bss[best].ie_len, 48);
		const uint8_t *tim = swu_ie_find(wp.bss[best].ie, wp.bss[best].ie_len, 5);
		static const uint8_t rate_val[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
		static const uint8_t rate_hw[12] = { 0x43, 0x42, 0x41, 0x40, 0x03, 0x07, 0x02, 0x06, 0x01, 0x05, 0x00, 0x04 };
		int lo = -1;

		jn.group_type = rsn[7];                 /* version(2), group OUI(3), group type */
		jn.dtim = (tim && tim[1] >= 2 && tim[3]) ? tim[3] : 1;
		for (uint8_t id = 1; id != 0; id = (id == 1) ? 50 : 0) {
			const uint8_t *e = swu_ie_find(wp.bss[best].ie, wp.bss[best].ie_len, id);

			for (unsigned k = 0; e && k < e[1]; k++) {
				for (int r = 0; r < 12; r++) {
					if ((e[2 + k] & 0x80) && (e[2 + k] & 0x7f) == rate_val[r] && (lo < 0 || rate_val[r] < rate_val[lo])) {
						lo = r;
					}
				}
			}
		}
		jn.mgmt_rate = lo >= 0 ? rate_hw[lo] : (jn.mhz > 4900 ? 0x03 : 0x43);
		ev("JOIN: BSS %02x%02x%02x%02x%02x%02x %u MHz %d dBm, beacon %u, dtim %u, group cipher type %u, mgmt rate %#x",
		    jn.bssid[0], jn.bssid[1], jn.bssid[2], jn.bssid[3], jn.bssid[4], jn.bssid[5], jn.mhz, wp.bss[best].rssi,
		    wp.bss[best].bint, jn.dtim, jn.group_type, jn.mgmt_rate);
		if (jn.group_type != 4) {
			ev("JOIN: the AP's group cipher is not CCMP: not supported yet");
			return false;
		}
	}
	return true;
}

/* ---- HTT TX_FRM for data and (without WMI mgmt tx) management frames ---- */
static bool
jn_ce4_send2(uint64_t pa0, uint16_t len0, uint64_t pa1, uint16_t len1)
{
	const unsigned r = 4;
	unsigned mask = wp_cfg[r].n - 1, wr = wp.wr[r];
	uint32_t srri = (g_wlan_halt || g_wlan_crashed || g_modem_crashed || wp.regs == NULL) ? 0 : (wp_ce(4)[0x44 / 4] & mask);
	struct wp_desc *d;

	if (g_wlan_halt || g_wlan_crashed || g_modem_crashed || wp.regs == NULL) {
		return false;
	}

	if (((srri - 1 - wr) & mask) < 2) {
		return false;
	}
	d = wp_desc(r, wr);
	d->addr = (pa0 & 0x1fffffffffull) | (0x10000ull << 32) | (1ull << 63);     /* gather */
	d->nbytes = len0;
	d->flags = 1;
	d->hash = 0;
	wp_sync(d, sizeof(*d));
	wr = (wr + 1) & mask;
	d = wp_desc(r, wr);
	d->addr = pa1 & 0x1fffffffffull;
	d->nbytes = len1;
	d->flags = 0;
	d->hash = 0;
	wp_sync(d, sizeof(*d));
	wr = (wr + 1) & mask;
	wp.wr[r] = wr;
	wp.regs[0x32010 / 4] = wr;
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}

static int
jn_txid_alloc(void)
{
	for (unsigned k = 0; k < JN_NTX; k++) {
		if (jn.txfree[k]) {
			jn.txfree[k] = 0;
			jn.tx_inflight++;
			return (int)k;
		}
	}
	return -1;
}

static void
jn_txid_free(unsigned id)
{
	if (id < JN_NTX && !jn.txfree[id]) {
		jn.txfree[id] = 1;
		jn.tx_inflight--;
	}
}

static bool
jn_htt_tx(unsigned id, uint16_t len, bool mgmt, bool no_encrypt, unsigned tid)
{
	uint64_t frame_pa = wp.pa + JN_TXFR_OFF + (uint64_t)id * 2048;
	uint64_t txbuf_pa = wp.pa + JN_TXBUF_OFF + (uint64_t)id * 64;
	uint8_t *tb = wp.pool + JN_TXBUF_OFF + (uint64_t)id * 64;
	uint64_t frags = frame_pa;
	uint16_t prefetch = (uint16_t)((MIN(len, (uint16_t)50) + 3) & ~3u), hl = (uint16_t)(20 + prefetch), f1, id16 = (uint16_t)id, peer = 0xffff;

	if (!mgmt) {
		uint8_t *ext = wp.pool + WP_HTT_FRAG + (uint64_t)id * 72;
		uint32_t lo = (uint32_t)frame_pa;
		uint16_t hi = (uint16_t)(frame_pa >> 32);

		memset(ext, 0, 72);
		memcpy(ext + 24, &lo, 4);
		memcpy(ext + 28, &hi, 2);
		memcpy(ext + 30, &len, 2);
		wp_sync(ext, 72);
		frags = wp.pa + WP_HTT_FRAG + (uint64_t)id * 72;
	}
	wp_hist(0x40000000u | ((mgmt ? 1u : 0u) << 16) | len);
	memset(tb, 0, 28);
	tb[0] = wp.htt_eid;
	memcpy(tb + 2, &hl, 2);
	tb[8] = 1;                                              /* HTT_H2T_MSG_TYPE_TX_FRM */
	tb[9] = (uint8_t)(1u | (no_encrypt ? 4u : 0u) | ((mgmt ? 3u : 1u) << 5));
	f1 = (uint16_t)(((tid & 0x1f) << 6) | 0x800u);
	memcpy(tb + 10, &f1, 2);
	memcpy(tb + 12, &len, 2);
	memcpy(tb + 14, &id16, 2);
	memcpy(tb + 16, &frags, 8);
	memcpy(tb + 24, &peer, 2);
	wp_sync(tb, 64);
	wp_sync(wp.pool + JN_TXFR_OFF + (uint64_t)id * 2048, len);
	return jn_ce4_send2(txbuf_pa, 28, frame_pa, prefetch);
}

/* one Ethernet II frame out as native wifi; protected once the pairwise key is in */
static int
jn_tx_eth(const uint8_t *eth, size_t len, bool eapol)
{
	int id = jn_txid_alloc();
	uint8_t *fr;
	size_t n;
	uint16_t sc;

	if (id < 0) {
		return 28;
	}
	fr = wp.pool + JN_TXFR_OFF + (uint64_t)id * 2048;
	n = swu_eth_to_nwifi(eth, len, jn.bssid, fr, 2048);
	{
		/* what kind of frame this is, for the history ring: class (1 arp, 2 udp, 3 tcp, 4 icmp, 5 ipv6, 6 other), multicast bit, length */
		uint16_t et = (uint16_t)((eth[12] << 8) | eth[13]);
		uint32_t cls = et == 0x0806 ? 1u : et == 0x86dd ? 5u : (et == 0x0800 && len >= 24) ? (eth[23] == 17 ? 2u : eth[23] == 6 ? 3u : eth[23] == 1 ? 4u : 6u) : 6u;

		wp_hist(0x50000000u | (cls << 20) | ((eth[0] & 1u) << 16) | (uint32_t)MIN(len, (size_t)0xffff));
	}
	if (n == 0) {
		jn_txid_free((unsigned)id);
		return 22;
	}
	if (jn.ptk_installed) {
		fr[1] |= 0x40;                                  /* Protected: the firmware encrypts */
	}
	sc = (uint16_t)(jn.seq++ << 4);
	memcpy(fr + 22, &sc, 2);
	if (!jn_htt_tx((unsigned)id, (uint16_t)n, false, !jn.ptk_installed, 16)) {
		jn_txid_free((unsigned)id);
		return 28;
	}
	if (eapol) {
		jn.eapol_msdu = id;
		jn.tx_eapol++;
	}
	jn.tx_frames++;
	return 0;
}

/* ---- management frames ---- */
static bool
jn_mgmt_tx(const uint8_t *f, size_t len, const char *what)
{
	uint16_t sc = (uint16_t)(jn.seq++ << 4);

	if (len > 1024) {
		return false;
	}
	jn.mgmt_busy = true;
	jn.mgmt_status = -1;
	if (jn.svc_mgmt_wmi) {
		unsigned slot = jn.mgmt_slot++ % 8u;
		uint8_t *buf = wp.pool + JN_MGMT_OFF + slot * 1024u;
		uint64_t pa = wp.pa + JN_MGMT_OFF + slot * 1024u;
		uint32_t blen = (uint32_t)((MIN(len, (size_t)64) + 3) & ~3u);
		uint32_t m[7] = { 0, slot, 0, (uint32_t)pa, (uint32_t)(pa >> 32), (uint32_t)len, blen };
		uint8_t c[8 + 28 + 4 + 64];
		uint32_t n;

		memcpy(buf, f, len);
		memcpy(buf + 22, &sc, 2);
		wp_sync(buf, len);
		n = wp_cmd(c, 0x7008, 422, m, 28);
		wp_tlv(c, &n, 17, buf, (uint16_t)blen);
		if (!wp_wmi(what, c, n)) {
			return false;
		}
	} else {
		int id = jn_txid_alloc();
		uint8_t *fr;

		if (id < 0) {
			return false;
		}
		fr = wp.pool + JN_TXFR_OFF + (uint64_t)id * 2048;
		memcpy(fr, f, len);
		memcpy(fr + 22, &sc, 2);
		if (!jn_htt_tx((unsigned)id, (uint16_t)len, true, true, 17)) {
			jn_txid_free((unsigned)id);
			return false;
		}
	}
	ev("JOIN: %s sent (%zu bytes, %s)", what, len, jn.svc_mgmt_wmi ? "WMI by reference" : "HTT");
	return true;
}

/* ---- the handshake glue ---- */
static void
jn_wpa_tx(void *arg, const uint8_t *frame, size_t len)
{
	uint8_t eth[14 + 256];

	(void)arg;
	if (len > 256) {
		return;
	}
	memcpy(eth, jn.bssid, 6);
	memcpy(eth + 6, wp_mac, 6);
	eth[12] = 0x88;
	eth[13] = 0x8e;
	memcpy(eth + 14, frame, len);
	(void)jn_tx_eth(eth, 14 + len, true);
}

static int
jn_wpa_key(void *arg, int pairwise, unsigned idx, const uint8_t *key, size_t len)
{
	(void)arg;
	if (len != 16) {
		return 95;
	}
	if (pairwise) {
		memcpy(jn.ptk, key, 16);
		jn.key_pending[0] = true;
	} else {
		memcpy(jn.gtk, key, 16);
		jn.gtk_idx = idx;
		jn.key_pending[1] = true;
	}
	return 0;
}

static void
jn_wpa_done(void *arg, int status)
{
	(void)arg;
	ev("JOIN: 4-way handshake %s", status ? "FAILED" : "done");
	jn.hs_done = status == 0;
}

static void
jn_random(uint8_t *buf, size_t len)
{
	read_random(buf, (u_int)len);
}

static const struct swu_wpa_ops jn_ops = { jn_wpa_tx, jn_wpa_key, jn_wpa_done, jn_random };

/* ---- WMI commands of the station ---- */
static bool
jn_vdev_param(uint32_t param, uint32_t value, const char *what)
{
	uint8_t c[32];
	uint32_t v[3] = { 0, param, value };

	return wp_wmi(what, c, wp_cmd(c, 0x5008, 95, v, 12)) ;
}

static bool
jn_peer_param(uint32_t param, uint32_t value, const char *what)
{
	uint8_t c[32];
	uint32_t v[5] = { 0, 0, 0, param, value };

	memcpy(&v[1], jn.bssid, 6);
	return wp_wmi(what, c, wp_cmd(c, 0x6004, 100, v, 20));
}

static bool
jn_vdev_wmm_params(void)
{
	uint8_t c[140];
	struct {
		uint32_t vdev_id;
		struct {
			uint32_t dummy;
			uint32_t cwmin;
			uint32_t cwmax;
			uint32_t aifs;
			uint32_t txop;
			uint32_t acm;
			uint32_t no_ack;
		} ac[4];
	} cmd;
	uint32_t n;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = 0;
	/* Standard 802.11 STA EDCA defaults */
	cmd.ac[0].cwmin = 15;
	cmd.ac[0].cwmax = 1023;
	cmd.ac[0].aifs = 3;
	cmd.ac[0].txop = 0;

	cmd.ac[1].cwmin = 15;
	cmd.ac[1].cwmax = 1023;
	cmd.ac[1].aifs = 7;
	cmd.ac[1].txop = 0;

	cmd.ac[2].cwmin = 7;
	cmd.ac[2].cwmax = 15;
	cmd.ac[2].aifs = 2;
	cmd.ac[2].txop = 3008;

	cmd.ac[3].cwmin = 3;
	cmd.ac[3].cwmax = 7;
	cmd.ac[3].aifs = 2;
	cmd.ac[3].txop = 1504;

	if (jn.bss >= 0 && (unsigned)jn.bss < wp.nbss) {
		const uint8_t *ies = wp.bss[jn.bss].ie;
		size_t ies_len = wp.bss[jn.bss].ie_len;
		for (size_t o = 0; o + 2 <= ies_len;) {
			uint8_t id = ies[o], l = ies[o + 1];
			if (o + 2 + l > ies_len) {
				break;
			}
			if (id == 221 && l >= 24 &&
			    ies[o + 2] == 0x00 && ies[o + 3] == 0x50 && ies[o + 4] == 0xf2 &&
			    ies[o + 5] == 2 && ies[o + 6] == 1) {
				const uint8_t *rec = ies + o + 8;
				for (int i = 0; i < 4; i++, rec += 4) {
					int aci = (rec[0] >> 5) & 3;
					int acm = (rec[0] >> 4) & 1;
					int aifsn = rec[0] & 0xf;
					int ecwmin = rec[1] & 0xf;
					int ecwmax = (rec[1] >> 4) & 0xf;
					uint16_t txop = (uint16_t)(rec[2] | (rec[3] << 8));
					if (aci >= 0 && aci < 4) {
						cmd.ac[aci].aifs = aifsn < 2 ? 2 : aifsn;
						cmd.ac[aci].cwmin = (1u << ecwmin) - 1;
						cmd.ac[aci].cwmax = (1u << ecwmax) - 1;
						cmd.ac[aci].txop = (uint32_t)txop * 32;
						cmd.ac[aci].acm = acm;
					}
				}
				ev("JOIN: WMM params from AP applied");
				break;
			}
			o += 2u + l;
		}
	}

	n = wp_cmd(c, 0x500d, 199, &cmd, sizeof(cmd));
	return wp_wmi("vdev set wmm", c, n);
}

static bool
jn_vdev_start(void)
{
	uint8_t c[128];
	uint32_t v[18], n;
	bool g5 = jn.mhz > 4900, dfs = jn.mhz >= 5260 && jn.mhz <= 5720;
	uint32_t mode = g5 ? 0u : 1u;                           /* 11A / 11G */
	uint32_t ch[6];

	if (!swu_no_ht && swu_ie_find(wp.bss[jn.bss].ie, wp.bss[jn.bss].ie_len, 45) != NULL) {
		mode = g5 ? 4u : 5u;                            /* MODE_11NA_HT20 / MODE_11NG_HT20: the peer is associated as an HT peer */
	}
	ch[0] = jn.mhz; ch[1] = jn.mhz; ch[2] = 0; ch[3] = mode | (dfs ? (1u << 10) : 0);
	ch[4] = 0u | (40u << 8) | (40u << 16); ch[5] = 0u | (40u << 8);
	memset(v, 0, sizeof(v));
	n = wp_cmd(c, 0x5003, 88, v, 72);
	wp_tlv(c, &n, 80, ch, 24);
	wp_tlv(c, &n, 18, NULL, 0);
	jn.start_resp = false;
	jn.vdev_started = true;
	return wp_wmi("vdev start", c, n);
}

static bool
jn_peer_assoc(void)
{
	static const uint8_t ours[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
	static const uint8_t ht_rates[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	uint8_t legacy[16], c[8 + 76 + 4 + 16 + 4 + 16 + 4 + 16];
	unsigned nl = 0, n_ht = 0;
	bool g5 = jn.mhz > 4900;
	uint32_t a[19], n;
	uint8_t pad[16], pad_ht[16];
	uint32_t vht[4] = { 0, 0, 0, 0 };

	for (unsigned r = 0; r < 12; r++) {
		if (g5 && r < 4) {
			continue;
		}
		for (unsigned k = 0; k < jn.ar.nrates; k++) {
			if ((jn.ar.rates[k] & 0x7f) == ours[r]) {
				legacy[nl++] = (uint8_t)(ours[r] | (ours[r] == 11 ? 0x80 : 0));      /* ath10k_mac_bitrate_to_rate: bit 7 only on 5.5 Mbit/s, it is not the 802.11 basic-rate flag */
				break;
			}
		}
	}
	memset(a, 0, sizeof(a));
	memcpy(&a[0], jn.bssid, 6);
	a[2] = 0;                                               /* vdev */
	a[3] = 1;                                               /* new_assoc */
	a[4] = jn.aid;
	a[5] = 0x1 | (jn.ar.wmm ? 0x2 : 0) | 0x4;               /* auth, qos, need the 4-way */
	if (jn.ar.ht) {
		a[5] |= 0x1000;                                 /* WMI_PEER_HT */
		a[8] = jn.ar.ht_caps ? jn.ar.ht_caps : 0x012d;  /* ht_caps */
		a[9] = 65535;                                   /* max_mpdu (64k) */
		a[10] = 0;                                      /* mpdu_density */
		a[11] |= 0x08;                                  /* WMI_RC_HT_FLAG */
		n_ht = 8;
	}
	a[6] = jn.ar.cap;
	a[7] = 1;
	a[12] = 1;                                              /* nss */
	if (jn.ar.ht) {
		a[14] = g5 ? 4 : 5;                             /* MODE_11NA_HT20 (4) / MODE_11NG_HT20 (5) */
	} else {
		{
			unsigned r0 = nl ? (legacy[0] & 0x7fu) : 0;
			bool first_cck = r0 == 2 || r0 == 4 || r0 == 11 || r0 == 22;

			a[14] = g5 ? 0 : ((nl > 4 || (nl && !first_cck)) ? 1 : 2);   /* 11A / 11G / 11B */
		}
	}
	a[17] = nl;
	a[18] = n_ht;
	n = wp_cmd(c, 0x6005, 101, a, 76);
	memset(pad, 0, sizeof(pad));
	memcpy(pad, legacy, nl);
	wp_tlv(c, &n, 17, pad, (uint16_t)((nl + 3) & ~3u));
	memset(pad_ht, 0, sizeof(pad_ht));
	if (n_ht) {
		memcpy(pad_ht, ht_rates, n_ht);
		wp_tlv(c, &n, 17, pad_ht, (uint16_t)((n_ht + 3) & ~3u));
	} else {
		wp_tlv(c, &n, 17, NULL, 0);
	}
	wp_tlv(c, &n, 102, vht, 16);
	ev("JOIN: peer assoc aid %u flags %#x caps %#x phy mode %u, %u legacy, %u HT rates", jn.aid, a[5], a[6], a[14], nl, n_ht);
	return wp_wmi("peer assoc", c, n);
}

static bool
jn_install_key(bool pairwise)
{
	uint8_t c[8 + 92 + 4 + 16];
	uint32_t k[23], n;

	memset(k, 0, sizeof(k));
	memcpy(&k[1], jn.bssid, 6);
	k[3] = pairwise ? 0 : jn.gtk_idx;
	k[4] = pairwise ? 0 : 1;
	k[5] = 4;                                               /* AES CCM */
	k[20] = 16;
	n = wp_cmd(c, 0x5009, 96, k, 92);
	wp_tlv(c, &n, 17, pairwise ? jn.ptk : jn.gtk, 16);
	jn.sec_ind = false;
	ev("JOIN: installing the %s key", pairwise ? "pairwise" : "group");
	return wp_wmi("install key", c, n);
}

/* ---- incoming: management frames, WMI events, HTT messages ---- */
static bool
jn_mgmt(const uint8_t *fr, uint32_t frl)
{
	uint16_t st = 0;

	if (swu_auth_resp(fr, frl, jn.bssid, wp_mac, &st) == 0) {
		jn.auth_resp = true;
		jn.auth_status = st;
		ev("JOIN: authentication response status %u", st);
		return true;
	}
	if (swu_assoc_resp(fr, frl, jn.bssid, wp_mac, &jn.ar) == 0) {
		jn.assoc_resp = true;
		ev("JOIN: association response status %u aid %u cap %#x, %u rates, WMM %u, HT %u (caps %#x)", jn.ar.status, jn.ar.aid, jn.ar.cap, jn.ar.nrates, jn.ar.wmm, jn.ar.ht, jn.ar.ht_caps);
		return true;
	}
	if (swu_kicked(fr, frl, jn.bssid, wp_mac, &st) == 0) {
		jn.kicked = true;
		ev("JOIN: %s from the AP, reason %u", fr[0] == 0xc0 ? "deauthentication" : "disassociation", st);
		return true;
	}
	return false;
}

static bool
jn_wmi_event(uint32_t evid, const uint8_t *t, uint32_t len)
{
	uint32_t v[5];

	memset(v, 0, sizeof(v));
	if (len >= 4) {
		memcpy(v, t + 4, MIN(len - 4, (uint32_t)sizeof(v)));
	}
	switch (evid) {
	case 0x5001:                                            /* VDEV_START_RESP {vdev, req, type, status} */
		jn.start_status = v[3];
		jn.start_resp = true;
		ev("JOIN: VDEV_START_RESP status %u", v[3]);
		return true;
	case 0x5002:
		jn.stopped = true;
		return true;
	case 0x7006:                                            /* MGMT_TX_COMPLETION {desc, status} */
		jn.mgmt_status = (int)v[1];
		jn.mgmt_busy = false;
		ev("JOIN: management TX %s (status %u)", v[1] ? "NOT acked" : "acked", v[1]);
		return true;
	case 0x7007: {                                          /* MGMT_TX_BUNDLE_COMPLETION */
		uint32_t o = 8, nrep = v[0];
		uint16_t l;

		if (o + 4 <= len) {
			memcpy(&l, t + o, 2);
			o += 4u + l;
			if (nrep && o + 8 <= len) {
				uint32_t st;

				memcpy(&st, t + o + 4, 4);
				jn.mgmt_status = (int)st;
			}
		}
		jn.mgmt_busy = false;
		ev("JOIN: management TX (bundle of %u) status %d", nrep, jn.mgmt_status);
		return true;
	}
	case 0x6001:
		jn.kicked = true;
		ev("JOIN: firmware kickout of the AP");
		return true;
	case 0xb001:                                            /* ROAM_EVENTID */
		ev("JOIN: ROAM event reason %u rssi %d", v[1], (int)v[2]);
		return true;
	case 0x5003:                                            /* INSTALL_KEY_COMPLETE */
	case 0x6006:                                            /* PEER_ASSOC_CONF */
	case 0x4005:                                            /* TX_PAUSE */
		return true;
	default:
		return false;
	}
}

static uint32_t g_rx_msgs, g_rx_ent, g_rx_reposts, g_rx_bad;     /* in-order indications, buffers in them, buffers reposted, indications dropped as malformed */

static void
jn_rx_repost(uint64_t pa)
{
	uint64_t *ring = (uint64_t *)(wp.pool + WP_HTT_RXRING);

	g_rx_reposts++;
	*(volatile uint32_t *)(wp.pool + (pa - wp.pa) + 4) = 0;                 /* rx_attention */
	wp_sync(wp.pool + (pa - wp.pa), 64);
	ring[wp.rx_idx] = pa;
	wp_sync(&ring[wp.rx_idx], 8);
	wp.rx_idx = (wp.rx_idx + 1) & (WP_HTT_RINGSZ - 1);
}

static void
jn_rx_frame(const uint8_t *buf, uint16_t mlen)
{
	uint8_t eth[1600];
	uint32_t att;
	size_t n;

	memcpy(&att, buf + 4, 4);
	if (!(att & (1u << 31)) || (att & ((1u << 30) | (1u << 29) | (1u << 3) | (1u << 27) | (1u << 17) | (1u << 16))) ||
	    mlen > WP_HTT_BUFSZ - JN_RXDESC) {
		jn.rx_drop++;
		wp_hist(0x70000000u | ((att >> 24) << 16) | (att & 0xffffu));      /* dropped: the rx_attention bits */
		return;
	}
	n = swu_nwifi_to_eth(buf + JN_RXDESC, mlen, eth, sizeof(eth));
	if (n < 14 || memcmp(eth + 6, wp_mac, 6) == 0) {
		jn.rx_drop++;
		return;
	}
	if (eth[12] == 0x88 && eth[13] == 0x8e) {
		jn.rx_eapol++;
		int e = swu_wpa_rx(&jn.wpa, eth + 14, n - 14);

		ev("JOIN: EAPOL %zu bytes -> %d (supplicant state %d)", n - 14, e, jn.wpa.state);
		return;
	}
	{
		uint16_t et = (uint16_t)((eth[12] << 8) | eth[13]);
		uint32_t cls = et == 0x0806 ? 1u : et == 0x86dd ? 5u : (et == 0x0800 && n >= 24) ? (eth[23] == 17 ? 2u : eth[23] == 6 ? 3u : eth[23] == 1 ? 4u : 6u) : 6u;

		wp_hist(0x60000000u | (cls << 20) | ((eth[0] & 1u) << 16) | (uint32_t)MIN(n, (size_t)0xffff));
	}
	jn.rx_frames++;
	if (jn.rx_logged < 3) {
		jn.rx_logged++;
		ev("JOIN: rx %zu bytes type %02x%02x from %02x%02x%02x%02x%02x%02x", n, eth[12], eth[13], eth[6], eth[7], eth[8], eth[9], eth[10], eth[11]);
	}
	jn_dataframe(eth, n);
}

static bool
jn_htt(const uint8_t *pl, uint32_t plen)
{
	if (plen < 4) {
		return false;
	}
	switch (pl[0]) {
	case 0x03:                                              /* PEER_MAP */
		if (plen >= 10 && memcmp(pl + 4, jn.bssid, 6) == 0) {
			memcpy(&jn.peer_id, pl + 2, 2);
			jn.peer_mapped = true;
			ev("JOIN: peer mapped id %u", jn.peer_id);
		}
		return true;
	case 0x04:                                              /* PEER_UNMAP */
		jn.peer_mapped = false;
		return true;
	case 0x0b:                                              /* SEC_IND */
		jn.sec_ind = true;
		ev("JOIN: SEC_IND type %u %s", pl[1] & 0x7f, (pl[1] & 0x80) ? "unicast" : "group");
		return true;
	case 0x07: {                                            /* TX_COMPL_IND: status, count, flags2, msdu ids */
		unsigned st = pl[1] & 7, num = pl[2];

		for (unsigned k = 0; k < num && 4 + 2 * k + 2 <= plen; k++) {
			uint16_t id;

			memcpy(&id, pl + 4 + 2 * k, 2);
			if (id == 0xffff) {
				continue;
			}
			if (st != 0) {
				jn.tx_fail++;
			}
			if ((int)id == jn.eapol_msdu) {
				jn.eapol_msdu = -1;
			}
			if (jn.mgmt_busy && !jn.svc_mgmt_wmi) {
				jn.mgmt_busy = false;
				jn.mgmt_status = (int)st;
			}
			jn_txid_free(id);
		}
		return true;
	}
	case 0x0e:                                              /* MGMT_TX_COMPL_IND (HTT path) */
		if (plen >= 12) {
			uint32_t id, st;

			memcpy(&id, pl + 4, 4);
			memcpy(&st, pl + 8, 4);
			jn.mgmt_busy = false;
			jn.mgmt_status = (int)st;
			jn_txid_free(id);
			ev("JOIN: management TX (HTT) status %u", st);
		}
		return true;
	case 0x12: {                                            /* RX_IN_ORD_PADDR_IND: info, peer, vdev, rsvd, count, 12-byte descs */
		uint16_t cnt;

		memcpy(&cnt, pl + 6, 2);
		g_rx_msgs++;
		g_rx_ent += cnt;
		if (8u + 12u * cnt > plen) {
			g_rx_bad++;
			return true;
		}
		for (unsigned k = 0; k < cnt; k++) {
			uint64_t pa;
			uint16_t ml;
			const uint8_t *buf;

			memcpy(&pa, pl + 8 + 12 * k, 8);
			memcpy(&ml, pl + 8 + 12 * k + 8, 2);
			pa &= 0x1fffffffffull;
			if (pa < wp.pa + WP_HTT_RXBUF || pa >= wp.pa + WP_POOL || ((pa - wp.pa - WP_HTT_RXBUF) % WP_HTT_BUFSZ) != 0) {
				ev("JOIN: in-order indication with an unknown buffer %#llx", pa);
				g_rx_bad++;
				return true;
			}
			buf = wp.pool + (pa - wp.pa);
			wp_sync(buf, MIN((uint32_t)ml + JN_RXDESC, WP_HTT_BUFSZ));
			if (!(pl[1] & 0x20)) {
				jn_rx_frame(buf, ml);
			}
			jn_rx_repost(pa);
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		*(volatile uint32_t *)(wp.pool + WP_HTT_ALLOCIDX) = wp.rx_idx;
		wp_sync(wp.pool + WP_HTT_ALLOCIDX, 64);
		return true;
	}
	default:
		return false;
	}
}

static bool
jn_wait(bool *flag, unsigned ms, const char *what)
{
	for (unsigned t = 0; t < ms; t += 5) {
		wp_poll();
		if (*flag) {
			return true;
		}
		if (jn.kicked) {
			ev("JOIN: kicked while waiting for %s", what);
			return false;
		}
		sleep_ms(5);
	}
	ev("JOIN: timeout waiting for %s", what);
	return false;
}

static bool
jn_join(void)
{
	uint8_t f[512], rsn[32], c[32];
	size_t n;
	uint32_t v[4];

	if (!jn_read_conf() || !jn_pick()) {
		return false;
	}
	jn.active = true;
	memset(jn.txfree, 1, sizeof(jn.txfree));
	jn.svc_mgmt_wmi = wp_svc_on(76);
	ev("JOIN: management TX via %s, rx reorder offload %u", jn.svc_mgmt_wmi ? "WMI" : "HTT", wp_svc_on(65));
	jn.eapol_msdu = -1;
	/* VDEV_START on the AP's channel; the vdev created for the scan is reused */
	if (!jn_vdev_start() || !jn_wait(&jn.start_resp, 3000, "VDEV_START_RESP") || jn.start_status) {
		return false;
	}
	wp_fault_check("vdev start");
	v[0] = 0; v[1] = 0;
	if (!wp_wmi("power save off", c, wp_cmd(c, 0x9001, 108, v, 8)) || !jn_vdev_param(0x03, wp.bss[jn.bss].bint, "beacon interval") ||
	    !jn_vdev_param(0x19, jn.mgmt_rate, "management rate") ||
	    !jn_vdev_param(0x11, 60, "bmiss count max") ||
	    !jn_vdev_param(0x3a, 0, "roam offload disable")) {
		return false;
	}
	v[0] = 0; v[1] = 0; v[2] = 0; v[3] = 0;
	memcpy(&v[1], jn.bssid, 6);
	if (!wp_wmi("peer create", c, wp_cmd(c, 0x6001, 97, v, 16))) {
		return false;
	}
	jn.peer = true;
	if (!jn_wait(&jn.peer_mapped, 3000, "HTT PEER_MAP")) {
		wp_hist_dump();
		return false;
	}
	n = swu_auth_req(f, sizeof(f), jn.bssid, wp_mac);
	for (unsigned tries = 0; tries < 3 && !jn.auth_resp; tries++) {
		if (!jn_mgmt_tx(f, n, "authentication")) {
			return false;
		}
		jn_wait(&jn.auth_resp, 400, "authentication response");
	}
	if (!jn.auth_resp || jn.auth_status) {
		return false;
	}
	{
		struct swu_assoc as;
		size_t rl = swu_build_rsn(rsn, jn.group_type);

		memset(&as, 0, sizeof(as));
		memcpy(as.bssid, jn.bssid, 6);
		memcpy(as.sa, wp_mac, 6);
		as.cap = (uint16_t)(0x0001 | (jn.mhz < 4900 ? 0x0420 : 0));
		memcpy(as.ssid, jn.ssid, jn.ssid_len);
		as.ssid_len = jn.ssid_len;
		as.ap_ies = wp.bss[jn.bss].ie;
		as.ap_ies_len = wp.bss[jn.bss].ie_len;
		as.rsn_ie = rsn;
		as.rsn_ie_len = rl;
		n = swu_assoc_req(f, sizeof(f), &as);
		if (n == 0) {
			ev("JOIN: could not build the association request");
			return false;
		}
		memcpy(jn.sta_rsn, rsn, rl);
		jn.sta_rsn_len = rl;
	}
	for (unsigned tries = 0; tries < 3 && !jn.assoc_resp; tries++) {
		if (!jn_mgmt_tx(f, n, "association request")) {
			return false;
		}
		jn_wait(&jn.assoc_resp, 400, "association response");
	}
	if (!jn.assoc_resp || jn.ar.status) {
		return false;
	}
	wp_fault_check("assoc");
	jn.aid = jn.ar.aid;
	swu_wpa_start(&jn.wpa, &jn_ops, NULL, jn.pmk, jn.bssid, wp_mac, jn.sta_rsn, jn.sta_rsn_len);
	if (!jn_vdev_param(0x0d, jn.dtim, "dtim period") ||
	    !jn_vdev_param(0x14, 1, "feature wmm") ||
	    !jn_vdev_wmm_params() ||
	    !jn_peer_assoc()) {
		return false;
	}
	{
		uint32_t pp[3] = { 0, 0x8b, 1 };                        /* ath10k_bss_assoc: pdev peer_stats_info_enable = 1 before VDEV_UP */

		(void)wp_wmi("peer stats info", c, wp_cmd(c, 0x4003, 82, pp, 12));
	}
	v[0] = 0; v[1] = jn.aid; v[2] = 0; v[3] = 0;
	memcpy(&v[2], jn.bssid, 6);
	if (!wp_wmi("vdev up", c, wp_cmd(c, 0x5005, 92, v, 16))) {
		return false;
	}
	jn.vdev_up = true;
	/* ath10k: "some firmware revisions have a buggy powersave state machine and must be poked with a peer param command" */
	(void)jn_peer_param(0xff, 1, "ps workaround poke");
	ev("JOIN: vdev up, waiting for the 4-way handshake");
	if (!jn_wait(&jn.hs_done, 6000, "the 4-way handshake")) {
		return false;
	}
	/* message 4 is out (unencrypted); then pairwise key -> authorize -> group key */
	for (unsigned t = 0; t < 200 && jn.eapol_msdu >= 0; t += 5) {
		wp_poll();
		sleep_ms(5);
	}
	if (!jn.key_pending[0] || !jn_install_key(true) || !jn_wait(&jn.sec_ind, 3000, "pairwise SEC_IND")) {
		return false;
	}
	jn.ptk_installed = true;
	if (!jn_peer_param(3, 1, "peer authorize")) {
		return false;
	}
	if (jn.key_pending[1]) {
		if (!jn_install_key(false) || !jn_wait(&jn.sec_ind, 3000, "group SEC_IND")) {
			return false;
		}
	}
	wp_fault_check("keys");
	jn.connected = true;
	ev("JOIN: CONNECTED to %02x%02x%02x%02x%02x%02x aid %u", jn.bssid[0], jn.bssid[1], jn.bssid[2], jn.bssid[3], jn.bssid[4], jn.bssid[5], jn.aid);
	return true;
}


/* ---- ew0: the network interface iOS sees (the Wi-Fi equivalent of ue0 in sweet_usb.c) ---- */
#define EW_TXQ          32u
#define EW_FRAME_MAX    1600u

static struct {
	bool attached;                  /* en0 was made by hakios.net.c on top of this link */
	lck_mtx_t *lock;
	uint8_t *txq;
	uint16_t txq_len[EW_TXQ];
	unsigned head, n;
	bool up;
	uint64_t rx_frames, tx_frames, tx_drop, rx_drop;
	char info[160];
	bool have_ip;
	uint8_t cur_ip[4];
} ew;

/* iokit/Kernel/hakios.wifi_sm7150.cpp (HakiOS's HakiosWLAN): the IORegistry entries wifid and SystemConfiguration look for */
extern void HakiosWLANStart(const char *interface, const uint8_t *address);
static uint32_t g_kdhcp;                /* boot-arg wlan_kdhcp=1: DHCP in the kernel, as the old tree did; else configd does it */

/* hakios_net_link.transmit: one Ethernet frame to the radio's queue; the packet stays hakios.net.c's */
static bool
ew_net_transmit(mbuf_t m, size_t len)
{
	lck_mtx_lock(ew.lock);
	if (!ew.up || ew.n >= EW_TXQ || len > EW_FRAME_MAX || len < 14) {
		lck_mtx_unlock(ew.lock);
		ew.tx_drop++;
		return false;
	}
	mbuf_copydata(m, 0, len, ew.txq + (size_t)((ew.head + ew.n) % EW_TXQ) * EW_FRAME_MAX);
	ew.txq_len[(ew.head + ew.n) % EW_TXQ] = (uint16_t)len;
	ew.n++;
	lck_mtx_unlock(ew.lock);
	return true;
}

/*
 * Apple80211 ioctls on ew0 (SIOCSA80211 / SIOCGA80211, struct apple80211req): what Apple's IO80211 library (wifid, MobileWiFi,
 * Settings) uses when there is no IO80211Controller in IOKit. Request types are listed in research/wifi/apple80211_ioc_table.txt.
 * Everything not handled yet is logged (first 120 distinct type/direction pairs) so that wifid shows what it wants.
 */
struct sweet_a80211req {
	char req_if_name[16];
	uint32_t req_type;
	uint32_t req_val;
	uint32_t req_len;
	uint32_t req_pad;
	uint64_t req_data;
};
#define SWEET_SIOCSA80211       _IOW('i', 200, struct sweet_a80211req)
#define SWEET_SIOCGA80211       _IOWR('i', 201, struct sweet_a80211req)

/*
 * SCAN_REQ from wifid / Settings (pull to refresh, the Wi-Fi page open): the service loop runs a real scan (the radio leaves the home channel
 * for a few seconds), at most once every 20 s; in between the cache answers at once. After a scan the networks that were not heard are dropped
 * (the cache would otherwise keep them for ever) once they were missed in three scans in a row, keeping the one we are connected to,
 * and jn.bss is found again.
 */
static volatile uint32_t g_scan_req;
static uint64_t g_last_scan_us;
/* protects wp.bss against the pruning in the service thread while the Apple80211 ioctls read it (allocated in ew_if_create) */
static lck_mtx_t *g_bss_lock;
#define BSS_LOCK()      do { if (g_bss_lock != NULL) { lck_mtx_lock(g_bss_lock); } } while (0)
#define BSS_UNLOCK()    do { if (g_bss_lock != NULL) { lck_mtx_unlock(g_bss_lock); } } while (0)

/* the cache entry of the network we are connected to, found by BSSID (the index jn.bss moves when the cache is pruned); -1 if none; call with the lock held */
static int
ew_find_cur(void)
{
	for (unsigned k = 0; k < wp.nbss && k < 48; k++) {
		if (memcmp(wp.bss[k].bssid, jn.bssid, 6) == 0) {
			return (int)k;
		}
	}
	return -1;
}
/* true if another cache entry with the same SSID is the one to report instead of entry i; call with the lock held */
static bool
ew_bss_shadowed(unsigned i)
{
	if (wp.bss[i].ssid[0] == '\0') {
		return false;
	}
	bool imine = jn.connected && memcmp(wp.bss[i].bssid, jn.bssid, 6) == 0;

	for (unsigned k = 0; k < wp.nbss && k < 48; k++) {
		if (k == i || strcmp(wp.bss[k].ssid, wp.bss[i].ssid) != 0) {
			continue;
		}
		bool kmine = jn.connected && memcmp(wp.bss[k].bssid, jn.bssid, 6) == 0;

		if (kmine || (!imine && (wp.bss[k].rssi > wp.bss[i].rssi || (wp.bss[k].rssi == wp.bss[i].rssi && k < i)))) {
			return true;
		}
	}
	return false;
}
static uint64_t nt_us(void);

static void
wp_bss_prune(void)
{
	unsigned n = 0;
	uint32_t newbss = 0xffffffffu;

	BSS_LOCK();
	for (unsigned k = 0; k < wp.nbss; k++) {
		bool mine = jn.connected && memcmp(wp.bss[k].bssid, jn.bssid, 6) == 0;

		if (wp.bss[k].seen) {
			wp.bss[k].missed = 0;
		} else if (!mine && ++wp.bss[k].missed >= 3) {    /* a weak network is not heard in every 3 s scan: gone after three misses in a row */
			continue;
		}
		if (mine) {
			newbss = n;
		}
		if (n != k) {
			wp.bss[n] = wp.bss[k];
		}
		n++;
	}
	wp.nbss = n;
	if (newbss != 0xffffffffu) {
		jn.bss = newbss;
	}
	BSS_UNLOCK();
}

/* Apple80211 events: kernel events vendor Apple (1), class 6, subclass 1; event code, then the interface name (16 bytes) and a payload */
static void
ew_a80211_event(uint32_t code, const void *payload, size_t plen)
{
	struct kev_msg km;
	char name[16] = "en0";

	memset(&km, 0, sizeof(km));
	km.vendor_code = 1;
	km.kev_class = 6;
	km.kev_subclass = 1;
	km.event_code = code;
	km.dv[0].data_length = sizeof(name);
	km.dv[0].data_ptr = name;
	if (plen != 0) {                                /* (the payload after the name, as a second piece: Darwin 27's is 540 bytes) */
		km.dv[1].data_length = (uint32_t)plen;
		km.dv[1].data_ptr = __DECONST(void *, payload);
	}
	(void)kev_post_msg(&km);
}

/*
 * A join's end for Darwin 27 (a 656-byte ASSOCIATE): ASSOC_DONE (9) with 540 bytes -- ten records of 52 bytes about the
 * access points tried (the first: its address), then 8 reserved, the reason at 0x210, the result at 0x214 (1 joined,
 * 2 failed, 9 timed out, 21 wrong key) and 1 at 0x218 to say the result is there. wifid of 24A446/24B119 takes the join's
 * outcome from this event and asks nothing; an empty one left Settings waiting 30 s and saying the join failed. (Layout as
 * UniXNU's iPadOS 27 report documents it: UniXNU-sheng reports/ipados27-wifi-association.md.) 16.5's request: no payload,
 * the outcome then asked as ASSOCIATE_RESULT (21).
 */
static bool g_assoc_modern;

static void
ew_a80211_assoc_done(uint32_t result, const uint8_t *bssid)
{
	if (g_assoc_modern) {
		uint8_t *e = kalloc_data(540, Z_WAITOK | Z_ZERO);

		if (e != NULL) {
			uint32_t one = 1;

			if (bssid != NULL) {
				memcpy(e, bssid, 6);
			}
			memcpy(e + 0x214, &result, 4);
			memcpy(e + 0x218, &one, 4);
			ew_a80211_event(9, e, 540);
			kfree_data(e, 540);
			return;
		}
	}
	ew_a80211_event(9, NULL, 0);
}

/* the events a station driver posts when it associates: SSID, BSSID, association done {status, reason}, link changed {is_link_down, ...} */
static void
ew_a80211_link_down(void)
{
	uint32_t d[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };            /* LINK_CHANGED: is_link_down = 1 */

	ew_a80211_event(4, d, 32);
}

static void
ew_a80211_link_events(void)
{
	uint8_t z[32] = { 0 };
	uint8_t aev[16] = { 0 };

	if (jn.connected && ew.up) {
		memcpy(aev, jn.bssid, 6);
	}
	if (g_assoc_modern) {
		/* Darwin 27, as UniXNU's verified order: the join's end first, then the name, the access point and the link */
		ew_a80211_assoc_done(jn.connected && ew.up ? 1 : (g_join_result != 0 ? g_join_result : 2), aev);
		ew_a80211_event(2, NULL, 0);
		ew_a80211_event(3, NULL, 0);
		ew_a80211_event(4, z, 32);
		return;
	}
	ew_a80211_event(2, z, 8);
	ew_a80211_event(3, z, 0);
	/*
	 * ASSOC_DONE with nothing after the interface's name, as drivers/board/apple80211.c posts it for Darwin 26/27: wifid
	 * then asks ASSOCIATE_RESULT (21) for the outcome. With 16 bytes here (the access point's address where 16.5's struct
	 * has {status, reason}) Darwin 27's never asked, and Settings said the join failed after ~30 s although the station
	 * had joined. boot-arg assoc_done_payload=1 brings the 16 bytes back.
	 */
	{
		uint32_t old = 0;

		PE_parse_boot_argn("assoc_done_payload", &old, sizeof(old));
		ew_a80211_event(9, old ? aev : NULL, old ? 16 : 0);
	}
	ew_a80211_event(4, z, 32);
}


static volatile uint32_t g_a80211_power = 1;           /* what Settings' Wi-Fi switch says (the radio itself stays on) */

/* copy a reply of n bytes (as much as the caller has room for) out to the request's buffer */
static int
ew_a80211_out(const struct sweet_a80211req *rq, const void *buf, size_t n)
{
	return copyout(buf, (user_addr_t)rq->req_data, MIN((size_t)rq->req_len, n));
}

static uint32_t
ew_a80211_channel(uint16_t mhz)
{
	return mhz > 5000 ? (uint32_t)(mhz - 5000) / 5 : mhz == 2484 ? 14 : (uint32_t)(mhz - 2407) / 5;
}

/* apple80211_channel flags (itlwm's apple80211_var.h): 20 MHz 0x2, 40 MHz 0x4, 2.4 GHz 0x8, 5 GHz 0x10, passive 0x40, DFS 0x80 */
static uint32_t
ew_a80211_chflags(uint16_t mhz)
{
	return 0x2 | (mhz > 5000 ? 0x10u : 0x8u);
}

/* kern.sweet_wlan_rssi_offset: dB added to the signal strength of scanned networks as Apple80211 reports it (iOS hides networks below -80 dBm) */
static int g_rssi_offset = 10;
SYSCTL_INT(_kern, OID_AUTO, sweet_wlan_rssi_offset, CTLFLAG_RW | CTLFLAG_LOCKED, &g_rssi_offset, 0, "dB added to scanned networks' signal in Apple80211 results");

/*
 * apple80211_scan_result, 1160 bytes (found from the loads in __addScanResultToList): version 0, channel {version, channel, flags}
 * at 4, noise/snr/rssi int16 at 0x12/0x14/0x16, beacon interval 0x18, capabilities 0x1a, BSSID 0x1c, rate count 0x22, SSID length
 * 0x60 and bytes 0x61, age 0x84, flags 0x88, IE length (u16) 0x8a, IEs 0x8c. The same record is the "current network" (type 103).
 */
static void
ew_a80211_scanrec(uint8_t out[1160], unsigned k)
{
	uint32_t u32;
	int16_t i16;
	uint16_t u16;
	const char *ssid = wp.bss[k].ssid;
	unsigned slen = strlen(ssid);

	if (slen == 0 && jn.connected && memcmp(wp.bss[k].bssid, jn.bssid, 6) == 0 && jn.ssid_len > 0) {
		ssid = jn.ssid;
		slen = MIN((unsigned)jn.ssid_len, 32u);
	}

	memset(out, 0, 1160);
	u32 = 1; memcpy(out + 0, &u32, 4);
	memcpy(out + 4, &u32, 4);
	u32 = ew_a80211_channel(wp.bss[k].mhz); memcpy(out + 8, &u32, 4);
	u32 = ew_a80211_chflags(wp.bss[k].mhz); memcpy(out + 12, &u32, 4);
	i16 = -95; memcpy(out + 0x12, &i16, 2);
	{
		/* Settings asks for minRSSI=-80 and wifid drops weaker networks; the offset lifts the other networks' signal (not the connected one's) */
		int rssi = wp.bss[k].rssi + (jn.connected && memcmp(wp.bss[k].bssid, jn.bssid, 6) == 0 ? 0 : g_rssi_offset);

		if (rssi > -30) {
			rssi = -30;
		}
		i16 = (int16_t)(rssi + 95); memcpy(out + 0x14, &i16, 2);
		i16 = (int16_t)rssi; memcpy(out + 0x16, &i16, 2);
	}
	u16 = wp.bss[k].bint; memcpy(out + 0x18, &u16, 2);
	u16 = wp.bss[k].cap; memcpy(out + 0x1a, &u16, 2);
	memcpy(out + 0x1c, wp.bss[k].bssid, 6);
	out[0x60] = (uint8_t)slen;
	memcpy(out + 0x61, ssid, MIN(slen, (unsigned)32));

	uint32_t max_out_ie = MIN((uint32_t)wp.bss[k].ie_len, (uint32_t)(1160 - 0x8c));
	uint32_t valid_out_ie = 0;
	for (uint32_t o = 0; o + 2u <= max_out_ie; ) {
		uint8_t l = wp.bss[k].ie[o + 1];
		if (o + 2u + l > max_out_ie) {
			break;
		}
		valid_out_ie = o + 2u + l;
		o += 2u + l;
	}
	u16 = (uint16_t)valid_out_ie;
	memcpy(out + 0x8a, &u16, 2);
	if (valid_out_ie > 0) {
		memcpy(out + 0x8c, wp.bss[k].ie, valid_out_ie);
	}
}

static int
ew_a80211(bool set, struct sweet_a80211req *rq)
{
	static uint32_t seen[128];
	static unsigned nseen;
	uint32_t key = rq->req_type | (set ? 0x80000000u : 0);
	int err = EOPNOTSUPP;
	bool up = jn.connected && ew.up;

	if (rq->req_data == 0 && rq->req_len != 0) {
		return EINVAL;
	}
	if (set && rq->req_type != 10) {
		/* what wifid sends (the first three of each type): a hex dump of up to 96 bytes, to learn the layouts of the set requests */
		static uint8_t nset[512];

		if (rq->req_type < sizeof(nset) && nset[rq->req_type] < 3) {
			uint8_t d[96];
			char hex[96 * 2 + 1];
			size_t n = rq->req_type == 20 ? 0 : MIN((size_t)rq->req_len, sizeof(d));     /* (ASSOCIATE carries the key: lengths only) */

			nset[rq->req_type]++;
			if (n != 0 && copyin((user_addr_t)rq->req_data, d, n) == 0) {
				for (size_t k = 0; k < n; k++) {
					snprintf(hex + 2 * k, 3, "%02x", d[k]);
				}
				ev("A80211 SET %u len %u val %u: %s", rq->req_type, rq->req_len, rq->req_val, hex);
			} else {
				ev("A80211 SET %u len %u val %u", rq->req_type, rq->req_len, rq->req_val);
			}
		}
	}
	switch (rq->req_type) {
	case 12: {                                              /* CARD_CAPABILITIES: 13 bytes, a bitmap */
		uint8_t caps[13] = { 0 };

		/*
		 * apple80211_var.h APPLE80211_CAP_*, a bit each: AES 2, AES-CCM 3, power management 6, short slot 9, short preamble 10, WPA2 14, WME 17,
		 * short GI at 20 MHz 19 (the set the Topaz port of this tree found to work). Apple80211AssociateAsync fails with -3903 before it
		 * sends ASSOCIATE unless the capability byte at handle+0x51 has bit 5 (WPA, 13) or bit 6 (WPA2, 14) set.
		 */
		static const uint8_t has[] = { 2, 3, 6, 9, 10, 14, 17, 19 };

		for (unsigned i = 0; i < sizeof(has); i++) {
			caps[has[i] / 8] |= (uint8_t)(1u << (has[i] % 8));
		}
		if (!set) {
			err = ew_a80211_out(rq, caps, sizeof(caps));
		}
		break;
	}
	case 19: {                                              /* POWER: {version, num_radios, power_state[4]} */
		if (set) {
			uint32_t in[6] = { 0 };

			err = copyin((user_addr_t)rq->req_data, in, MIN((size_t)rq->req_len, sizeof(in)));
			if (err == 0) {
				g_a80211_power = in[2];
			}
		} else {
			uint32_t out[6] = { 1, 1, g_a80211_power, 0, 0, 0 };

			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 13:                                                /* STATE: the value is returned in req_val: 0 init, 1 scan, 2 auth, 3 assoc, 4 run */
		if (!set) {
			rq->req_val = up ? 4 : 1;
			err = 0;
		}
		break;
	case 1: {                                               /* SSID: raw string bytes up to 32 */
		if (!set) {
			size_t slen = up ? MIN((size_t)jn.ssid_len, (size_t)32) : 0;
			rq->req_len = (uint32_t)slen;
			err = ew_a80211_out(rq, jn.ssid, slen);
		}
		break;
	}
	case 2: {                                               /* AUTH_TYPE: 16 bytes {version, auth_lower, auth_upper, auth_upper_ext} */
		if (!set) {
			/* (WPA2 personal is 0x10 to Darwin 27's wifid, 8 to 16.5's: UniXNU's iPadOS 27 report) */
			uint32_t out[4] = { 1, up ? 1 : 0, up ? (g_assoc_modern ? 0x10 : 8) : 0, 0 };
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 6: {                                               /* PROTMODE: 12 bytes {version=1, mode=0, pad=0} */
		if (!set) {
			uint32_t out[3] = { 1, 0, 0 };
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 5:                                                 /* POWERSAVE: 0 in req_val */
	case 18:                                                /* INT_MIT: 0 in req_val */
	case 26:                                                /* TX_POWER: in req_val (mW) */
	case 57:                                                /* POWER_SAVE_LEVEL: in req_val */
	case 353:                                               /* NSS (number of spatial streams) */
	case 423:                                               /* COUNTRY_BAND_SUPPORT: bitmask (1=2.4G, 2=5G) */
		if (!set) {
			if (rq->req_type == 26) {
				rq->req_val = 100;
			} else if (rq->req_type == 353) {
				rq->req_val = up ? 1 : 0;
			} else if (rq->req_type == 423) {
				rq->req_val = 3;
			} else {
				rq->req_val = 0;
			}
			err = 0;
		}
		break;
	case 9: {                                               /* BSSID: 6 bytes, the bare ether_addr */
		uint8_t out[6] = { 0 };

		if (!set) {
			if (up) {
				memcpy(out, jn.bssid, 6);
			}
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 4: {                                               /* CHANNEL: {version, {version, channel, flags}} */
		uint32_t out[4] = { 1, 1, up ? ew_a80211_channel(jn.mhz) : 0, up ? ew_a80211_chflags(jn.mhz) : 0 };

		if (!set) {
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 16:                                                /* RSSI / NOISE: {version, num_radios, unit, ctl[4], ctl_agr, ext[4], ext_agr} (found with markers) */
	case 17: {
		int32_t out[13] = { 0 };
		int32_t v = -95;

		if (rq->req_type == 16) {
			int cur;

			BSS_LOCK();
			cur = ew_find_cur();
			v = (up && cur >= 0) ? wp.bss[cur].rssi : -100;
			BSS_UNLOCK();
		}

		if (!set) {
			out[0] = 1;
			out[1] = 1;
			out[2] = 0;                                     /* unit: dBm */
			out[3] = v;
			out[7] = v;
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 14: {                                              /* PHY_MODE: {version, supported modes, active mode}: 11a 1, 11b 2, 11g 4, 11n 8 */
		uint32_t out[3] = { 1, 0xf, up ? (jn.mhz > 5000 ? 0x9u : 0xcu) : 0 };

		if (!set) {
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 15:                                                /* OP_MODE: in req_val: 1 station */
		if (!set) {
			rq->req_val = 1;
			err = 0;
		}
		break;
	case 8:                                                 /* RATE: in req_val, Mbit/s */
		if (!set) {
			rq->req_val = up ? 54 : 0;
			err = 0;
		}
		break;
	case 43:                                                /* DRIVER_VERSION / HARDWARE_VERSION: 256 bytes, a C string from offset 0 */
	case 44: {
		char out[256] = { 0 };

		if (!set) {
			strlcpy(out, rq->req_type == 43 ? "SweetWLAN 1.0 (WCN3990)" : "WCN3990", sizeof(out));
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 10:                                                /* SCAN_REQ (set): start over with the results; SCAN_DONE (event 10) */
		if (set) {
			static unsigned nscan;

			if (nscan++ < 40) {
				ev("A80211: scan request #%u (%u networks cached)", nscan, wp.nbss);
			}
			err = 0;
			if (up && nt_us() - g_last_scan_us >= 20000000ull) {
				g_scan_req = 1;                         /* the service loop scans and then posts SCAN_DONE */
			} else {
				ew_a80211_event(10, NULL, 0);           /* the cache is fresh enough (or there is no service loop to scan for us) */
			}
		}
		break;
	case 11: {                                              /* SCAN_RESULT (get): indexed by req_val */
		if (!set) {
			uint32_t idx = rq->req_val;
			uint8_t out[1160];
			bool have;

			BSS_LOCK();
			/* one record per SSID (the connected BSS, else the strongest): wifid lists every BSSID of a network (2.4 and 5 GHz) as its own row */
			while (idx < wp.nbss && ew_bss_shadowed(idx)) {
				idx++;
			}
			have = idx < wp.nbss;
			if (have) {
				ew_a80211_scanrec(out, idx);
			}
			BSS_UNLOCK();
			if (have) {
				rq->req_val = idx + 1;
				err = ew_a80211_out(rq, out, sizeof(out));
			} else {
				err = EIO;
			}
		}
		break;
	}
	case 103: {                                             /* CURRENT_NETWORK (get): the connected network, in the scan result record */
		if (!set) {
			if (!up) {
				err = ENOLINK;
			} else {
				uint8_t out[1160];

				int cur;

				BSS_LOCK();
				cur = ew_find_cur();
				if (cur >= 0) {
					ew_a80211_scanrec(out, (unsigned)cur);
				} else {
					memset(out, 0, sizeof(out));
					uint32_t u32 = 1;
					memcpy(out + 0, &u32, 4);
					memcpy(out + 4, &u32, 4);
				}
				BSS_UNLOCK();
				{
					static unsigned n103;

					if (n103++ < 400) {
						const uint8_t *ie_ssid = swu_ie_find(out + 0x8c, *(uint16_t *)(out + 0x8a), 0);

						ev("A80211: 103 #%u cur=%d nbss=%u ie_ssid=%.*s", n103, cur, wp.nbss, ie_ssid ? ie_ssid[1] : 0, ie_ssid ? (const char *)ie_ssid + 2 : "");
					}
				}
				/* Ensure critical fields always match jn */
				uint32_t ch = ew_a80211_channel(jn.mhz);
				memcpy(out + 8, &ch, 4);
				uint32_t flags = ew_a80211_chflags(jn.mhz);
				memcpy(out + 12, &flags, 4);
				memcpy(out + 0x1c, jn.bssid, 6);
				if (jn.ssid_len > 0) {
					unsigned slen = MIN((unsigned)jn.ssid_len, (unsigned)32);
					out[0x60] = (uint8_t)slen;
					memcpy(out + 0x61, jn.ssid, slen);
				}
				err = ew_a80211_out(rq, out, sizeof(out));
			}
		}
		break;
	}
	case 25:                                                /* HOST_AP_MODE: wifid's last step of its start-up: tell it we are associated */
	case 365:                                               /* DISPLAY_STATE */
	case 383:                                               /* SET_PROPERTY: wifid pushes configuration plists; ignored */
		if (set) {
			err = 0;
			if (rq->req_type == 25 && up) {
				ew_a80211_link_events();
			}
		}
		break;
	case 51:                                                /* COUNTRY_CODE: get {3 bytes}, set {version, cc[4]} */
		if (set) {
			err = 0;
		} else {
			uint8_t out[8] = { 'B', 'G', 0, 0, 0, 0, 0, 0 };

			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	case 254:                                               /* HW_SUPPORTED_CHANNELS: the same list as SUPPORTED_CHANNELS */
	case 27: {                                              /* SUPPORTED_CHANNELS: {version, count, channels[count] of {version, channel, flags}} */
		static const uint16_t chans[] = {
			2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
			5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320, 5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680,
			5700, 5720, 5745, 5765, 5785, 5805, 5825,
		};
		uint32_t out[2 + 3 * 64] = { 1, (uint32_t)(sizeof(chans) / sizeof(chans[0])) };

		if (!set) {
			for (unsigned k = 0; k < sizeof(chans) / sizeof(chans[0]); k++) {
				out[2 + 3 * k] = 1;
				out[3 + 3 * k] = ew_a80211_channel(chans[k]);
				out[4 + 3 * k] = ew_a80211_chflags(chans[k]);
			}
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 106: {                                             /* RSSI_BOUNDS: 16 bytes {version=1, min=-100, max=-30, threshold=-75} */
		if (!set) {
			int32_t out[4] = { 1, -100, -30, -75 };
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 20:                                                /* ASSOCIATE (set): a join asked for in Settings; answered at once, the station loop does it */
		if (set) {
			/* apple80211_assoc, 592 bytes: ssid_length @0xc, ssid @0x10, bssid @0x30, key_length @0x3c, key_cipher @0x40 (10: the passphrase as typed,
			 * 6: the PMK), key @0x48 */
			uint8_t a[0x90];
			uint32_t slen, klen, cipher, k;

			if (rq->req_len < sizeof(a) || copyin((user_addr_t)rq->req_data, a, sizeof(a)) != 0) {
				err = EINVAL;
				break;
			}
			/*
			 * Darwin 27 sends 656 bytes (16.5: 592) and the fields moved. Boot-arg assoc_map=1: the request's shape, a
			 * word at a time -- 0 for zero, a number for a word under 0x1000 (lengths, kinds), x for anything else --
			 * never a byte of the network's name or its key.
			 */
			{
				uint32_t map_on = 0;

				PE_parse_boot_argn("assoc_map", &map_on, sizeof(map_on));
				if (map_on && rq->req_len <= 0x400) {
					uint8_t *all = kalloc_data(rq->req_len, Z_WAITOK | Z_ZERO);

					if (all != NULL && copyin((user_addr_t)rq->req_data, all, rq->req_len) == 0) {
						char line[200];
						size_t at = 0;

						for (uint32_t off = 0; off + 4 <= rq->req_len; off += 4) {
							uint32_t w;

							memcpy(&w, all + off, 4);
							if (at == 0) {
								at += snprintf(line, sizeof(line), "%03x:", off);
							}
							at += w == 0 ? snprintf(line + at, sizeof(line) - at, " 0") :
							    w < 0x1000 ? snprintf(line + at, sizeof(line) - at, " %u", w) :
							    snprintf(line + at, sizeof(line) - at, " x");
							if ((off / 4) % 16 == 15 || off + 8 > rq->req_len) {
								hakios_say("A80211 ASSOC map %s\n", line);
								at = 0;
							}
						}
					}
					if (all != NULL) {
						memset(all, 0, rq->req_len);
						kfree_data(all, rq->req_len);
					}
				}
			}
			/*
			 * The key's block: 16.5 (592 bytes) has its version at 0x38, length 0x3c, kind 0x40, the key at 0x48. Darwin 27 (656)
			 * has 4 bytes more before it -- version 0x3c, length 0x40, kind 0x44, key 0x4c: its _performAssociation clears the
			 * block from 0x3c, and read at 16.5's places a join said "a key of 1 byte of kind 32" (version 1, length 32).
			 * The name moved the same 4 bytes (its length 0x10, its bytes 0x14; at 0xc Darwin 27 has the kind of authentication,
			 * 8 for WPA2 with a passphrase): assoc_map=1 showed a length of 21 at 0x10 and 21 bytes of text from 0x14.
			 */
			k = rq->req_len >= 0x290 ? 4 : 0;
			g_assoc_modern = k != 0;
			memcpy(&slen, a + 0xc + k, 4);
			memcpy(&klen, a + 0x3c + k, 4);
			memcpy(&cipher, a + 0x40 + k, 4);
			ev("A80211: ASSOCIATE (%u bytes): name of %u bytes, key of %u bytes of kind %u", rq->req_len, slen, klen, cipher);
			if (slen == 0 || slen > 32 || g_ujoin_pending) {
				err = EINVAL;
				break;
			}
			memset(&g_ujoin_new, 0, sizeof(g_ujoin_new));
			memcpy(g_ujoin_new.ssid, a + 0x10 + k, slen);
			g_ujoin_new.ssid_len = (uint8_t)slen;
			if (cipher == 10 && klen >= 8 && klen <= 63) {
				ujoin_pmk(a + 0x48 + k, klen, a + 0x10 + k, slen, g_ujoin_new.pmk);
			} else if (cipher == 6 && klen == 32) {
				memcpy(g_ujoin_new.pmk, a + 0x48 + k, 32);
			} else {
				memset(a, 0, sizeof(a));
				err = EINVAL;
				break;
			}
			memset(a, 0, sizeof(a));
			g_join_result = 0;
			g_ujoin_pending = 1;
			err = 0;
		}
		break;
	case 21:                                                /* ASSOCIATE_RESULT (get): how the last join went, in req_val (0: not over yet) */
		if (!set) {
			rq->req_val = g_join_result;
			err = 0;
		}
		break;
	case 22:                                                /* DISASSOCIATE (set): wifid sends it before every join; accepted without leaving the network */
		if (set) {
			err = 0;
		}
		break;
	case 422:                                               /* 6G_MODE (set): wifid sets it before every ASSOCIATE and abandons the join when it fails; no 6 GHz here */
		if (set) {
			err = 0;
		}
		break;
	case 207: {                                             /* CHANNELS_INFO: {version, ..., count=0 at offset 12} */
		if (!set) {
			uint32_t out[4] = { 1, 0, 0, 0 };
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	case 387: {                                             /* COLOCATED_NETWORK_SCOPE_ID: 37 bytes */
		if (!set) {
			char out[37] = { 0 };
			err = ew_a80211_out(rq, out, sizeof(out));
		}
		break;
	}
	default:
		break;
	}
	{
		unsigned i;

		static uint32_t log_all = 2;

		if (log_all == 2) {
			log_all = 0;
			PE_parse_boot_argn("a80211_log", &log_all, sizeof(log_all));   /* every request and its answer, not only new ones */
		}
		for (i = 0; i < nseen && seen[i] != key; i++) {
		}
		if (log_all && rq->req_type != 103) {
			ev("A80211: %s %u len %u -> %d val %u", set ? "set" : "get", rq->req_type, rq->req_len, err, rq->req_val);
		}
		if (i == nseen && nseen < 128) {
			seen[nseen++] = key;
			ev("A80211: %s type %u len %u val %u -> %d", set ? "set" : "get", rq->req_type, rq->req_len, rq->req_val, err);
		}
	}
	return err;
}

/* what net.c passes on of en0's ioctls for a radio: Darwin's Wi-Fi side (wifid, Settings) and its requests; apple80211.c
 * hands them here on a board that lists wifi-wcn3990 */
errno_t hakios_sm7150_apple80211_ioctl(u_long command, void *data);

errno_t
hakios_sm7150_apple80211_ioctl(u_long command, void *data)
{
	if (command == SWEET_SIOCSA80211 || command == SWEET_SIOCGA80211) {
		return ew_a80211(command == SWEET_SIOCSA80211, (struct sweet_a80211req *)data);
	}
	return EOPNOTSUPP;
}

static struct hakios_net_link ew_link = {
	.name = "sunfish-wlan",
	.unit = 0,
	.wireless = true,
	.up = false,
	.transmit = ew_net_transmit,
	.poll = NULL,
};

static void
ew_if_create(void)
{
	static lck_grp_t *grp;

	if (ew.attached) {
		return;
	}
	grp = lck_grp_alloc_init("sweet_wlan_if", LCK_GRP_ATTR_NULL);
	ew.lock = lck_mtx_alloc_init(grp, LCK_ATTR_NULL);
	g_bss_lock = lck_mtx_alloc_init(grp, LCK_ATTR_NULL);
	ew.txq = kalloc_data(EW_TXQ * EW_FRAME_MAX, Z_WAITOK | Z_ZERO);
	wp_mac_take();                  /* before en0 is made with it: the radio and en0 must have the same one */
	memcpy(ew_link.address, wp_mac, ETHER_ADDR_LEN);
	if (ew.txq == NULL || !hakios_net_attach(&ew_link)) {
		ev("EW: hakios_net_attach failed");
		return;
	}
	ew.attached = true;
	ev("EW: interface en0 created by hakios.net.c");
	HakiosWLANStart("en0", wp_mac);
}

static void
ew_link_event(bool up)
{
	if (ew.attached) {
		hakios_net_link_state(up);
	}
}

/* the link is up (associated, keys set): configd starts DHCP on it, and wifid is told */
static void
ew_link_up(void)
{
	ew.up = true;
	ew_link_event(true);
	ew_a80211_link_events();
	ev("EW: en0 link up: configd runs DHCP");
}

/* give ew0 the leased address and mark the link up (sweet_usb.c's ue0 does it the same way) */
static void
ew_configure(const uint8_t ip[4], const uint8_t mask[4])
{
	errno_t err = 0;
	socket_t so = NULL;

	if (!ew.attached) {
		return;
	}
	if (err == 0 || err == EEXIST) {
		err = sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so);
	}
	if (err == 0) {
		struct ifaliasreq ifra;
		struct sockaddr_in *a;
		uint32_t addr = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3];
		uint32_t m = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16) | ((uint32_t)mask[2] << 8) | mask[3];

		if (ew.have_ip && memcmp(ew.cur_ip, ip, 4) != 0) {        /* a different lease after a rejoin: drop the old address first */
			struct ifreq ifr;
			struct sockaddr_in *o = (struct sockaddr_in *)&ifr.ifr_addr;

			bzero(&ifr, sizeof(ifr));
			snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "en0");
			o->sin_len = sizeof(*o);
			o->sin_family = AF_INET;
			memcpy(&o->sin_addr.s_addr, ew.cur_ip, 4);
			(void)sock_ioctl(so, SIOCDIFADDR, &ifr);
		}
		bzero(&ifra, sizeof(ifra));
		snprintf(ifra.ifra_name, sizeof(ifra.ifra_name), "en0");
		a = (struct sockaddr_in *)&ifra.ifra_addr;
		a->sin_len = sizeof(*a); a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(addr);
		a = (struct sockaddr_in *)&ifra.ifra_mask;
		a->sin_len = sizeof(*a); a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(m);
		a = (struct sockaddr_in *)&ifra.ifra_broadaddr;
		a->sin_len = sizeof(*a); a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(addr | ~m);
		err = sock_ioctl(so, SIOCAIFADDR, &ifra);
		sock_close(so);
	}
	memcpy(ew.cur_ip, ip, 4);
	ew.have_ip = true;
	ew.up = true;
	ew_link_event(true);
	ew_a80211_link_events();
	ev("EW: en0 %u.%u.%u.%u %s (err %d)", ip[0], ip[1], ip[2], ip[3], err ? "NOT configured" : "up", err);
}

/* a decrypted Ethernet frame from the AP to the stack */
static void
ew_rx(const uint8_t *eth, size_t len)
{
	if (!ew.up || !ew.attached || len < ETHER_HDR_LEN || len > ETHER_MAX_LEN) {
		return;
	}
	ew.rx_frames++;
	hakios_net_input(eth, len);              /* copied there; this is the radio's service thread, not an interrupt */
}

static int
sysctl_wlan_info SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	return SYSCTL_OUT(req, ew.info, strlen(ew.info) + 1);
}

/* kern.sweet_wlan_vparam = (vdev param id << 24) | value: set a WMI vdev parameter on the running station (e.g. 0x1b000004 = fixed rate OFDM 24 Mbit/s, 0x1b000207 = HT MCS 7, 0x1b000000 = back to rate control) */
static volatile uint32_t g_vparam;

static int
sysctl_wlan_vparam SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = 0, err = sysctl_handle_int(oidp, &v, 0, req);

	if (err == 0 && req->newptr != USER_ADDR_NULL && (uint32_t)v >> 24 != 0) {
		g_vparam = (uint32_t)v;
	}
	return err;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_wlan_vparam, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_wlan_vparam, "I", "set a Wi-Fi vdev parameter");

/* kern.sweet_wlan_event = N: post Apple80211 event N (payload sizes as the library expects: 2 and 9 eight bytes, 4 thirty-two, else none); 0 = the set of association events */
static int
sysctl_wlan_event SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = 0, err = sysctl_handle_int(oidp, &v, 0, req);

	if (err == 0 && req->newptr != USER_ADDR_NULL) {
		uint8_t z[32] = { 0 };

		if (v == 0) {
			ew_a80211_link_events();
		} else if (v > 0) {
			ew_a80211_event((uint32_t)v, z, v == 2 || v == 9 ? 8 : v == 4 ? 32 : 0);
		}
	}
	return err;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_wlan_event, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_wlan_event, "I", "post an Apple80211 event");

/* kern.sweet_wlan_kick=1: behave as if the AP had kicked us (tests the rejoin path) */
static int
sysctl_wlan_kick SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = 0, err = sysctl_handle_int(oidp, &v, 0, req);

	if (err == 0 && req->newptr != USER_ADDR_NULL && v == 1) {
		jn.kicked = true;
	} else if (err == 0 && req->newptr != USER_ADDR_NULL && v == 2) {
		g_wlan_crashed = true;                  /* test the modem restart without waiting for the firmware to crash */
	}
	return err;
}

SYSCTL_PROC(_kern, OID_AUTO, sweet_wlan_kick, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_wlan_kick, "I", "force a Wi-Fi rejoin");

SYSCTL_PROC(_kern, OID_AUTO, sweet_wlan_info, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_wlan_info, "A",
    "Wi-Fi state: ip mask router dns ssid");

/* ---- a minimal IPv4 test client: DHCP, ARP the router, ping it (proves data TX and RX) ---- */
static struct {
	int state;                      /* 0 idle, 1 discover sent, 2 offer seen, 3 request sent, 4 bound */
	uint32_t xid;
	uint8_t yiaddr[4], server[4], router[4], mask[4], dns[4], router_mac[6];
	bool have_mac;
	unsigned pongs, offers;
	uint16_t ping_seq;
	uint64_t ping_t0;
	int64_t rtt_us[3];
} nt;

static uint64_t
nt_us(void)
{
	uint64_t abs, ns;

	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000;
}

static uint16_t
ip_sum(const uint8_t *p, size_t n)
{
	uint32_t sum = 0;

	for (size_t i = 0; i + 1 < n; i += 2) {
		sum += (uint32_t)((p[i] << 8) | p[i + 1]);
	}
	if (n & 1) {
		sum += (uint32_t)(p[n - 1] << 8);
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

static size_t
nt_ip_hdr(uint8_t *f, const uint8_t src[4], const uint8_t dst[4], uint8_t proto, size_t payload)
{
	uint16_t sum, total = (uint16_t)(20 + payload);

	memset(f, 0, 20);
	f[0] = 0x45;
	f[2] = (uint8_t)(total >> 8);
	f[3] = (uint8_t)total;
	f[6] = 0x40;                                    /* don't fragment */
	f[8] = 64;
	f[9] = proto;
	memcpy(f + 12, src, 4);
	memcpy(f + 16, dst, 4);
	sum = ip_sum(f, 20);
	f[10] = (uint8_t)(sum >> 8);
	f[11] = (uint8_t)sum;
	return 20;
}

static void
nt_dhcp(uint8_t type)
{
	uint8_t f[400], *d, *o;
	static const uint8_t zero[4], bcast[4] = { 255, 255, 255, 255 };
	size_t dl, ul;

	memset(f, 0, sizeof(f));
	memset(f, 0xff, 6);
	memcpy(f + 6, wp_mac, 6);
	f[12] = 0x08;
	f[13] = 0x00;
	d = f + 14 + 20 + 8;                            /* BOOTP */
	d[0] = 1; d[1] = 1; d[2] = 6;
	d[4] = (uint8_t)(nt.xid >> 24); d[5] = (uint8_t)(nt.xid >> 16); d[6] = (uint8_t)(nt.xid >> 8); d[7] = (uint8_t)nt.xid;
	d[10] = 0x80;                                   /* broadcast flag */
	memcpy(d + 28, wp_mac, 6);
	d[236] = 99; d[237] = 130; d[238] = 83; d[239] = 99;
	o = d + 240;
	*o++ = 53; *o++ = 1; *o++ = type;
	*o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, wp_mac, 6); o += 6;
	if (type == 3) {
		*o++ = 50; *o++ = 4; memcpy(o, nt.yiaddr, 4); o += 4;
		*o++ = 54; *o++ = 4; memcpy(o, nt.server, 4); o += 4;
	}
	*o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15;
	*o++ = 255;
	dl = (size_t)(o - d);
	ul = 8 + dl;
	nt_ip_hdr(f + 14, zero, bcast, 17, ul);
	f[34] = 0; f[35] = 68; f[36] = 0; f[37] = 67;
	f[38] = (uint8_t)(ul >> 8); f[39] = (uint8_t)ul;
	(void)jn_tx_eth(f, 14 + 20 + ul, false);
}

static void
nt_arp_router(void)
{
	uint8_t f[42];

	memset(f, 0xff, 6);
	memcpy(f + 6, wp_mac, 6);
	f[12] = 0x08; f[13] = 0x06;
	f[14] = 0; f[15] = 1; f[16] = 0x08; f[17] = 0; f[18] = 6; f[19] = 4; f[20] = 0; f[21] = 1;
	memcpy(f + 22, wp_mac, 6);
	memcpy(f + 28, nt.yiaddr, 4);
	memset(f + 32, 0, 6);
	memcpy(f + 38, nt.router, 4);
	(void)jn_tx_eth(f, 42, false);
}

static void
nt_ping(void)
{
	uint8_t f[14 + 20 + 40];
	uint16_t sum;

	memset(f, 0, sizeof(f));
	memcpy(f, nt.router_mac, 6);
	memcpy(f + 6, wp_mac, 6);
	f[12] = 0x08;
	nt_ip_hdr(f + 14, nt.yiaddr, nt.router, 1, 40);
	f[34] = 8;                                      /* echo request */
	f[38] = 0x53; f[39] = 0x57;                     /* id */
	f[40] = (uint8_t)(nt.ping_seq >> 8); f[41] = (uint8_t)nt.ping_seq;
	for (unsigned i = 0; i < 32; i++) {
		f[42 + i] = (uint8_t)i;
	}
	sum = ip_sum(f + 34, 40);
	f[36] = (uint8_t)(sum >> 8);
	f[37] = (uint8_t)sum;
	nt.ping_t0 = nt_us();
	(void)jn_tx_eth(f, sizeof(f), false);
}

static void
jn_dataframe(const uint8_t *eth, size_t n)
{
	uint16_t type = (uint16_t)((eth[12] << 8) | eth[13]);

	if (ew.up) {
		ew_rx(eth, n);
		return;
	}

	if (type == 0x0806 && n >= 42 && eth[21] == 2 && nt.state == 4 && memcmp(eth + 28, nt.router, 4) == 0) {
		memcpy(nt.router_mac, eth + 22, 6);
		nt.have_mac = true;
		return;
	}
	if (type != 0x0800 || n < 14 + 20) {
		return;
	}
	if (eth[23] == 17 && n >= 14 + 20 + 8 + 240 && eth[36] == 0 && eth[37] == 68) {         /* UDP to port 68 */
		const uint8_t *d = eth + 14 + 20 + 8, *o = d + 240;
		uint8_t mt = 0;
		uint32_t xid = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];

		if (d[0] != 2 || xid != nt.xid) {
			return;
		}
		for (; o + 2 <= eth + n && *o != 255;) {
			uint8_t code = o[0], l = o[1];

			if (code == 0) {
				o++;
				continue;
			}
			if (o + 2 + l > eth + n) {
				break;
			}
			if (code == 53) {
				mt = o[2];
			} else if (code == 54 && l == 4) {
				memcpy(nt.server, o + 2, 4);
			} else if (code == 3 && l >= 4) {
				memcpy(nt.router, o + 2, 4);
			} else if (code == 1 && l == 4) {
				memcpy(nt.mask, o + 2, 4);
			} else if (code == 6 && l >= 4) {
				memcpy(nt.dns, o + 2, 4);
			}
			o += 2 + l;
		}
		if (mt == 2 && nt.state == 1) {
			memcpy(nt.yiaddr, d + 16, 4);
			nt.state = 2;
			nt.offers++;
		} else if (mt == 5 && nt.state == 3) {
			memcpy(nt.yiaddr, d + 16, 4);
			nt.state = 4;
		}
		return;
	}
	if (eth[23] == 1 && nt.state == 4 && memcmp(eth + 26, nt.router, 4) == 0 && eth[34] == 0 && n >= 14 + 20 + 8) {
		unsigned seq = (unsigned)((eth[40] << 8) | eth[41]);

		if (seq < 3 && nt.rtt_us[seq] == 0) {
			nt.rtt_us[seq] = (int64_t)(nt_us() - nt.ping_t0);
			nt.pongs++;
		}
	}
}

static void
jn_net_test(void)
{
	uint64_t t0;
	unsigned tries;

	nt.xid = 0x53575746;
	for (tries = 0; tries < 4 && nt.state != 2; tries++) {
		nt.state = 1;
		nt_dhcp(1);
		t0 = nt_us();
		while (nt.state == 1 && nt_us() - t0 < 2000000) {
			wp_poll();
			sleep_ms(5);
		}
	}
	if (nt.state != 2) {
		ev("NET: no DHCP offer after %u tries", tries);
		return;
	}
	ev("NET: DHCP offer %u.%u.%u.%u from %u.%u.%u.%u, router %u.%u.%u.%u", nt.yiaddr[0], nt.yiaddr[1], nt.yiaddr[2], nt.yiaddr[3], nt.server[0],
	    nt.server[1], nt.server[2], nt.server[3], nt.router[0], nt.router[1], nt.router[2], nt.router[3]);
	nt.state = 3;
	for (tries = 0; tries < 3 && nt.state == 3; tries++) {   /* the ack can be lost: ask again, as a DHCP client does */
		nt_dhcp(3);
		t0 = nt_us();
		while (nt.state == 3 && nt_us() - t0 < 2000000) {
			wp_poll();
			sleep_ms(5);
		}
	}
	if (nt.state != 4) {
		ev("NET: no DHCP ack");
		return;
	}
	ev("NET: bound %u.%u.%u.%u mask %u.%u.%u.%u router %u.%u.%u.%u dns %u.%u.%u.%u", nt.yiaddr[0], nt.yiaddr[1], nt.yiaddr[2], nt.yiaddr[3],
	    nt.mask[0], nt.mask[1], nt.mask[2], nt.mask[3], nt.router[0], nt.router[1], nt.router[2], nt.router[3], nt.dns[0], nt.dns[1], nt.dns[2], nt.dns[3]);
	if (g_wlan_probe >= 8) {
		snprintf(ew.info, sizeof(ew.info), "%u.%u.%u.%u %u.%u.%u.%u %u.%u.%u.%u %u.%u.%u.%u %.*s", nt.yiaddr[0], nt.yiaddr[1], nt.yiaddr[2], nt.yiaddr[3],
		    nt.mask[0], nt.mask[1], nt.mask[2], nt.mask[3], nt.router[0], nt.router[1], nt.router[2], nt.router[3], nt.dns[0], nt.dns[1], nt.dns[2],
		    nt.dns[3], jn.ssid_len, jn.ssid);
		ew_configure(nt.yiaddr, nt.mask);
		return;
	}
	for (tries = 0; tries < 5 && !nt.have_mac; tries++) {
		nt_arp_router();
		t0 = nt_us();
		while (!nt.have_mac && nt_us() - t0 < 500000) {
			wp_poll();
			sleep_ms(5);
		}
	}
	if (!nt.have_mac) {
		ev("NET: the router did not answer ARP");
		return;
	}
	ev("NET: router MAC %02x%02x%02x%02x%02x%02x", nt.router_mac[0], nt.router_mac[1], nt.router_mac[2], nt.router_mac[3], nt.router_mac[4], nt.router_mac[5]);
	for (nt.ping_seq = 0; nt.ping_seq < 3; nt.ping_seq++) {
		nt_ping();
		t0 = nt_us();
		while (nt.rtt_us[nt.ping_seq] == 0 && nt_us() - t0 < 1000000) {
			wp_poll();
			sleep_ms(2);
		}
		ev("NET: ping %u: %s %lld us", nt.ping_seq, nt.rtt_us[nt.ping_seq] ? "reply" : "NO REPLY", (long long)nt.rtt_us[nt.ping_seq]);
	}
	ev("NET: %u of 3 pings answered", nt.pongs);
}

/* one received message (HTC header: eid, flags, len, ctrl[2], seq?, ...) */
/* the last few things the firmware sent and we sent it, dumped when it crashes (to see what it was doing) */
static struct {
	uint32_t t_ms, key;
} g_hist[160];
static unsigned g_hist_n;

static void
wp_hist(uint32_t key)
{
	uint64_t abs, ns;

	if (key == 0x10007001u) {                       /* beacons and probe responses: ten a second, they would fill the ring */
		return;
	}
	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	g_hist[g_hist_n++ % 160].t_ms = (uint32_t)(ns / 1000000);
	g_hist[(g_hist_n - 1) % 160].key = key;
}

static void
wp_hist_dump(void)
{
	char line[200];
	unsigned n = MIN(g_hist_n, 160u), lp = 0;

	for (unsigned i = 0; i < n; i++) {
		unsigned k = (g_hist_n - n + i) % 160;

		lp += (unsigned)snprintf(line + lp, sizeof(line) - lp, " %u:%x", g_hist[k].t_ms % 100000, g_hist[k].key);
		if (lp > 170 || i == n - 1) {
			ev("HIST%s", line);
			lp = 0;
			line[0] = 0;
		}
	}
}

/* a distinct unhandled message class, noted once with its length (to see what the firmware says before it dies) */
static void
wp_note_event(uint32_t key, uint32_t len)
{
	static uint32_t seen[32];
	static unsigned n;

	for (unsigned i = 0; i < n; i++) {
		if (seen[i] == key) {
			return;
		}
	}
	if (n < 32) {
		seen[n++] = key;
		ev("MSG: %s %#x (%u bytes) first seen", (key >> 28) == 1 ? "wmi event" : "htt/other", key & 0xfffffff, len);
	}
}

static void
wp_rx(unsigned ce, const uint8_t *p, uint32_t len)
{
	uint16_t hlen, id;
	uint32_t trailer, plen;
	const uint8_t *pl;

	wp.msgs++;
	if (wp.msgs <= 5) {
		ev("WMI: CE%u msg %u bytes: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", ce, len, p[0], p[1], p[2], p[3], p[4],
		    p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}
	if (len < 10) {
		return;
	}
	memcpy(&hlen, p + 2, 2);
	trailer = (p[1] & 2) ? p[4] : 0;
	if (8u + hlen > len || trailer > hlen) {
		return;
	}
	pl = p + 8;
	plen = hlen - trailer;
	for (uint32_t o = plen; o + 4 <= hlen;) {       /* trailer records: id 1 = credit report {eid, credits} */
		uint8_t rid = pl[o], rl = pl[o + 1];

		if (o + 4 + rl > hlen) {
			break;
		}
		for (uint32_t k = 0; rid == 1 && k + 4 <= rl; k += 4) {
			if (wp.wmi_eid && pl[o + 4 + k] == wp.wmi_eid) {
				wp.wmi_credits = MIN(wp.wmi_credits + pl[o + 4 + k + 1], 2u);
			}
		}
		o += 4u + rl;
	}
	memcpy(&id, pl, 2);
	if (p[0] == 0) {                                /* HTC control */
		if (id == 1 && plen >= 8) {
			memcpy(&wp.credits, pl + 2, 2);
			memcpy(&wp.credit_size, pl + 4, 2);
			wp.htc_ready = true;
			ev("WMI: HTC READY %u credits of %u, %u endpoints", wp.credits, wp.credit_size, pl[6]);
		} else if (id == 3 && plen >= 8) {
			memcpy(&wp.conn_svc, pl + 2, 2);
			memcpy(&wp.conn_max, pl + 6, 2);
			wp.conn_st = pl[4];
			wp.conn_eid = pl[5];
			wp.got_conn = true;
			ev("WMI: HTC connect %#x -> status %u endpoint %u max message %u", wp.conn_svc, wp.conn_st, wp.conn_eid, wp.conn_max);
		}
		return;
	}
	if (wp.wmi_eid && p[0] == wp.wmi_eid && plen >= 4) {
		uint32_t evid;

		memcpy(&evid, pl, 4);
		evid &= 0xffffff;
		wp_hist(0x10000000u | evid);
		if (evid == 0x1) {
			wp_service_ready(pl + 4, plen - 4);
		} else if (evid == 0x3) {
			uint16_t l, tag;

			if (plen >= 4 + 4 + 8) {
				memcpy(&l, pl + 4, 2);
				memcpy(&tag, pl + 6, 2);
				if (tag == 559 && l >= 4 + 8 && 8u + l <= plen) {
					uint32_t ext[2];

					memcpy(ext, pl + 12, 8);
					wp.ack_rssi = (ext[(174 - 128) / 32] >> ((174 - 128) % 32)) & 1;
					wp.ext_addr = (ext[(179 - 128) / 32] >> ((179 - 128) % 32)) & 1;
				}
			}
			wp.svc_avail = true;
		} else if (evid == 0x2) {
			for (uint32_t o = 4; o + 4 <= plen;) {
				uint16_t l, tag;

				memcpy(&l, pl + o, 2);
				memcpy(&tag, pl + o + 2, 2);
				if (o + 4 + l > plen) {
					break;
				}
				if (tag == 35 && l >= 36) {
					memcpy(wp.mac, pl + o + 4 + 24, 6);
					wp.wmi_ready = true;
				}
				o += 4u + l;
			}
			ev("WMI: READY, MAC %02x:%02x:%02x:%02x:%02x:%02x", wp.mac[0], wp.mac[1], wp.mac[2], wp.mac[3], wp.mac[4], wp.mac[5]);
		} else if (jn.active && jn_wmi_event(evid, pl + 4, plen - 4)) {
			wp.msgs--;
		} else if (evid == 0x1d001) {
			wp.echo_ok++;
		} else if (evid == 0x3001) {
			wp_scan_event(pl + 4, plen - 4);
		} else if (evid == 0x7001) {
			wp_mgmt_rx(pl + 4, plen - 4);
		} else {
			wp.msgs--;
			wp_note_event(0x10000000u | evid, plen);
		}
		return;
	}
	if (wp.htt_eid && p[0] == wp.htt_eid) {
		wp_hist(0x20000000u | pl[0]);
	}
	if (jn.active && wp.htt_eid && p[0] == wp.htt_eid && jn_htt(pl, plen)) {
		wp.msgs--;
		return;
	}
	if (p[0] != 0 && p[0] != wp.wmi_eid) {
		wp_note_event(0x20000000u | (ce << 8) | pl[0], plen);
	}
	if (wp.htt_eid && p[0] == wp.htt_eid && plen >= 4 && pl[0] == 0 && !wp.htt_conf) {
		wp.htt_conf = true;
		wp.htt_minor = pl[1];
		wp.htt_major = pl[2];
		ev("WMI: HTT version %u.%u", wp.htt_major, wp.htt_minor);
	}
}

static void
wp_poll(void)
{
	if (g_wlan_halt || g_wlan_crashed || g_modem_crashed || wp.regs == NULL) {
		return;
	}
	for (unsigned r = 0; r < WP_NR; r++) {
		unsigned mask = wp_cfg[r].n - 1;

		if (wp_cfg[r].src) {
			continue;
		}
		for (unsigned guard = 0; guard < wp_cfg[r].n; guard++) {
			struct wp_desc *d = wp_desc(r, wp.sw[r]);
			const uint8_t *buf = wp.pool + wp_cfg[r].bufs + (uint64_t)wp.sw[r] * WP_BUF;
			uint16_t nb;

			wp_sync(d, sizeof(*d));
			nb = *(volatile uint16_t *)&d->nbytes;
			if (nb == 0) {
				break;
			}
			wp_sync(buf, MIN((uint32_t)nb, WP_BUF));
			wp_rx(wp_cfg[r].ce, buf, MIN((uint32_t)nb, WP_BUF));
			wp.sw[r] = (wp.sw[r] + 1) & mask;
			if ((wp.wr[r] & 3) != 0) {
				wp_repost(r);                   /* not on a line boundary: the old way */
			} else if (++wp.pend[r] == 4) {
				wp.pend[r] = 0;
				wp_repost4(r);
			}
		}
	}
}

static bool
wp_wait(bool *flag, unsigned ms, const char *what)
{
	for (unsigned t = 0; t < ms; t += 10) {
		wp_poll();
		if (*flag) {
			return true;
		}
		sleep_ms(10);
	}
	ev("WMI: timeout waiting for %s", what);
	return false;
}

/* the WCN3990's supplies as the Pixel's icnss node lists them: vdd-cx-mx (l9a, 640 mV), xo, rfa, ch0, ch1 */
static const struct { const char *res; unsigned mv; } g_wlan_rails[] = {
	{ "ldoa9", 640 }, { "ldoc1", 1800 }, { "ldoc2", 1304 }, { "ldoc10", 3312 }, { "ldoc11", 3312 },
};

static void
wlan_rails_on(void)
{
	char log[96];
	unsigned lp = 0;

	for (unsigned k = 0; k < sizeof(g_wlan_rails) / sizeof(g_wlan_rails[0]); k++) {
		int err = hakios_rpmh_vreg_set(g_wlan_rails[k].res, g_wlan_rails[k].mv);

		lp += (unsigned)snprintf(log + lp, sizeof(log) - lp, " %s=%d", g_wlan_rails[k].res, err);
		lp = MIN(lp, (unsigned)sizeof(log) - 1);
		sleep_ms(2);
	}
	ev("EW: WCN3990 rails voted:%s, rfclka2=%d", log, hakios_rpmh_rfclk_enable("rfclka2"));
	(void)aop_send("{class: clock, res: qdss, val: 1}");
}

/* power cycle the chip around a modem restart, as icnss does in its recovery path: the new firmware wants a chip at reset */
static void
wlan_rails_cycle(void)
{
	for (int k = (int)(sizeof(g_wlan_rails) / sizeof(g_wlan_rails[0])) - 1; k >= 0; k--) {
		(void)hakios_rpmh_vreg_off(g_wlan_rails[k].res);
		sleep_ms(2);
	}
	sleep_ms(300);
	wlan_rails_on();
}

/* frames ew0 queued: out through the data path (service thread only) */
static void
wlan_tx_pump(void)
{
	for (;;) {
		unsigned k;
		int e;

		lck_mtx_lock(ew.lock);
		if (ew.n == 0) {
			lck_mtx_unlock(ew.lock);
			return;
		}
		k = ew.head;
		lck_mtx_unlock(ew.lock);
		e = jn_tx_eth(ew.txq + (size_t)k * EW_FRAME_MAX, ew.txq_len[k], false);
		if (e == 28) {
			return;                                 /* ring or ids full: try again next loop */
		}
		lck_mtx_lock(ew.lock);
		ew.head = (ew.head + 1) % EW_TXQ;
		ew.n--;
		lck_mtx_unlock(ew.lock);
		if (e == 0) {
			ew.tx_frames++;
		} else {
			ew.tx_drop++;
		}
	}
}

/* tear the station down (VDEV_DOWN, PEER_DELETE, VDEV_STOP as ath10k does) so that the next join starts clean */
static void
jn_cleanup(void)
{
	uint8_t c[32];
	uint32_t v[3] = { 0, 0, 0 };

	if (g_wlan_crashed || g_modem_crashed) {
		return;
	}
	if (jn.vdev_up) {
		(void)wp_wmi("vdev down", c, wp_cmd(c, 0x5007, 94, v, 4));
	}
	if (jn.peer) {
		v[0] = 0;
		memcpy(&v[1], jn.bssid, 6);
		jn.peer_mapped = true;                  /* cleared by HTT PEER_UNMAP */
		(void)wp_wmi("peer delete", c, wp_cmd(c, 0x6002, 98, v, 12));
		for (unsigned t = 0; t < 1000 && jn.peer_mapped; t += 5) {
			wp_poll();
			sleep_ms(5);
		}
	}
	if (jn.vdev_started) {
		v[0] = 0;
		jn.stopped = false;
		(void)wp_wmi("vdev stop", c, wp_cmd(c, 0x5006, 93, v, 4));
		for (unsigned t = 0; t < 2000 && !jn.stopped; t += 5) {
			wp_poll();
			sleep_ms(5);
		}
	}
	ev("EW: station torn down%s", jn.stopped ? "" : " (no VDEV_STOPPED)");
}

/* join, run, and join again when the link is lost: until the WLAN domain or the modem crashes */
static void
wlan_station_loop(void)
{
	uint64_t started = nt_us();
	unsigned lines = 0;
	bool hist_done = false;
	unsigned rc_phase = 0;
	uint64_t rc_next = nt_us() + 3000000, rc_at = 0;

	for (;;) {
		bool found = false;
		uint64_t next_mon;

		if (g_wlan_crashed || g_modem_crashed) {
			return;
		}
		if (g_ujoin_pending) {
			g_ujoin_pending = 0;
			g_ujoin = g_ujoin_new;
			g_ujoin_valid = 1;
		}
		for (unsigned att = 0; att < 4 && !found && !g_wlan_crashed && !g_modem_crashed; att++) {
			if (att > 0 && g_ujoin_pending && !g_ujoin_valid) {
				break;          /* a join Settings asked for came while this one (the stored network) went on: that one first */
			}
			wp_scan();
			wp_fault_check("scan");
			if (!wp.scan_done) {
				break;
			}
			found = jn_read_conf() && jn_pick();
		}
		if (!found || !jn_join()) {
			ev("EW: no connection (found %u): trying again in 5 s", found);
			if (g_ujoin_valid) {
				/* the join Settings asked for failed: forget it (the next try is the stored network) and tell wifid */
				g_ujoin_valid = 0;
				memset(&g_ujoin, 0, sizeof(g_ujoin));
				g_join_result = found ? 21 : 2;
				ew_a80211_assoc_done(g_join_result, NULL);
			}
			if (jn.active) {
				jn_cleanup();
			}
			/* (a join asked for in Settings ends the wait: wifid gives a join some seconds, and it came during this attempt) */
			for (unsigned t = 0; t < 5000 && !g_wlan_crashed && !g_modem_crashed && !g_ujoin_pending; t += 10) {
				wp_poll();
				sleep_ms(10);
			}
			memset(&nt, 0, sizeof(nt));
			{
				uint32_t keep_rx = wp.rx_idx;       /* the HTT RX ring goes on where it was */

				memset(&jn, 0, sizeof(jn));
				wp.rx_idx = keep_rx;
			}
			continue;
		}
		wp_fault_check("join done");
		/*
		 * Station power save on once associated (boot-arg wifi_ps, default 1; 0: as before). The join turns it off
		 * (WMI_STA_POWERSAVE_MODE 0 before VDEV_UP, as ath10k does), and Linux turns it back on after association
		 * (ath10k_config_ps); this driver never did, so the radio stayed awake listening all the time.
		 */
		if (g_wifi_ps) {
			uint8_t c[32];
			uint32_t v[2] = { 0, 1 };                       /* vdev 0, WMI_STA_PS_MODE_ENABLED */

			ev("EW: station power save %s", wp_wmi("power save on", c, wp_cmd(c, 0x9001, 108, v, 8)) ? "on" : "FAILED");
		}
		if (g_ujoin_valid && g_join_result == 0) {
			g_join_result = 1;
		}
		if (g_kdhcp) {
			jn_net_test();                          /* DHCP in the kernel, then en0 gets the address */
			if (nt.state != 4) {
				ev("EW: no lease: joining again");      /* otherwise the link sits here for ever without an address */
				jn.kicked = true;
			}
		} else {
			ew_link_up();                           /* configd does DHCP, makes the route, tells mDNSResponder */
		}
		ev("EW: service running");
		next_mon = nt_us() + 5000000;
		while (!g_wlan_crashed && !g_modem_crashed && !jn.kicked) {
			if (g_ujoin_pending) {
				/*
				 * The network we are on, asked for again (same name and key): nothing to leave. Leaving it fed a loop
				 * (2026-11-19): wifid asked twice, the second request dropped the link just made, the drop made wifid ask
				 * again: link up and down every 3.5 s for good. Now it is told that the join is done.
				 */
				if (g_ujoin_new.ssid_len == jn.ssid_len && memcmp(g_ujoin_new.ssid, jn.ssid, jn.ssid_len) == 0 &&
				    memcmp(g_ujoin_new.pmk, jn.pmk, 32) == 0) {
					static unsigned said;

					g_ujoin_pending = 0;
					g_join_result = 1;
					if (said++ < 5) {
						ev("EW: a join of the network we are on: kept (no leave)");
					}
					ew_a80211_link_events();        /* what wifid hears after a join: SSID, BSSID, association done */
				} else {
					jn.kicked = true;               /* a join asked for in Settings: leave this network, the loop's start takes it */
					break;
				}
			}
			wp_poll();
			wlan_tx_pump();
			if (g_vparam != 0) {
				uint32_t vp = g_vparam;

				g_vparam = 0;
				ev("EW: vdev param %#x = %#x: %s", vp >> 24, vp & 0xffffff, jn_vdev_param(vp >> 24, vp & 0xffffff, "vparam") ? "sent" : "failed");
			}
			/*
			 * The firmware's rate control starts at a poor TX rate on some links (5 GHz: 4.5 Mbit/s for hours where 13 is possible) and does
			 * not climb by itself; forcing a fixed OFDM rate for a second and then handing the vdev back to rate control restarts it.
			 * Once 3 s after the link comes up and then every 5 minutes (it costs a second at 24 Mbit/s).
			 */
			if (rc_phase == 0 && nt_us() >= rc_next) {
				(void)jn_vdev_param(0x1b, 4, "rate control reset");             /* WMI_VDEV_PARAM_FIXED_RATE = OFDM 24 Mbit/s */
				rc_phase = 1;
				rc_at = nt_us() + 1000000;
				rc_next = nt_us() + 300000000ull;
			} else if (rc_phase == 1 && nt_us() >= rc_at) {
				(void)jn_vdev_param(0x1b, 0, "rate control back");              /* 0 = rate control */
				rc_phase = 0;
			}
			if (g_scan_req != 0) {
				g_scan_req = 0;
				for (unsigned k = 0; k < wp.nbss; k++) {
					wp.bss[k].seen = 0;
				}
				wp_scan();
				wp_bss_prune();
				g_last_scan_us = nt_us();
				ev("EW: scan on request: %u networks", wp.nbss);
				ew_a80211_event(10, NULL, 0);
			}
			sleep_ms(2);
			if (nt_us() >= next_mon) {
				uint8_t c[16];
				uint32_t id = 0x1d001, v = ++wp.echo_sent, sg = g_smmu[0x48 / 4];
				uint16_t l4 = 4, tg = 149;

				next_mon += 5000000;
				lines++;
				memcpy(c, &id, 4); memcpy(c + 4, &l4, 2); memcpy(c + 6, &tg, 2); memcpy(c + 8, &v, 4);
				(void)wp_send(1, wp.wmi_eid, c, 12);
				wp_fault_check("monitor");
				if (sg) {
					ev("EW: global fault sgfsr %#x", sg);
					g_smmu[0x48 / 4] = sg;
				}
				if (jn.tx_inflight >= 4 && !hist_done) {
					hist_done = true;
					ev("EW: %u TX frames not completed (rx ring: msgs %u bufs %u reposted %u bad %u idx %u): message history follows", jn.tx_inflight, g_rx_msgs, g_rx_ent, g_rx_reposts, g_rx_bad, wp.rx_idx);
					wp_hist_dump();
				}
				if (lines % 6 == 0) {
					ev("EW: %llu s: tx %llu rx %llu drop %llu/%llu echo %u/%u credits %u inflight %u rxring msgs %u bufs %u reposted %u bad %u idx %u", (nt_us() - started) / 1000000, ew.tx_frames, ew.rx_frames,
					    ew.tx_drop, ew.rx_drop, wp.echo_ok, wp.echo_sent, wp.wmi_credits, jn.tx_inflight, g_rx_msgs, g_rx_ent, g_rx_reposts, g_rx_bad, wp.rx_idx);
				}
			}
		}
		if (ew.up) {
			ew.up = false;
			ew_link_event(false);
			ew_a80211_link_down();
		}
		if (g_wlan_crashed || g_modem_crashed) {
			wp_hist_dump();
			ev("EW: the %s crashed: link down", g_modem_crashed ? "modem" : "WLAN domain");
			return;
		}
		ev("EW: link lost (kicked by the AP or the firmware): joining again");
		jn_cleanup();
		memset(&nt, 0, sizeof(nt));
		{
			uint32_t keep_rx = wp.rx_idx;

			memset(&jn, 0, sizeof(jn));
			wp.rx_idx = keep_rx;
		}
	}
}

static void
wlfw_wmi_bringup(volatile uint32_t *regs)
{
	uint8_t sc[12] = { 5 };

	if (g_wlan_probe >= 8 && wp.pool != NULL) {
		/* restarted after a crash of the WLAN domain: forget the old station, keep the pool, the SMMU bank and ew0 */
		uint8_t *pool = wp.pool;
		uint64_t pa = wp.pa;

		ev("EW: restarting the WLAN service");
		memset(&wp, 0, sizeof(wp));
		wp.pool = pool;
		wp.pa = pa;
		memset(&jn, 0, sizeof(jn));
		memset(&nt, 0, sizeof(nt));
		ew.up = false;
		ew.n = 0;
		ew.head = 0;
	}
	g_wlan_crashed = false;
	if (g_wlan_probe >= 8 && !ew.attached) {
		wlan_rails_on();
		ew_if_create();
	}
	if (!wlfw_smmu_identity(0x240) || !wlfw_smmu_identity(0x241)) {
		ev("WMI: SMMU not set up");
		return;
	}
	wp.regs = regs;
	wp_fault_check("smmu setup");
	if (!wp_rings() || !wp_wait(&wp.htc_ready, 3000, "HTC ready")) {
		return;
	}
	wp_fault_check("htc ready");
	wp.wmi_credits = 0;
	if (!wp_connect(0x0300) || !wp_wait(&wp.got_conn, 2000, "HTT connect") || wp.conn_st != 0) {
		return;
	}
	wp.htt_eid = wp.conn_eid;
	if (!wp_connect(0x0100) || !wp_wait(&wp.got_conn, 2000, "WMI connect") || wp.conn_st != 0) {
		return;
	}
	wp_fault_check("htc connect");
	wp.wmi_eid = wp.conn_eid;
	wp.wmi_credits = 1;
	if (!wp_send(0, 0, sc, sizeof(sc))) {
		ev("WMI: setup complete not sent");
		return;
	}
	ev("WMI: setup complete sent");
	if (!wp_wait(&wp.svc_ready, 5000, "WMI service ready")) {
		return;
	}
	wp_fault_check("service ready");
	wp_wait(&wp.svc_avail, 300, "service available");
	wp_send_init();
	if (wp_wait(&wp.wmi_ready, 5000, "WMI ready")) {
		wp_fault_check("wmi ready");
		if (g_wlan_probe >= 8) {
			wlan_station_loop();
			return;
		}
		wp_scan();
		if (g_wlan_probe >= 7 && wp.scan_done && jn_read_conf() && jn_pick() && jn_join()) {
			jn_net_test();
			for (unsigned t = 0; t < 5000; t += 10) {
				wp_poll();
				sleep_ms(10);
			}
			ev("JOIN: end: tx %llu frames (%llu eapol, %llu failed), rx %llu frames (%llu eapol, %llu dropped)", jn.tx_frames, jn.tx_eapol, jn.tx_fail,
			    jn.rx_frames, jn.rx_eapol, jn.rx_drop);
		}
	}
}

static volatile uint32_t *g_wlan_regs;

static void
wlan_thread(void *arg, wait_result_t wr)
{
#pragma unused(arg, wr)
	wlfw_wmi_bringup(g_wlan_regs);
	g_wlan_thread_running = 0;
	ev("EW: service thread ended");
	thread_terminate(current_thread());
}

static void
wlan_start_thread(volatile uint32_t *regs)
{
	thread_t th;

	if (!os_atomic_cmpxchg(&g_wlan_thread_running, 0, 1, relaxed)) {
		ev("EW: the service thread is still running: not starting a second one");   /* two threads share wp: one clears it under the other */
		return;
	}
	g_wlan_regs = regs;
	g_wlan_halt = false;
	if (kernel_thread_start_priority(wlan_thread, NULL, SWEET_WLAN_PRI, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	} else {
		g_wlan_thread_running = 0;
		ev("EW: cannot start the service thread");
	}
}

/* Read what the WCN3990 copy engines look like from the host: the first check that the host can reach the chip. */
static void
wlfw_probe_hw(void)
{
	volatile uint32_t *regs;

	if (g_wlan_probe >= 8 && g_wlan_regs != NULL) {
		wlan_start_thread(g_wlan_regs);                 /* the service restarts after a crash of the WLAN domain */
		return;
	}
	regs = (volatile uint32_t *)ml_io_map_unmappable(WCN_MMIO, WCN_MMIO_SIZE, WIMG_IO);
	if (regs == NULL) {
		ev("WLAN probe: cannot map the chip's registers");
		return;
	}
	if (g_wlan_probe >= 2) {
		g_events_length = 0;            /* the 8 KB log is full by now: keep only the probe */
		g_events[0] = 0;
	}
	ev("WLAN probe: registers mapped at %p", regs);
	for (unsigned ce = 0; ce < (g_wlan_probe >= 6 ? 0u : 7u); ce++) {
		uint32_t base = (WCN_CE_BASE + 0x1000 * ce) / 4;

		/* sr_size 0x8, dr_size 0x14, current_srri 0x44, current_drri 0x48 */
		ev("WLAN CE%u: sr_size %#x dr_size %#x srri %#x drri %#x", ce, regs[base + 0x8 / 4], regs[base + 0x14 / 4],
		    regs[base + 0x44 / 4], regs[base + 0x48 / 4]);
	}
	if (g_wlan_probe == 2) {
		wlfw_dma_test(regs);
	} else if (g_wlan_probe == 3) {
		wlfw_smmu_dump(false);
	} else if (g_wlan_probe == 8) {
		wlan_start_thread(regs);
	} else if (g_wlan_probe == 6 || g_wlan_probe == 7) {
		wlfw_wmi_bringup(regs);
	} else if (g_wlan_probe == 5) {
		wlfw_ce_rx_test(regs);
	} else if (g_wlan_probe == 4) {
		wlfw_smmu_dump(true);
		wlfw_dma_test(regs);
	}
}

static void
qmi_put_header(uint8_t *p, uint16_t txn, uint16_t msg)
{
	p[0] = 0;                               /* request */
	p[1] = (uint8_t)txn;
	p[2] = (uint8_t)(txn >> 8);
	p[3] = (uint8_t)msg;
	p[4] = (uint8_t)(msg >> 8);
	p[5] = p[6] = 0;
}

static uint32_t
qmi_put_tlv(uint8_t *p, uint32_t at, uint8_t type, const void *value, uint32_t n)
{
	p[at] = type;
	p[at + 1] = (uint8_t)n;
	p[at + 2] = (uint8_t)(n >> 8);
	memcpy(p + at + 3, value, n);
	return at + 3 + n;
}

/* Give the message in g_wlfw.out (QMI header already there) its length and queue it; the pump sends it. */
static void
wlfw_queue(uint32_t at)
{
	g_wlfw.out[5] = (uint8_t)(at - 7);
	g_wlfw.out[6] = (uint8_t)((at - 7) >> 8);
	g_wlfw.out_len = at;
	g_wlfw.pending = true;
}

static uint32_t
wlfw_begin(uint16_t msg)
{
	qmi_put_header(g_wlfw.out, ++g_wlfw.txn, msg);
	return 7;
}

static void
wlfw_fail(const char *what, unsigned result, unsigned error)
{
	g_wlfw.state = WL_FAILED;
	g_wlfw.pending = false;
	hakios_say("SWEET WLFW: %s failed (result %u, error %#x)\n", what, result, error);
	ev("WLFW %s failed (result %u error %#x)", what, result, error);
}

/* Share a region with the modem and the WLAN (qcom_scm_assign_mem from HLOS only): ath10k_qmi_map_msa_permission(). */
static bool
wlfw_assign(uint64_t addr, uint64_t size, bool secure)
{
	static uint64_t done[4];
	static unsigned ndone;
	vm_offset_t page = 0;
	uint8_t *p;
	uint64_t answer = ~0ull, phys;
	uint32_t dest[3] = { VMID_MSS_MSA, VMID_WLAN, VMID_WLAN_CE };
	unsigned n = secure ? 2 : 3;
	int32_t result;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	p = (uint8_t *)page;
	phys = kvtophys(page);
	*(uint32_t *)(p + 0) = VMID_HLOS;
	*(uint64_t *)(p + 64) = addr;
	*(uint64_t *)(p + 72) = size;
	for (unsigned i = 0; i < n; i++) {
		*(uint32_t *)(p + 128 + 24 * i) = dest[i];
		*(uint32_t *)(p + 132 + 24 * i) = SCM_PERM_RW;
	}
	*(uint64_t *)(p + 256) = 4;
	*(uint64_t *)(p + 264) = phys + 128;
	*(uint64_t *)(p + 272) = 24 * n;
	*(uint64_t *)(p + 280) = 0;
	for (int o = 0; o < 512; o += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (p + o) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	scm_fifth = phys + 256;
	result = scm("to share the WLAN memory", SCM_64 | SCM_SIP | SCM_MP_ASSIGN, SCM_ARGS_ASSIGN, phys + 64, 16, phys, &answer);
	ev("WLFW assign_mem %#llx +%#llx (%s): %d", addr, size, secure ? "secure" : "shared", result);
	if (result != 0) {
		for (unsigned i = 0; i < ndone; i++) {
			if (done[i] == addr) {
				ev("WLFW assign_mem %#llx: still shared with the WLAN from before the restart", addr);
				return true;
			}
		}
	}
	if (result == 0 && ndone < 4) {
		done[ndone++] = addr;
	}
	return result == 0;
}

static void
wlfw_send_bdf_chunk(void)
{
	uint8_t flag;
	uint32_t at = wlfw_begin(WLFW_BDF_DOWNLOAD), n = MIN(WLFW_CHUNK, g_wlfw.bdf_len - g_wlfw.bdf_sent);
	uint32_t value;
	uint16_t len16 = (uint16_t)n;
	uint8_t *p = g_wlfw.out;

	flag = 1;
	at = qmi_put_tlv(p, at, 0x01, &flag, 1);                // valid
	value = 0;
	at = qmi_put_tlv(p, at, 0x10, &value, 4);               // file_id
	at = qmi_put_tlv(p, at, 0x11, &g_wlfw.bdf_len, 4);      // total_size
	at = qmi_put_tlv(p, at, 0x12, &g_wlfw.bdf_seg, 4);      // seg_id
	p[at] = 0x13;                                           // data: u16 length, bytes
	p[at + 1] = (uint8_t)(n + 2);
	p[at + 2] = (uint8_t)((n + 2) >> 8);
	memcpy(p + at + 3, &len16, 2);
	memcpy(p + at + 5, g_wlfw_bdf + g_wlfw.bdf_sent, n);
	at += 5 + n;
	flag = (g_wlfw.bdf_sent + n >= g_wlfw.bdf_len) ? 1 : 0;
	at = qmi_put_tlv(p, at, 0x14, &flag, 1);                // end
	g_wlfw.bdf_sent += n;
	g_wlfw.bdf_seg++;
	g_wlfw.state = WL_BDF;
	wlfw_queue(at);
}

static void
wlfw_next_after_cap(void)
{
	/* Both the capability response and the MSA ready indication are in: download the board data. */
	uint64_t size = 0;
	int error = tftp_file(WLFW_BDF_PATH, 0, NULL, 0, &size);

	if (error == 0 && size > 0 && size <= WLFW_BDF_MAX) {
		error = tftp_file(WLFW_BDF_PATH, 0, g_wlfw_bdf, (int)size, NULL);
	} else {
		error = error ? error : EFBIG;
	}
	if (error != 0) {
		hakios_say("SWEET WLFW: no board data " WLFW_BDF_PATH " (error %d)\n", error);
		ev("WLFW no board data %s (error %d)", WLFW_BDF_PATH, error);
		g_wlfw.state = WL_FAILED;
		return;
	}
	g_wlfw.bdf_len = (uint32_t)size;
	g_wlfw.bdf_sent = 0;
	g_wlfw.bdf_seg = 0;
	ev("WLFW board data: %u bytes", g_wlfw.bdf_len);
	wlfw_send_bdf_chunk();
}

static void
wlfw_send_cfg(void)
{
	static const struct { uint32_t pipe, dir, entries, bytes, flags; } ce[12] = {
		{ 0, 2, 32, 2048, 0 }, { 1, 1, 32, 2048, 0 }, { 2, 1, 64, 2048, 0 }, { 3, 2, 32, 2048, 0 },
		{ 4, 2, 256, 256, 8 }, { 5, 2, 1024, 64, 8 }, { 6, 3, 32, 16384, 0 }, { 7, 4, 0, 0, 8 },
		{ 8, 1, 32, 2048, 0 }, { 9, 1, 32, 2048, 0 }, { 10, 1, 32, 2048, 0 }, { 11, 1, 32, 2048, 0 },
	};
	static const struct { uint32_t service, dir, pipe; } svc[21] = {
		{ 0x104, 2, 3 }, { 0x104, 1, 2 }, { 0x102, 2, 3 }, { 0x102, 1, 2 }, { 0x101, 2, 3 }, { 0x101, 1, 2 },
		{ 0x103, 2, 3 }, { 0x103, 1, 2 }, { 0x100, 2, 3 }, { 0x100, 1, 2 }, { 0x001, 2, 0 }, { 0x001, 1, 2 },
		{ 0xfe00, 2, 0 }, { 0xfe00, 1, 2 }, { 0x300, 2, 4 }, { 0x300, 1, 1 }, { 0xfe00, 2, 5 }, { 0x301, 1, 9 },
		{ 0x302, 1, 10 }, { 0x600, 1, 11 }, { 0, 0, 0 },
	};
	static const struct { uint16_t ce, offset; } shadow[12] = {
		{ 0, 0x3c }, { 3, 0x3c }, { 4, 0x3c }, { 5, 0x3c }, { 7, 0x3c }, { 1, 0x40 },
		{ 2, 0x40 }, { 7, 0x40 }, { 8, 0x40 }, { 9, 0x40 }, { 10, 0x40 }, { 11, 0x40 },
	};
	uint32_t at = wlfw_begin(WLFW_WLAN_CFG);
	uint8_t *p = g_wlfw.out;

	p[at] = 0x11;                                   /* tgt_cfg: u8 count, 12 x 20 bytes */
	p[at + 1] = (uint8_t)(1 + 12 * 20);
	p[at + 2] = 0;
	p[at + 3] = 12;
	memcpy(p + at + 4, ce, sizeof(ce));
	at += 4 + sizeof(ce);
	p[at] = 0x12;                                   /* svc_cfg: u8 count, 21 x 12 bytes */
	p[at + 1] = (uint8_t)(1 + 21 * 12);
	p[at + 2] = (uint8_t)((1 + 21 * 12) >> 8);
	p[at + 3] = 21;
	memcpy(p + at + 4, svc, sizeof(svc));
	at += 4 + sizeof(svc);
	p[at] = 0x13;                                   /* shadow_reg: u8 count, 12 x 4 bytes */
	p[at + 1] = (uint8_t)(1 + 12 * 4);
	p[at + 2] = 0;
	p[at + 3] = 12;
	memcpy(p + at + 4, shadow, sizeof(shadow));
	at += 4 + sizeof(shadow);
	g_wlfw.state = WL_CFG;
	wlfw_queue(at);
}

static void
wlfw_send_mode(void)
{
	uint32_t at = wlfw_begin(WLFW_WLAN_MODE), mode = 0;     /* QMI_WLFW_MISSION_V01 */
	uint8_t hw_debug = 0;

	at = qmi_put_tlv(g_wlfw.out, at, 0x01, &mode, 4);
	at = qmi_put_tlv(g_wlfw.out, at, 0x10, &hw_debug, 1);
	g_wlfw.state = WL_MODE;
	wlfw_queue(at);
}

static void
wlfw_handle_response(uint16_t msg, const uint8_t *p, uint32_t len, unsigned result, unsigned error)
{
	uint32_t at = 7;

	if (result != 0 && !(msg == WLFW_HOST_CAP) && msg != WLFW_INI) {
		wlfw_fail("request", result, error);
		ev("WLFW request %#x was refused", msg);
		return;
	}
	switch (g_wlfw.state) {
	case WL_IND_REG: {
		uint32_t a = wlfw_begin(WLFW_HOST_CAP);
		uint32_t daemon = 0;

		a = qmi_put_tlv(g_wlfw.out, a, 0x10, &daemon, 4);
		g_wlfw.state = WL_HOST_CAP;
		wlfw_queue(a);
		break;
	}
	case WL_HOST_CAP: {
		uint32_t a = wlfw_begin(WLFW_MSA_INFO);
		uint64_t addr = WLFW_MSA_ADDR;
		uint32_t size = WLFW_MSA_SIZE;

		a = qmi_put_tlv(g_wlfw.out, a, 0x01, &addr, 8);
		a = qmi_put_tlv(g_wlfw.out, a, 0x02, &size, 4);
		g_wlfw.state = WL_MSA_INFO;
		wlfw_queue(a);
		break;
	}
	case WL_MSA_INFO: {
		/* TLV 3: u8 count, then (u64 address, u32 size, u8 secure_flag) each */
		uint32_t a;

		while (at + 3 <= len) {
			uint8_t type = p[at];
			uint16_t tlen = (uint16_t)(p[at + 1] | p[at + 2] << 8);

			if (at + 3 + tlen > len) {
				break;
			}
			if (type == 3 && tlen >= 1) {
				unsigned count = p[at + 3];

				for (unsigned i = 0; i < count && 1 + 13 * (i + 1) <= tlen; i++) {
					const uint8_t *r = p + at + 4 + 13 * i;
					uint64_t addr, size;

					memcpy(&addr, r, 8);
					size = (uint64_t)(r[8] | r[9] << 8 | r[10] << 16 | (uint32_t)r[11] << 24);
					ev("WLFW MSA region %u: %#llx +%#llx secure %u", i, addr, size, r[12]);
					if (addr < WLFW_MSA_ADDR || addr + size > WLFW_MSA_ADDR + WLFW_MSA_SIZE || !wlfw_assign(addr, size, r[12] != 0)) {
						wlfw_fail("MSA assign", 0, i);
						return;
					}
				}
			}
			at += 3 + tlen;
		}
		sleep_ms(20);                           /* ath10k: the XPU update needs a moment */
		a = wlfw_begin(WLFW_MSA_READY);
		g_wlfw.state = WL_MSA_READY;
		wlfw_queue(a);
		break;
	}
	case WL_MSA_READY: {
		uint32_t a = wlfw_begin(WLFW_CAP);

		g_wlfw.state = WL_CAP;
		wlfw_queue(a);
		break;
	}
	case WL_CAP:
		while (at + 3 <= len) {
			uint8_t type = p[at];
			uint16_t tlen = (uint16_t)(p[at + 1] | p[at + 2] << 8);

			if (at + 3 + tlen > len) {
				break;
			}
			if (type == 0x10 && tlen >= 8) {
				ev("WLFW chip id %#x family %#x", (uint32_t)(p[at + 3] | p[at + 4] << 8 | p[at + 5] << 16 | (uint32_t)p[at + 6] << 24),
				    (uint32_t)(p[at + 7] | p[at + 8] << 8 | p[at + 9] << 16 | (uint32_t)p[at + 10] << 24));
			} else if (type == 0x11 && tlen >= 4) {
				ev("WLFW board id %#x", (uint32_t)(p[at + 3] | p[at + 4] << 8 | p[at + 5] << 16 | (uint32_t)p[at + 6] << 24));
			} else if (type == 0x12 && tlen >= 4) {
				ev("WLFW soc id %#x", (uint32_t)(p[at + 3] | p[at + 4] << 8 | p[at + 5] << 16 | (uint32_t)p[at + 6] << 24));
			} else if (type == 0x13 && tlen >= 5) {
				ev("WLFW fw version %#x", (uint32_t)(p[at + 3] | p[at + 4] << 8 | p[at + 5] << 16 | (uint32_t)p[at + 6] << 24));
			}
			at += 3 + tlen;
		}
		g_wlfw.cap_done = true;
		if (g_wlfw.msa_ind) {
			wlfw_next_after_cap();
		}
		break;
	case WL_BDF:
		if (g_wlfw.bdf_sent < g_wlfw.bdf_len) {
			wlfw_send_bdf_chunk();
		} else {
			uint32_t a = wlfw_begin(WLFW_CAL_REPORT);
			uint8_t none = 0;

			a = qmi_put_tlv(g_wlfw.out, a, 0x01, &none, 1);         /* meta_data_len 0 */
			g_wlfw.state = WL_CAL;
			wlfw_queue(a);
		}
		break;
	case WL_CAL:
		/* The configuration and the mode are sent when the firmware says it is ready (ath10k: at hif power up). */
		g_wlfw.state = WL_WAIT_READY;
		if (g_wlfw.fw_ready) {
			wlfw_send_cfg();
		}
		break;
	case WL_CFG:
		wlfw_send_mode();
		break;
	case WL_MODE:
		g_wlfw.state = WL_DONE;
		ev("WLFW configured in mission mode");
		{
			/* ath10k_qmi_ini_send_sync_msg: fw_log_mode 0 (diag log), what ath10k_core_start asks for after the mode is on */
			uint32_t a = wlfw_begin(WLFW_INI);
			uint8_t mode = 0;

			a = qmi_put_tlv(g_wlfw.out, a, 0x10, &mode, 1);
			wlfw_queue(a);
		}
		if (g_wlan_probe) {
			wlfw_probe_hw();
		}
		break;
	default:
		break;
	}
}

static void
sweet_wlfw_handle(struct sweet_glink_channel *ch, const struct qrtr_hdr_v1 *qh, const uint8_t *p, uint32_t len)
{
	uint16_t msg;
	unsigned result = 0, error = 0;

	(void)qh;
	g_tftp_ch = ch;
	if (len < 7) {
		return;
	}
	msg = (uint16_t)(p[3] | p[4] << 8);
	if (p[0] == 4) {                                        /* indication */
		ev("WLFW indication %#x", msg);
		if (msg == WLFW_MSA_READY_IND) {
			g_wlfw.msa_ind = true;
			if (g_wlfw.cap_done && g_wlfw.state == WL_CAP) {
				wlfw_next_after_cap();
			}
		} else if (msg == WLFW_FW_READY_IND) {
			hakios_say("SWEET WLFW: THE WLAN FIRMWARE IS READY\n");
			ev("WLFW FIRMWARE READY");
			note("[WLFW READY]");
			g_wlfw.fw_ready = true;
			if (g_wlfw.state == WL_WAIT_READY) {
				wlfw_send_cfg();
			}
		}
		return;
	}
	if (p[0] != 2) {
		return;
	}
	for (uint32_t at = 7; at + 3 <= len;) {
		uint16_t tlen = (uint16_t)(p[at + 1] | p[at + 2] << 8);

		if (at + 3 + tlen > len) {
			break;
		}
		if (p[at] == 2 && tlen >= 4) {
			result = (unsigned)(p[at + 3] | p[at + 4] << 8);
			error = (unsigned)(p[at + 5] | p[at + 6] << 8);
		}
		at += 3 + tlen;
	}
	ev("WLFW response %#x: result %u error %#x (state %d)", msg, result, error, g_wlfw.state);
	wlfw_handle_response(msg, p, len, result, error);
}

/* The service has appeared (QRTR NEW_SERVER): start the sequence. */
static void
sweet_wlfw_server(struct sweet_glink_channel *ch, uint32_t node, uint32_t port)
{
	uint32_t at;
	uint8_t one = 1;
	uint32_t client = WLFW_CLIENT_ID;

	if (g_wlfw.state != WL_IDLE) {
		if (g_wlfw.state != WL_DONE || g_wlan_probe < 8) {
			return;
		}
		/* the WLAN protection domain restarted (crash recovery): the whole sequence again, on the new port */
		memset(&g_wlfw, 0, offsetof(typeof(g_wlfw), out));
		ev("WLFW service %u is back: starting over", WLFW_SERVICE);
	}
	g_tftp_ch = ch;
	g_wlfw.known = true;
	g_wlfw.node = node;
	g_wlfw.port = port;
	at = wlfw_begin(WLFW_IND_REGISTER);
	at = qmi_put_tlv(g_wlfw.out, at, 0x10, &one, 1);        /* fw_ready_enable */
	at = qmi_put_tlv(g_wlfw.out, at, 0x13, &one, 1);        /* msa_ready_enable */
	at = qmi_put_tlv(g_wlfw.out, at, 0x15, &client, 4);     /* client_id */
	g_wlfw.state = WL_IND_REG;
	ev("WLFW service %u at %u:%u: registering", WLFW_SERVICE, node, port);
	wlfw_queue(at);
}

static void
sweet_wlfw_pump(void)
{
	if (!g_wlfw.pending || g_tftp_ch == NULL) {
		return;
	}
	if (tftp_send_to(g_wlfw.node, g_wlfw.port, WLFW_CLIENT_PORT, g_wlfw.out, g_wlfw.out_len)) {
		g_wlfw.pending = false;
		return;
	}
	if (g_send_failure == 1) {
		uint32_t need = g_wlfw.out_len + 28, have = 0;

		for (uint32_t i = 0; i < g_tftp_ch->num_intents; i++) {
			if (!g_tftp_ch->intents[i].in_use && g_tftp_ch->intents[i].size >= need) {
				have++;
			}
		}
		if (have == 0 && g_pump_clock - g_last_req_clock > 8) {
			struct glink_msg req = { .cmd = GLINK_CMD_RX_INTENT_REQ, .param1 = g_tftp_ch->lcid, .param2 = need };

			sweet_glink_tx_cmd(&g_glink, &req, sizeof(req));
			g_last_req_clock = g_pump_clock;
			g_big_pending = 1;
		}
	}
}

/*
 * The modem's cellular side to low-power mode (boot-arg modem_lpm, default 1; 0: left online): with no SIM it otherwise
 * searches for any network to camp on for emergency calls, and the modem stays busy (~110 mA idle in the 2026-11-19
 * measurements, its largest single part). QMI DMS (service 2) SET_OPERATING_MODE (0x002E), TLV 0x01 = 1 (LPM), what
 * airplane mode does to the cellular radio. Wi-Fi runs on the WCN3990, not on this radio.
 */
#define DMS_SERVICE             2
#define DMS_CLIENT_PORT         102
#define DMS_SET_OPERATING_MODE  0x002e
#define DMS_GET_OPERATING_MODE  0x002d
static uint32_t g_modem_lpm = 1, g_dms_sent, g_dms_node, g_dms_port;
static bool g_dms_known;

/* (noted when the modem announces it; asked at 90 s of the modem's life, by the 1 Hz loop, where the log keeps it) */
static void
sweet_dms_server(uint32_t node, uint32_t port)
{
	g_dms_node = node;
	g_dms_port = port;
	g_dms_known = true;
	hakios_say("mpss: DMS (service 2) at %u:%u\n", node, port);
}

static void
sweet_dms_send(void)
{
	uint8_t q[16];
	uint8_t lpm = 1;
	uint32_t at, node = g_dms_node, port = g_dms_port;

	if (!g_modem_lpm || g_dms_sent) {
		return;
	}
	if (!g_dms_known) {
		hakios_say("mpss: no DMS service announced by 90 s: the cellular radio left as it is\n");
		g_dms_sent = 2;
		return;
	}
	(void)lpm;
	qmi_put_header(q, 1, DMS_GET_OPERATING_MODE);       /* first what it is in; SET follows only from online (0) */
	at = 7;
	q[5] = (uint8_t)(at - 7);
	q[6] = (uint8_t)((at - 7) >> 8);
	g_dms_sent = tftp_send_to(node, port, DMS_CLIENT_PORT, q, at) ? 1 : 0;
	hakios_say("mpss: DMS at %u:%u: its operating mode asked: %s\n", node, port, g_dms_sent ? "sent" : "NOT sent");
}

/* QMI operating modes: 0 online, 1 low power, 2 factory test, 3 offline, 4 resetting, 5 shutting down, 6 persistent LPM, 7 mode-only LPM */
static void
sweet_dms_handle(const uint8_t *p, uint32_t len)
{
	uint16_t msg = len >= 7 ? (uint16_t)(p[3] | p[4] << 8) : 0;
	unsigned result = 0xffff, error = 0, mode = 0xff;

	for (uint32_t at = 7; at + 3 <= len;) {
		uint8_t type = p[at];
		uint16_t n = (uint16_t)(p[at + 1] | p[at + 2] << 8);

		if (at + 3 + n > len) {
			break;
		}
		if (type == 0x02 && n >= 4) {
			result = p[at + 3] | p[at + 4] << 8;
			error = p[at + 5] | p[at + 6] << 8;
		} else if (type == 0x01 && n >= 1) {
			mode = p[at + 3];
		}
		at += 3 + n;
	}
	if (msg == DMS_GET_OPERATING_MODE) {
		hakios_say("mpss: DMS: the cellular radio's operating mode is %u (0 online, 1 low power, 3 offline, 6 persistent low power), result %u error %u\n",
		    mode, result, error);
		if (result == 0 && mode == 0 && g_dms_known) {
			uint8_t q[16], lpm = 1;
			uint32_t at;

			qmi_put_header(q, 2, DMS_SET_OPERATING_MODE);
			at = qmi_put_tlv(q, 7, 0x01, &lpm, 1);
			q[5] = (uint8_t)(at - 7);
			q[6] = (uint8_t)((at - 7) >> 8);
			hakios_say("mpss: DMS: to low-power mode: %s\n", tftp_send_to(g_dms_node, g_dms_port, DMS_CLIENT_PORT, q, at) ? "asked" : "NOT sent");
		}
	} else if (msg == DMS_SET_OPERATING_MODE) {
		hakios_say("mpss: DMS: low-power mode %s (result %u, error %u)\n", result == 0 ? "SET" : "REFUSED", result, error);
	} else {
		hakios_say("mpss: DMS: message %#x, %u bytes back\n", msg, len);
	}
}

static void
sweet_glink_poll(void)
{
	if (!g_glink.ready) return;

	for (int iter = 0; iter < 32; iter++) {
		uint32_t rx_head = g_glink.descs[3];
		uint32_t rx_tail = g_glink.descs[2];
		uint32_t avail = (rx_head >= rx_tail) ? (rx_head - rx_tail) : (g_glink.rx_size - rx_tail + rx_head);

		if (avail < sizeof(struct glink_msg)) break;

		struct glink_msg msg;
		sweet_glink_rx_peek(&g_glink, rx_tail, &msg, sizeof(msg));

		switch (msg.cmd) {
		case GLINK_CMD_VERSION: {
			uint16_t rem_ver = msg.param1;
			uint32_t rem_feat = msg.param2;
			hakios_say("SWEET GLINK RX: CMD_VERSION v%u feat %#x\n", rem_ver, rem_feat);
			note("[GLINK RX v%u f%#x]", rem_ver, rem_feat);
			sweet_glink_rx_advance(&g_glink, sizeof(msg));

			struct glink_msg ack = {
				.cmd = GLINK_CMD_VERSION_ACK,
				.param1 = GLINK_VERSION_1,
				.param2 = rem_feat & GLINK_FEATURE_INTENT_REUSE,
			};
			sweet_glink_tx_cmd(&g_glink, &ack, sizeof(ack));
			g_glink.version_ack_sent = true;
			hakios_say("SWEET GLINK: sent VERSION_ACK\n");
			note("[GLINK sent vack]");
			break;
		}
		case GLINK_CMD_VERSION_ACK: {
			hakios_say("SWEET GLINK RX: CMD_VERSION_ACK v%u feat %#x\n", msg.param1, msg.param2);
			note("[GLINK RX vack]");
			sweet_glink_rx_advance(&g_glink, sizeof(msg));
			g_glink.version_ack_received = true;
			break;
		}
		case GLINK_CMD_OPEN: {
			uint16_t rcid = msg.param1;
			uint32_t name_len = msg.param2 & 0xffff;
			uint32_t pkt_len = (sizeof(msg) + name_len + 7) & ~7;
			if (avail < pkt_len) return; /* wait for full message */

			char chname[32] = {0};
			uint32_t name_off = (rx_tail + (uint32_t)sizeof(msg)) % g_glink.rx_size;
			sweet_glink_rx_peek(&g_glink, name_off, chname, MIN(name_len, 31));

			hakios_say("SWEET GLINK RX: OPEN channel '%s' (rcid %u, name_len %u)\n", chname, rcid, name_len);
			note("[GLINK OPEN '%s' r%u]", chname, rcid);
			sweet_glink_rx_advance(&g_glink, pkt_len);

			struct sweet_glink_channel *ch = sweet_glink_get_or_add_channel(&g_glink, chname);
			if (ch) {
				ch->rcid = rcid;
				ch->remote_open = true;

				/* 1. Reply OPEN_ACK */
				struct glink_msg oack = {
					.cmd = GLINK_CMD_OPEN_ACK,
					.param1 = rcid,
					.param2 = 0,
				};
				sweet_glink_tx_cmd(&g_glink, &oack, sizeof(oack));

				/* 2. Open our side if not already open */
				if (!ch->open) {
					ch->open = true;
					struct {
						struct glink_msg hdr;
						char name[32];
					} ch_open;
					memset(&ch_open, 0, sizeof(ch_open));
					ch_open.hdr.cmd = GLINK_CMD_OPEN;
					ch_open.hdr.param1 = ch->lcid;
					ch_open.hdr.param2 = (uint32_t)strlen(chname) + 1;
					strncpy(ch_open.name, chname, sizeof(ch_open.name) - 1);
					uint32_t tx_sz = (sizeof(ch_open.hdr) + ch_open.hdr.param2 + 7) & ~7;
					sweet_glink_tx_cmd(&g_glink, &ch_open, tx_sz);
				}

				/* 3. Send RX INTENT (advertise 4096 buffer) */
				struct glink_msg_intent intent = {
					.cmd = GLINK_CMD_INTENT,
					.lcid = ch->lcid,
					.count = 1,
					.size = 4096,
					.liid = 1,
				};
				sweet_glink_tx_cmd(&g_glink, &intent, sizeof(intent));
				hakios_say("SWEET GLINK: channel '%s' connected (lcid %u, rcid %u, intent 4096)\n",
				    chname, ch->lcid, ch->rcid);
				note("[GLINK conn '%s' l%u:r%u]", chname, ch->lcid, ch->rcid);
			}
			break;
		}
		case GLINK_CMD_OPEN_ACK: {
			uint16_t lcid = msg.param1;
			hakios_say("SWEET GLINK RX: OPEN_ACK for lcid %u\n", lcid);
			note("[GLINK OACK l%u]", lcid);
			sweet_glink_rx_advance(&g_glink, sizeof(msg));
			break;
		}
		case GLINK_CMD_INTENT: {
			uint16_t rcid = msg.param1;
			uint32_t count = msg.param2;
			uint32_t pkt_len = (sizeof(msg) + count * 8 + 7) & ~7;
			if (avail < pkt_len) return; /* wait for full message */

			struct sweet_glink_channel *ch = sweet_glink_find_by_rcid(&g_glink, rcid);
			for (uint32_t i = 0; i < count; i++) {
				struct {
					uint32_t size;
					uint32_t iid;
				} pair;
				uint32_t pair_off = (rx_tail + (uint32_t)sizeof(msg) + i * 8) % g_glink.rx_size;
				sweet_glink_rx_peek(&g_glink, pair_off, &pair, sizeof(pair));
				if (pair.size > 1000 && g_big_pending > 0) {
					g_big_pending--;
				}
				if (pair.size > 1000 && ch && strncmp(ch->name, "IPCRTR", 6) == 0 && g_events_length < sizeof(g_events) / 2) {
					ev("GLINK intent offered: %u bytes, id %u on '%s' (have %u)", pair.size, pair.iid, ch ? ch->name : "?",
					    ch ? ch->num_intents : 0);
				}
				if (ch && ch->num_intents < 64) {
					ch->intents[ch->num_intents].size = pair.size;
					ch->intents[ch->num_intents].iid = pair.iid;
					ch->intents[ch->num_intents].in_use = false;
					ch->num_intents++;
				}
			}
			sweet_glink_rx_advance(&g_glink, pkt_len);
			break;
		}
		case GLINK_CMD_RX_DONE:
		case GLINK_CMD_RX_DONE_W_REUSE: {
			/* The remote names the channel by its own id (our rcid); with reuse the intent is free again, without it gone. */
			uint16_t remote_cid = msg.param1;
			uint32_t liid = msg.param2;
			struct sweet_glink_channel *ch = sweet_glink_find_by_rcid(&g_glink, remote_cid);
			if (ch) {
				for (uint32_t i = 0; i < ch->num_intents; i++) {
					if (ch->intents[i].iid != liid) {
						continue;
					}
					if (msg.cmd == GLINK_CMD_RX_DONE_W_REUSE) {
						ch->intents[i].in_use = false;
					} else {
						ch->intents[i] = ch->intents[--ch->num_intents];
					}
					break;
				}
			}
			sweet_glink_rx_advance(&g_glink, sizeof(msg));
			break;
		}
		case GLINK_CMD_TX_DATA: {
			struct glink_msg_tx_data tdata;
			if (avail < sizeof(tdata)) return;
			sweet_glink_rx_peek(&g_glink, rx_tail, &tdata, sizeof(tdata));

			uint32_t data_pkt_sz = (sizeof(tdata) + tdata.chunk_size + 7) & ~7;
			if (avail < data_pkt_sz) return;

			static uint8_t payload[4096 + 64];

			memset(payload, 0, 256);
			uint32_t data_off = (rx_tail + (uint32_t)sizeof(tdata)) % g_glink.rx_size;
			sweet_glink_rx_peek(&g_glink, data_off, payload, MIN(tdata.chunk_size, sizeof(payload)));

			struct sweet_glink_channel *ch = sweet_glink_find_by_lcid(&g_glink, tdata.lcid);
			if (ch) ch->rx_count++;

			hakios_say("SWEET GLINK RX DATA: lcid %u, riid %u, chunk %u, left %u\n",
			    tdata.lcid, tdata.riid, tdata.chunk_size, tdata.left_size);
			note("[GLINK DATA l%u sz %u]", tdata.lcid, tdata.chunk_size);
			sweet_glink_rx_advance(&g_glink, data_pkt_sz);

			/* Send RX_DONE_W_REUSE */
			struct glink_msg_rx_done rxdone = {
				.cmd = GLINK_CMD_RX_DONE_W_REUSE,
				.lcid = tdata.lcid,
				.liid = tdata.riid,
			};
			sweet_glink_tx_cmd(&g_glink, &rxdone, sizeof(rxdone));

			/* Check if channel is IPCRTR: handle QRTR packets! */
			if (ch && strncmp(ch->name, "IPCRTR", 6) == 0 && tdata.chunk_size >= sizeof(struct qrtr_hdr_v1)) {
				struct qrtr_hdr_v1 *qhdr = (struct qrtr_hdr_v1 *)payload;
				uint32_t qtype = qhdr->type;
				uint32_t *w = (uint32_t *)payload;
				uint32_t nw = MIN(tdata.chunk_size / 4, 13);
				hakios_say("SWEET QRTR: type %u (sz %u) words:", qtype, tdata.chunk_size);
				for (uint32_t wi = 0; wi < nw; wi++) hakios_say(" %#x", w[wi]);
				hakios_say("\n");

				if (qtype == QRTR_TYPE_HELLO) {
					hakios_say("SWEET QRTR: received HELLO from node %u! Replying with HELLO (src 1)\n", qhdr->src_node_id);
					note("[QRTR HELLO from %u]", qhdr->src_node_id);

					struct {
						struct qrtr_hdr_v1 hdr;
						struct qrtr_ctrl_pkt ctrl;
					} __attribute__((packed)) reply;
					memset(&reply, 0, sizeof(reply));
					reply.hdr.version = 1;
					reply.hdr.type = QRTR_TYPE_HELLO;
					reply.hdr.src_node_id = 1; /* APPS node is 1 */
					reply.hdr.src_port_id = QRTR_PORT_CTRL;
					reply.hdr.size = sizeof(struct qrtr_ctrl_pkt);
					reply.hdr.dst_node_id = qhdr->src_node_id;
					reply.hdr.dst_port_id = QRTR_PORT_CTRL;
					reply.ctrl.cmd = QRTR_TYPE_HELLO;

					sweet_glink_send_data(&g_glink, ch, &reply, sizeof(reply));
					hakios_say("SWEET QRTR: sent HELLO to node %u\n", qhdr->src_node_id);
					note("[QRTR sent HELLO]");

					/* Announce local services */
					sweet_qrtr_announce_server(ch, qhdr->src_node_id, 14, 1, 14);     /* RMTFS */
					sweet_qrtr_announce_server(ch, qhdr->src_node_id, 64, 0x101, 64); /* SERVREG_LOC: version 1 | instance 0 << 8 */
					sweet_qrtr_announce_server(ch, qhdr->src_node_id, 4096, 1, 4096); /* TFTP */
				} else if (qtype == QRTR_TYPE_NEW_SERVER) {
					struct qrtr_ctrl_pkt *cp = (struct qrtr_ctrl_pkt *)(payload + sizeof(*qhdr));
					hakios_say("SWEET QRTR NEW_SERVER: service %u, instance %#x, node %u, port %u\n",
					    cp->server.service, cp->server.instance, cp->server.node, cp->server.port);
					note("[QRTR svc %u inst %#x port %u]",
					    cp->server.service, cp->server.instance, cp->server.port);
					ev("QRTR new server %u inst %#x port %u", cp->server.service, cp->server.instance, cp->server.port);
					if (cp->server.service == WLFW_SERVICE) {
						sweet_wlfw_server(ch, cp->server.node, cp->server.port);
					}
					if (cp->server.service == DMS_SERVICE) {
						sweet_dms_server(cp->server.node, cp->server.port);
					}
				} else if (qtype == QRTR_TYPE_DATA) {
					if (qhdr->confirm_rx) {
						sweet_qrtr_resume_tx(ch, qhdr);
					}
					if (qhdr->dst_port_id == RMTFS_PORT) {
						sweet_rmtfs_handle_qmi(ch, qhdr, payload + sizeof(*qhdr), tdata.chunk_size - (uint32_t)sizeof(*qhdr));
					} else if (qhdr->dst_port_id == WLFW_CLIENT_PORT) {
						sweet_wlfw_handle(ch, qhdr, payload + sizeof(*qhdr), tdata.chunk_size - (uint32_t)sizeof(*qhdr));
					} else if (qhdr->dst_port_id == DMS_CLIENT_PORT) {
						sweet_dms_handle(payload + sizeof(*qhdr), tdata.chunk_size - (uint32_t)sizeof(*qhdr));
					} else if (qhdr->dst_port_id == SERVREG_LOC_PORT) {
						sweet_servreg_loc_handle(ch, qhdr, payload + sizeof(*qhdr), tdata.chunk_size - (uint32_t)sizeof(*qhdr));
					} else if (qhdr->dst_port_id >= TFTP_PORT && qhdr->dst_port_id <= TFTP_PORT + TFTP_XFERS) {
						sweet_tftp_handle(ch, qhdr, payload + sizeof(*qhdr), tdata.chunk_size - (uint32_t)sizeof(*qhdr));
					} else {
						hakios_say("SWEET QRTR DATA: p%u -> p%u sz %u\n",
						    qhdr->src_port_id, qhdr->dst_port_id, qhdr->size);
						note("[QRTR DATA p%u->p%u sz %u]",
						    qhdr->src_port_id, qhdr->dst_port_id, qhdr->size);
					}
				}
			}
			break;
		}
		case GLINK_CMD_RX_INTENT_REQ: {
			/* The remote has no buffer of ours to send into (qcom_glink_handle_intent_req): give it some, and say so. */
			struct sweet_glink_channel *ch = sweet_glink_find_by_rcid(&g_glink, msg.param1);
			static uint32_t next_liid = 16;

			sweet_glink_rx_advance(&g_glink, sizeof(msg));
			if (ch) {
				for (int n = 0; n < 4; n++) {
					struct glink_msg_intent grant = {
						.cmd = GLINK_CMD_INTENT,
						.lcid = ch->lcid,
						.count = 1,
						.size = MAX(msg.param2, 4096u),
						.liid = next_liid++,
					};

					sweet_glink_tx_cmd(&g_glink, &grant, sizeof(grant));
				}
				struct glink_msg ack = { .cmd = GLINK_CMD_RX_INTENT_REQ_ACK, .param1 = ch->lcid, .param2 = 1 };

				sweet_glink_tx_cmd(&g_glink, &ack, sizeof(ack));
				ev("GLINK intent request for %u bytes on '%s': granted 4", msg.param2, ch->name);
			}
			break;
		}
		case GLINK_CMD_RX_INTENT_REQ_ACK: {
			ev("GLINK intent request acknowledged (%u)", msg.param2);
			sweet_glink_rx_advance(&g_glink, sizeof(msg));
			break;
		}
		case GLINK_CMD_SIGNALS: {
			hakios_say("SWEET GLINK RX: SIGNALS rcid %u sigs %#x\n", msg.param1, msg.param2);
			sweet_glink_rx_advance(&g_glink, sizeof(msg));
			break;
		}
		default: {
			hakios_say("SWEET GLINK RX: unhandled cmd %u (p1 %u, p2 %#x)\n", msg.cmd, msg.param1, msg.param2);
			sweet_glink_rx_advance(&g_glink, sizeof(msg));
			break;
		}
		}
	}
}

static bool
sweet_smem_find_part(uint16_t host0, uint16_t host1, uint64_t *out_phys, uint32_t *out_size)
{
	volatile uint8_t *pt_win = (volatile uint8_t *)ml_io_map_unmappable(SMEM_MEMORY + SMEM_SIZE - PAGE_SIZE, PAGE_SIZE, WIMG_WCOMB);
	if (!pt_win) return false;

	struct smem_ptable *ptable = NULL;
	if (memcmp((const void *)(pt_win + (PAGE_SIZE - 4096)), "$TOC", 4) == 0) {
		ptable = (struct smem_ptable *)(pt_win + (PAGE_SIZE - 4096));
	} else if (memcmp((const void *)pt_win, "$TOC", 4) == 0) {
		ptable = (struct smem_ptable *)pt_win;
	}

	if (!ptable) {
		ml_io_unmap((vm_offset_t)pt_win, PAGE_SIZE);
		return false;
	}

	bool found = false;
	for (uint32_t i = 0; i < ptable->num_entries; i++) {
		if (ptable->entry[i].size == 0) continue;
		if ((ptable->entry[i].host0 == host0 && ptable->entry[i].host1 == host1) ||
		    (ptable->entry[i].host0 == host1 && ptable->entry[i].host1 == host0)) {
			if (out_phys) *out_phys = SMEM_MEMORY + ptable->entry[i].offset;
			if (out_size) *out_size = ptable->entry[i].size;
			found = true;
			break;
		}
	}
	ml_io_unmap((vm_offset_t)pt_win, PAGE_SIZE);
	return found;
}

static void
mpss_run(void)
{
	unsigned crash_ticks = 0;

	/* Map SMEM header once to log status */
	volatile uint8_t *hdr_win = (volatile uint8_t *)ml_io_map_unmappable(SMEM_MEMORY, PAGE_SIZE, WIMG_WCOMB);
	if (hdr_win) {
		struct smem_header *hdr = (struct smem_header *)hdr_win;
		hakios_say("SWEET SMEM: header init %u, SBL ver %#x, free %#x, avail %#x\n",
		    hdr->initialized, hdr->version[7], hdr->free_offset, hdr->available);
		note("[smem init %u SBL %#x]", hdr->initialized, hdr->version[7]);
		ml_io_unmap((vm_offset_t)hdr_win, PAGE_SIZE);
	}

	/* Map Partition 7 (APPS <-> MODEM) and Partition 9 (GLOBAL) permanently */
	uint64_t part_phys = 0;
	uint32_t part_size = 0;
	sweet_smem_find_part(0, 1, &part_phys, &part_size);

	uint64_t p9_phys = 0;
	uint32_t p9_size = 0;
	sweet_smem_find_part(0xfffe, 0xfffe, &p9_phys, &p9_size);

	/* Map RMTFS shared memory buffer at 0xfde01000 (phys 0xfde00000 aligned) */
	uint64_t rmtfs_base = RMTFS_BUFF_PHYS & ~(uint64_t)PAGE_MASK;
	uint64_t rmtfs_mapsz = (RMTFS_BUFF_SIZE + 2 * PAGE_SIZE) & ~(uint64_t)PAGE_MASK;
	g_rmtfs_map = (volatile uint8_t *)ml_io_map_unmappable(rmtfs_base, rmtfs_mapsz, WIMG_WCOMB);
	if (g_rmtfs_map) {
		g_rmtfs_buf = g_rmtfs_map + (RMTFS_BUFF_PHYS - rmtfs_base);
		hakios_say("SWEET RMTFS: mapped shared buffer at %p (phys %#llx, sz %#llx)\n",
		    g_rmtfs_buf, RMTFS_BUFF_PHYS, RMTFS_BUFF_SIZE);
		note("[RMTFS buf mapped]");
	} else {
		hakios_say("SWEET RMTFS: failed to map shared buffer!\n");
		note("[RMTFS buf map FAILED]");
	}

	if (part_phys && part_size) {
		uint64_t part_base = part_phys & ~(uint64_t)PAGE_MASK;
		uint64_t map_sz = (part_phys - part_base + part_size + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
		map_sz = MIN(map_sz, (uint64_t)CHUNK);

		volatile uint8_t *part_win = (volatile uint8_t *)ml_io_map_unmappable(part_base, map_sz, WIMG_WCOMB);
		volatile uint8_t *p9_win = NULL;
		struct smem_partition_header *phdr9 = NULL;
		uint64_t p9_mapsz = 0;
		if (p9_phys && p9_size) {
			uint64_t p9_base = p9_phys & ~(uint64_t)PAGE_MASK;
			p9_mapsz = (p9_phys - p9_base + p9_size + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
			p9_mapsz = MIN(p9_mapsz, (uint64_t)CHUNK);
			p9_win = (volatile uint8_t *)ml_io_map_unmappable(p9_base, p9_mapsz, WIMG_WCOMB);
			if (p9_win) phdr9 = (struct smem_partition_header *)(p9_win + (p9_phys - p9_base));
		}

		if (part_win) {
			struct smem_partition_header *phdr = (struct smem_partition_header *)(part_win + (part_phys - part_base));
			hakios_say("SWEET SMEM: mapped Partition 7 at %p (phys %#llx, sz %#x), Part 9 at %p (phys %#llx, sz %#x)\n",
			    phdr, part_phys, part_size, phdr9, p9_phys, p9_size);

			/* Fast poll loop: 50 ms ticks = 20 Hz */
			for (int tick = 1;; tick = (tick >= 1000000) ? 201 : tick + 1) {      /* no end: the modem is polled for as long as the phone runs */
				g_events_second = (uint32_t)(tick / 20);
				for (int sub = 0; sub < 10; sub++) {
					sleep_ms(5);
					if (g_glink.ready && !g_modem_crashed) {
						sweet_glink_poll();
						sweet_tftp_pump();
					}
				}

				if (g_modem_crashed || g_wlan_crashed) {
					if (g_wlan_probe >= 8 && ++crash_ticks >= 40) {
						break;                          /* two seconds for the service thread to notice; then the modem is restarted */
					}
					if (g_modem_crashed) {
						/* the modem's SMEM state is not ours to poke while it restarts (a GLINK send aborted the kernel) */
						continue;
					}
				}

				/* 1. Outbound SMP2P setup */
				if (tick <= 100 || tick % 200 == 0) {
					sweet_smp2p_setup_outbound(phdr, part_size);
				}

				/* 2. Inbound SMP2P status (item 435) */
				sweet_smp2p_poll_inbound(phdr, part_size, tick);

				/* 3. GLINK setup and poll */
				if (!g_glink.ready) {
					sweet_glink_setup(phdr, part_size);
				} else {
					sweet_glink_poll();
				}

				/* 4. Check crash log (item 421 in Part 9) */
				if (phdr9) {
					sweet_smem_check_crash(phdr9, p9_size);
				}

				/* 5. 1 Hz alive banner */
				if (tick % 20 == 0) {
					int sec = tick / 20;
					if (sec <= 30 || sec % 10 == 0) {
						hakios_say("mpss: alive %d s: %s\n", sec, report);
					}
					if (sec == 90) {
						sweet_dms_send();
					}
					/* boot-arg mpss_dump=N: every N seconds of the modem's life, the event ring one event a line (the
					 * alive line above is cut after about 2 KB and the middle of a long run's log is dropped) */
					if (g_ev_dump != 0 && sec % (int)g_ev_dump == 0) {
						static char copy[sizeof(g_events)];
						char *line = copy, *end;

						memcpy(copy, g_events, g_events_length);
						copy[g_events_length] = 0;
						hakios_say("mpss-ring: at %d s, %zu bytes of events\n", sec, g_events_length);
						while (*line != 0) {
							end = strchr(line, '\n');
							if (end != NULL) {
								*end = 0;
							}
							if (strncmp(line, "GLINK DATA", 10) != 0 && strnstr(line, "A80211: scan", 64) == NULL) {
								hakios_say("mpss-ring: %s\n", line);
							}
							if (end == NULL) {
								break;
							}
							line = end + 1;
						}
					}
				}
			}
			ml_io_unmap((vm_offset_t)part_win, map_sz);
			if (p9_win) ml_io_unmap((vm_offset_t)p9_win, p9_mapsz);
			if (g_rmtfs_map) ml_io_unmap((vm_offset_t)g_rmtfs_map, rmtfs_mapsz);
		}
	} else {
		hakios_say("SWEET SMEM: Partition 7 (hosts 0:1) not found!\n");
	}
}

/* everything the host remembers about the running modem, for the next boot */
static void
mpss_restart_cleanup(void)
{
	g_wlan_halt = true;
	for (unsigned t = 0; t < 8000 && g_wlan_thread_running; t += 10) {
		sleep_ms(10);
	}
	memset(&g_glink, 0, sizeof(g_glink));
	g_tftp_ch = NULL;
	memset(g_rmtfs_callers, 0, sizeof(g_rmtfs_callers));
	memset(g_tftp, 0, sizeof(g_tftp));
	g_big_pending = g_pump_clock = g_last_req_clock = 0;
	g_send_failure = 0;
	memset(&g_wlfw, 0, offsetof(typeof(g_wlfw), out));
	g_wlfw.state = WL_IDLE;
	g_last_crash[0] = 0;
	g_crash_seen = false;
	g_modem_crashed = false;
	g_wlan_crashed = false;
	g_events_length = 0;                    /* the 16 KB log was full: keep the story of this restart */
	g_events[0] = 0;
	if (g_wlan_probe >= 8) {
		if (g_wlan_thread_running) {
			ev("EW: service thread still running: rails left on");      /* its next register access would fault with the rails off */
		} else {
			wlan_rails_cycle();
		}
	}
}

static void
mpss_thread(void *arg, wait_result_t result)
{
#pragma unused(arg, result)
	for (;;) {
		if (g_wlan_probe >= 8) {
			/* before the modem boots, as ath10k_snoc enables its "qdss" clock before the firmware runs */
			(void)aop_send("{class: clock, res: qdss, val: 1}");
		}
		if (!mpss_start()) {
			hakios_say("mpss: failed to start\n");
			break;
		}
		mpss_run();
		if (g_wlan_probe < 8) {
			break;
		}
		ev("MPSS: the modem crashed: restarting it (PAS shutdown, load, start)");
		hakios_say("mpss: restarting the modem after a crash\n");
		mpss_restart_cleanup();
	}
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static void
sweet_mpss_status(char *buf, size_t buflen)
{
	size_t len = 0;
	#define BPRINTF(...) do { \
		if (len < buflen) { \
			len += (size_t)snprintf(buf + len, buflen - len, __VA_ARGS__); \
		} \
	} while (0)

	BPRINTF("mpss: %s\n", report);
	if (g_glink.ready) {
		BPRINTF("GLINK: v_sent=%d v_ack_rx=%d v_ack_tx=%d | tx_head=%u tx_tail=%u rx_head=%u rx_tail=%u\n",
		    g_glink.version_sent, g_glink.version_ack_received, g_glink.version_ack_sent,
		    g_glink.descs[1], g_glink.descs[0], g_glink.descs[3], g_glink.descs[2]);
		BPRINTF("GLINK channels (%u):\n", g_glink.num_channels);
		for (uint32_t i = 0; i < g_glink.num_channels; i++) {
			struct sweet_glink_channel *ch = &g_glink.channels[i];
			BPRINTF("  [%u] '%s': lcid=%u rcid=%u open=%d ropen=%d rx=%u tx=%u\n",
			    i, ch->name, ch->lcid, ch->rcid, ch->open, ch->remote_open, ch->rx_count, ch->tx_count);
		}
	} else {
		BPRINTF("GLINK: not ready\n");
	}
	#undef BPRINTF
}

static int
sysctl_mpss_report SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	static char live_buf[2048];
	sweet_mpss_status(live_buf, sizeof(live_buf));
	return SYSCTL_OUT(req, live_buf, strlen(live_buf) + 1);
}

SYSCTL_PROC(_kern, OID_AUTO, sweet_mpss, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sysctl_mpss_report, "A", "MPSS boot and SMEM report");

static int
sysctl_mpss_events SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	return SYSCTL_OUT(req, g_events, g_events_length + 1);
}

SYSCTL_PROC(_kern, OID_AUTO, sweet_mpss_events, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sysctl_mpss_events, "A", "MPSS events with times");

void
hakios_mpss_sm7150_start(void)
{
	static bool started;
	uint32_t enable = 0;
	thread_t thread;

	if (started || !PE_parse_boot_argn("mpss", &enable, sizeof(enable)) || enable == 0) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_WIFI_WCN3990)) {
		hakios_say("mpss: mpss=%u, but this board's board.dts does not list wifi-wcn3990: nothing started\n", enable);
		return;
	}
	/*
	 * en0 and its registry entries now, the link down, before launchd: configd's interface namer and wifid look for the
	 * interfaces once, as they start, and the radio comes up a minute and a half later. Made only then (as on 16.5),
	 * configd never took en0 up nor ran DHCP on it after a join (0 frames sent, no SIOCSIFFLAGS). The same as the
	 * Galaxy S7 port's herolte_wifi_early() and apollo's (wardevsa/ios-a5xelte).
	 */
	ew_if_create();
	started = true;
	PE_parse_boot_argn("wlanprobe", &g_wlan_probe, sizeof(g_wlan_probe));
	PE_parse_boot_argn("mpss_log", &g_ev_log, sizeof(g_ev_log));
	PE_parse_boot_argn("wifi_ps", &g_wifi_ps, sizeof(g_wifi_ps));
	PE_parse_boot_argn("modem_lpm", &g_modem_lpm, sizeof(g_modem_lpm));
	PE_parse_boot_argn("wlan_kdhcp", &g_kdhcp, sizeof(g_kdhcp));
	PE_parse_boot_argn("mpss_dump", &g_ev_dump, sizeof(g_ev_dump));
	PE_parse_boot_argn("wlannoht", &swu_no_ht, sizeof(swu_no_ht));
	if (kernel_thread_start_priority(mpss_thread, NULL, SWEET_WLAN_PRI, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		hakios_say("mpss: kernel_thread_start failed\n");
	}
}

/* (a thread cannot be started this early in the kernel's start: from a thread call, a little later) */
static thread_call_t hakios_mpss_call;

static void
hakios_mpss_sm7150_go(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	vm_offset_t mem = 0;
	uint32_t enable = 0;

	/*
	 * The host memory the Wi-Fi firmware works in (WP_POOL, 16 MB, physically contiguous) is taken now, 2 s into the kernel: asked
	 * for later, when the modem has loaded (about 100 s in, with iOS's user space running) there was no run of free pages that
	 * long and the driver gave up with "WMI: no contiguous pool". wp_rings() keeps a pool that is there already.
	 */
	if (PE_parse_boot_argn("mpss", &enable, sizeof(enable)) && enable != 0 && hakios_board_has(HAKIOS_HAS_WIFI_WCN3990)) {
		if (kmem_alloc_contig(kernel_map, &mem, WP_POOL, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO | KMA_PERMANENT,
		    VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
			wp.pool = (uint8_t *)mem;
			wp.pa = kvtophys(mem);
			hakios_say("mpss: the Wi-Fi's host memory (%u MB) kept early, at pa %#llx\n", WP_POOL >> 20, wp.pa);
		} else {
			hakios_say("mpss: no %u MB of contiguous memory even this early\n", WP_POOL >> 20);
		}
	}
	hakios_mpss_sm7150_start();
}

extern uint64_t hakios_normal_nc_lo, hakios_normal_nc_hi;     /* osfmk/arm/pmap/pmap.c (kernel/patches/0002) */

__startup_func
static void
hakios_mpss_sm7150_init(void)
{
	uint64_t deadline;

	/*
	 * Under xnu-12377 a write-combined mapping (ml_io_map_unmappable(..., VM_WIMG_WCOMB)) is Normal non-cacheable only
	 * for the kernel's own RAM and the range [hakios_normal_nc_lo, hakios_normal_nc_hi); anything else becomes Device-GRE,
	 * where bzero's DC ZVA and memcpy's unaligned accesses fault ("Unaligned kernel data abort" in bzero, from
	 * sweet_smem_alloc_entry, right after the modem started). What this driver maps write-combined is all DRAM below
	 * 4 GiB -- SMEM 0x86000000, the modem's image 0x8b000000, rmtfs, the loader's lowdma= -- and DRAM is never Device.
	 */
	if (hakios_board_has(HAKIOS_HAS_WIFI_WCN3990) && hakios_normal_nc_hi == 0) {
		hakios_normal_nc_lo = 0x80000000ull;
		hakios_normal_nc_hi = 0x100000000ull;
	}

	hakios_mpss_call = thread_call_allocate(hakios_mpss_sm7150_go, NULL);
	clock_interval_to_deadline(2, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(hakios_mpss_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_mpss_sm7150_init);
