/*
 * A command-mode panel's frames. A DSI command-mode panel (the Pixel 4a's) takes a frame only when it is sent one,
 * where a video-mode panel is scanned out all the time: the framebuffer Darwin draws into is the memory the display
 * engine reads, so without this the screen stays on the last frame the boot loader sent. This turns on the
 * autorefresh of each pingpong block that does tear checks: the engine then sends the frame at every tear-effect
 * pulse of the panel.
 *
 *   panel_autorefresh=1    autorefresh on, at every pulse, for each pingpong whose tear check is on
 *   panel_autorefresh=-1   a look: each pingpong's tear check and autorefresh read and said, nothing written
 *
 * Where the blocks are is the SoC file's display { pingpong } (PINGPONG_0; the others 0x800 apart). Registers, from
 * Linux's DPU driver (dpu_hw_pingpong.c): PP_TEAR_CHECK_EN +0x000 (bit 0), PP_AUTOREFRESH_CONFIG +0x030 (bit 31 on,
 * the low 16 bits the pulses between frames).
 *
 * The display engine has to be on and clocked when this runs, or touching it can stop the phone: the boot loader
 * leaves it so on a phone that shows its own text. No check here can tell in advance; hence the boot argument.
 * Nothing is undone on the way back: the loader draws on the same panel afterwards and needs the frames too.
 *
 * From the Pixel 4a's Darwin 16.5 port (devices/sunfish/drivers/gpu/hakios.cmdpanel.c), where it ran on the phone.
 */
#include <kern/startup.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "board.h"
#include "log.h"
#include "mmio.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define PP_BLOCKS               4u
#define PP_STRIDE               0x800u
#define PP_TEAR_CHECK_EN        0x000u
#define PP_AUTOREFRESH          0x030u
#define AUTOREFRESH_ENABLE      0x80000000u
#define AUTOREFRESH_ON          (AUTOREFRESH_ENABLE | 1u)       /* every pulse */

void hakios_cmdpanel_init(void);

void
hakios_cmdpanel_init(void)
{
	int32_t level = 0;
	const struct hakios_board *board;
	vm_offset_t pp;
	unsigned found = 0;

	if (!PE_parse_boot_argn("panel_autorefresh", &level, sizeof(level)) || level == 0) {
		return;
	}
	board = hakios_board();
	if (board == NULL || board->display.pingpong == 0) {
		hakios_say("cmdpanel: no display { pingpong } in this board's SoC file: nothing done\n");
		return;
	}
	pp = ml_io_map(board->display.pingpong, PP_BLOCKS * PP_STRIDE);
	if (pp == 0) {
		hakios_say("cmdpanel: the pingpong blocks at %#x could not be mapped\n", board->display.pingpong);
		return;
	}
	for (unsigned n = 0; n < PP_BLOCKS; n++) {
		vm_offset_t block = pp + n * PP_STRIDE;
		uint32_t was, now;

		if (!(rd(block + PP_TEAR_CHECK_EN) & 1u)) {
			continue;       /* not driving a command-mode panel */
		}
		found++;
		was = rd(block + PP_AUTOREFRESH);
		if (level > 0 && !(was & AUTOREFRESH_ENABLE)) {
			wr(block + PP_AUTOREFRESH, AUTOREFRESH_ON);
			__asm__ volatile ("dsb sy" ::: "memory");
		}
		now = rd(block + PP_AUTOREFRESH);
		hakios_say("cmdpanel: pingpong %u: autorefresh %#x -> %#x%s\n", n, was, now,
		    level < 0 ? " (a look)" : (now & AUTOREFRESH_ENABLE) ? "" : " (DID NOT TAKE: the screen will not follow)");
	}
	if (found == 0) {
		hakios_say("cmdpanel: no pingpong does tear checks: the panel was not left in command mode, nothing done\n");
	}
}

/* As early as the device tree is there: the console's first lines are on the screen only once this has run. */
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_cmdpanel_init);
