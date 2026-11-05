/*
 * The Pixel 4a panel's brightness: DCS 0x51 with a 10-bit level, high byte first, as a long write through DSI0's command
 * FIFO (no memory, no SMMU), as Linux's dsi_ctrl_hw_cmn_kickoff_fifo_command does. No PWM or WLED on this phone.
 *
 * Only with boot-arg panel_brightness=N (4..1023): N a few seconds after the kernel starts. Then the slider in iOS
 * (hakios.cmdbright.cpp) and kern.hakios.panel_brightness set it. Nothing is sent unless DSI0 is on in command mode.
 *
 * Writes to no PMIC: only DSI0's registers, and to the panel only this one command.
 *
 * Where it comes from: sweet_fb_set_brightness() in SweetFramebuffer.cpp of ios-sunfish, which ran on this phone from
 * ABL (2026-10-28), and docs/SUNFISH-BACKLIGHT.md there. Status: RUN ON THE PHONE through U-Boot, iOS 16.5 (2026-11-05): the user
 * saw it dim during boot.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void HakiosBacklightStart(int level);
extern void delay(int usec);

#define DSI0_BASE       0x0AE94000UL
#define DSI_CTRL        0x004
#define DSI_DMA_CTRL    0x03C
#define DSI_DMA_LENGTH  0x04C
#define DSI_DMA_TRIGGER 0x090
#define DSI_INT_CTRL    0x110
#define DSI_INT_KEEP    0xA222AA02u     /* the enables and the error mask, no other status bit */
#define DSI_TPG_CTRL    0x15C
#define DSI_TPG_DMA_INIT 0x17C
#define DSI_TPG_FIFO_RST 0x1EC

#define LEVEL_MIN       4
#define LEVEL_MAX       1023
#define START_SECONDS   3

static LCK_GRP_DECLARE(cmdbright_group, "hakios.cmdbright");
static LCK_MTX_DECLARE(cmdbright_lock, &cmdbright_group);
static vm_offset_t dsi;
static int first_level, level_now = -1;
static thread_call_t start_call;

int hakios_panel_brightness(int level);

int
hakios_panel_brightness(int level)
{
	volatile uint32_t *r = (volatile uint32_t *)dsi;
	uint32_t ctrl, dma;
	bool done = false;

	if (dsi == 0) {
		return ENODEV;
	}
	level = level < LEVEL_MIN ? LEVEL_MIN : level > LEVEL_MAX ? LEVEL_MAX : level;
	lck_mtx_lock(&cmdbright_lock);
	ctrl = r[DSI_CTRL / 4];
	if ((ctrl & 0x5u) != 0x5u) {
		lck_mtx_unlock(&cmdbright_lock);
		printf("cmdbright: DSI0 is not on in command mode (DSI_CTRL %#x): nothing sent\n", ctrl);
		return ENXIO;
	}
	r[DSI_INT_CTRL / 4] = (r[DSI_INT_CTRL / 4] & DSI_INT_KEEP) | 1u;
	r[DSI_TPG_CTRL / 4] = 0x00030006u;
	r[DSI_TPG_DMA_INIT / 4] = 0xC0390003u;  /* long write, DT 0x39, 3 bytes, last command */
	r[DSI_TPG_DMA_INIT / 4] = 0xFF000051u | ((uint32_t)level & 0xffu) << 16 | ((uint32_t)level >> 8) << 8;
	dma = r[DSI_DMA_CTRL / 4];
	r[DSI_DMA_CTRL / 4] = (dma & ~(1u << 31 | 1u << 30 | 1u << 26)) | 1u << 28;
	r[DSI_DMA_LENGTH / 4] = 8;
	__asm__ volatile("dsb sy" ::: "memory");
	r[DSI_DMA_TRIGGER / 4] = 1;
	for (unsigned i = 0; i < 500 && !done; i++) {
		done = (r[DSI_INT_CTRL / 4] & 1u) != 0;
		if (!done) {
			delay(100);
		}
	}
	r[DSI_INT_CTRL / 4] = (r[DSI_INT_CTRL / 4] & DSI_INT_KEEP) | 1u;
	r[DSI_TPG_CTRL / 4] = 0;
	r[DSI_TPG_FIFO_RST / 4] = 1;
	delay(1);
	r[DSI_TPG_FIFO_RST / 4] = 0;
	r[DSI_DMA_CTRL / 4] = dma;
	__asm__ volatile("dsb sy" ::: "memory");
	if (done) {
		level_now = level;
	}
	lck_mtx_unlock(&cmdbright_lock);
	printf("cmdbright: level %d %s\n", level, done ? "sent" : "NOT acknowledged (50 ms)");
	return done ? 0 : ETIMEDOUT;
}

static int
sysctl_panel_brightness(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = level_now, error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	return hakios_panel_brightness(value);
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, panel_brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    sysctl_panel_brightness, "I", "the panel's level, 4..1023; -1: none sent yet");

static void
start_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	dsi = ml_io_map(DSI0_BASE, 0x1000);
	if (dsi == 0) {
		printf("cmdbright: could not map DSI0\n");
		return;
	}
	(void)hakios_panel_brightness(first_level);
	HakiosBacklightStart(first_level);
}

__startup_func
static void
cmdbright_init(void)
{
	uint64_t deadline;

	if (!PE_parse_boot_argn("panel_brightness", &first_level, sizeof(first_level)) || first_level <= 0) {
		return;
	}
	start_call = thread_call_allocate(start_run, NULL);
	clock_interval_to_deadline(START_SECONDS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(start_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, cmdbright_init);
