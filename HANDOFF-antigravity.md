# Handoff: iOS 16.5 on the Pixel 4a (sunfish), HakiOS kernel

Written 2026-11-08 for a fresh agent session (Antigravity or anything else). Read this, then `NOTES.md`, then `AGENTS.md`
in this repo. Treat everything marked UNVERIFIED as a guess. Do not trust this file over a command you can run.

## Who and what

- The user owns a Pixel 4a ("sunfish", SM7150, Adreno 618). Goal: iOS 16.5 (20F66, Apple's own volume) living on the phone,
  booting without a PC, usable. Android (EvolutionX 17.0) stays on the phone in slot b.
- Two trees: `~/hakios` is where ALL new work goes (HakiOS kernel, loader, userland, U-Boot). `~/ios-sunfish` is this
  notes/scratch repo (NOTES.md is the long-lived handoff; keep it short: milestones, decisions, traps).
- The user wants you to work autonomously: do not ask unless a decision is truly theirs. Exceptions (always ask first):
  anything destructive (flash writes, partition changes), pushing to git, anything touching the phone while they say not to.
- The user said, at the end of this session: "dont do anything on the phone yet". Ask before ANY fastboot/adb/boot action.

## Hard rules

- Never put serial numbers, Wi-Fi names, MACs or passwords in tracked files (git-ignored places only). No personal data
  remembered by the OS image either.
- Never write modemst1/2, fsg, persist, bootloaders, recovery. Keep the backups under `~/ios-sunfish/scratch/` (git-ignored).
- Android = slot b. iOS = slot a (`boot_a`). NEVER `fastboot set_active b` except what `devices/sunfish/run.sh` already does
  (it refills the boot tries of slot b before each `fastboot boot`, because each `fastboot boot` spends one try of the active slot).
- `set_active` changes flags AND GUIDs AND the UFS boot LUN together. Only ABL/bootctl should change the active slot.
  U-Boot may only mark the active slot `successful` or `unbootable` (`hakios slot ...`).
- `boot_a` flashing is done rarely (flash wear) and only with the user's go-ahead; it is done from Android through the
  KernelSU module (see below). A permission classifier once blocked my flash command: the user ran it himself.
- Commit with explicit pathspecs (`git commit -- path ...`): a peer session may stage other files. gpg times out when the
  user is away, so commits are unsigned. Never push without being told. Commit trailers (end of message):
  `Co-Authored-By: <model> <noreply@anthropic.com>` and a `Claude-Session:` line if the harness gives one.
- No subagents unless asked. Do not turn off random services ("lets not turn off random services").
- Update NOTES.md after each milestone, short. Distinguish observed from guessed.

## How the phone boots (what exists)

ABL -> U-Boot (storage variant, `u-boot/build-sunfish_storage/u-boot.bin`) -> xnuloader (`xnuloader/loader.efi`) -> XNU
(xnu-8796.121.2, Darwin 22.5 = iOS 16.5). Root volume + OS/App cryptexes live in the 20 GiB `darwin` partition (sda17,
cut from userdata); a 4 MB `flashboot.apfs` (boot volume) rides inside the boot image as the loader's resident image.
Boot args: `root_apfs=ufs_darwin ufs=2` + the shared lists in `devices/sunfish/boot-args.sh`.

Self-contained image: `devices/sunfish/mkinstall.sh` -> `build/ios-boot.img` (U-Boot + loader + payload with kernel,
root.apfs, flashboot.apfs). It is written to `boot_a` from Android with the KernelSU module
(`devices/sunfish/ksu-module`, installed; installed script: `/data/adb/modules/hakios_ios/system/bin/ios-boot`; slot choice
file `/data/adb/hakios_ios_slot` = `a`). Module's backup dir `/sdcard/Download/...` DOES NOT EXIST on this phone: run a
copy of the script with the backup dir changed to `/data/local/tmp/ios-boot-backups`
(`sed s#/sdcard/Download/ios-boot-backups#/data/local/tmp/ios-boot-backups#`), `echo yes | sh ib.sh install IMG`. It backs
up boot_a first and verifies the read-back. Backups from the PC side: `~/ios-sunfish/scratch/boot-backups/`.
(Fix the module's path: UNDONE.)

Test builds without flashing: `fastboot boot build/ios-boot.img` (standalone, no log: debug with run.sh instead).

## Daily workflow

From `~/hakios`:
1. Build userland + flash-root image: `devices/sunfish/build-flash.sh` (use `RC_JOBS=rc.jobs.X` for a test job list;
   default `rc.jobs.daily`; env `METAL=1` for the Metal path). Needs ldid on PATH (`~/theos/toolchain/linux/iphone/bin`).
2. Kernel: `make kernel` (exit status 2 from kext symbol-set checks is normal on this config). Kernel sources are
   edited in `xnu/files/...` AND copied into `xnu-src/xnu-8796.121.2/...` (the build tree); re-export with
   `xnu/export.sh` when needed. osfmk code cannot include BSD headers (put sysctls in bsd files, see apfs_vfsops.c).
3. Loader: `make loader`. U-Boot: edit `uefi/u-boot/files/...` and `devices/sunfish/u-boot/files/...` (both git-ignored in
   ~/hakios!) then copy into `u-boot/` and run `uefi/u-boot/build.sh sunfish_storage` (CROSS_COMPILE=aarch64-linux-gnu-).
4. Run from the PC: `FLASH=1 devices/sunfish/run.sh "" SECONDS` (resets the phone to its bootloader via PMIC reason, ~4 s;
   boots a USB-mode loader, uploads, runs; log in `logs/<time>-run.log`). Helpers: `./xl run|log|screenshot FILE|stop|state`,
   `rwtouch` (on-device, injected touches/buttons), `vol_ls` (ls/cat/sysctl inside iOS). Warn the user: a run resets the phone.
5. A run is ~5 min. The kernel log keeps only the first 256 KB and the last 1 MB (the middle is dropped): important lines
   must go through the loader log or be printed at the end by a job.

## State at the end of this session (all observed unless marked)

Working: boot from flash, touch (intermittent failure fixed), Wi-Fi spinner fix, power-off from the iOS UI,
standalone boot from `boot_a`, GPU compositor via Metal plug-in (`METAL=1`), Safari image-hold crash stubbed (hook in
libhakios.ci.dylib), Android f2fs shrunk to fit, loader `fastboot reboot` reaches the stock bootloader via PMIC reason.
Boot Switch app (Antigravity wrote it, `devices/sunfish/apps/BootSwitch`): registers with LaunchServices through
`bootd` (~30 s after SpringBoard), opens, "Reboot" works. Committed in ~/hakios `d640c0a`.

IN PROGRESS: the "Restart into Android" button. Pieces:
- bootd (`userland/src/bootd.m`) listens for notification `com.hakios.next-boot-android`, sets sysctl
  `kern.hakios_next_boot=1` (defined in `bsd/miscfs/apfs/apfs_vfsops.c`, variable `hakios_next_boot` in
  `osfmk/arm64/loader_return.c`), then `reboot(0)`.
- The kernel reports a restart as loader code 3 when that sysctl is 1 (OBSERVED in a PC run: "halt code 3").
- Loader: on code 3 in a standalone boot it must tell U-Boot. FIRST design = a marker word in IMEM 0x146aa680 + cold reset.
  OBSERVED FAILURE: after the reset the word read `0x90ef2e92` (garbage), so IMEM does not survive. Commit `58d4c34` in
  ~/hakios still has this design and was FLASHED into boot_a (so the flashed "Restart into Android" cannot work).
- SECOND design (UNCOMMITTED, UNTESTED in ~/hakios working tree; `build/ios-boot.img` currently contains it): no reset;
  the loader writes `0x41444e52` ("ANDR") into the first word of the payload at 0xF0000000, pets the watchdog and
  returns to U-Boot; U-Boot env `bootcmd=scsi scan; if hakios load; then hakios boot; fi; if hakios next; then hakios slot
  unbootable; hakios reboot; fi; run hakios_fastboot`; `hakios next` (in `uefi/u-boot/files/cmd/hakios.c`) reads that word,
  clears it, prints and succeeds. Then `hakios slot unbootable` marks the ACTIVE slot (a) unbootable (it refuses unless
  the other slot is `successful`; slot b is) and ABL makes slot b active on the next start.
- The last test was started with `fastboot set_active a` + `fastboot boot build/ios-boot.img` with a job
  (`rc.jobs.halttest`, `halt_test android 130`) that sets the sysctl and restarts at 130 s. The result was NOT seen (the
  user rebooted the PC). The phone may now have slot a flagged unbootable. UNVERIFIED. Check with
  `fastboot getvar slot-unbootable:a` (etc.), repair by `ios-boot` in Android (`bootctl set-active-boot-slot 0` resets
  the flags) or `fastboot set_active b` for tests.
- Risks to check on the second design: U-Boot drawing text over the framebuffer Darwin owns (the user sees "about 5
  pixels of iOS during the log spam"), UFS usability in U-Boot after the kernel ran, watchdog (30 s) during U-Boot's part.

OPEN BUG the user reported twice: after tapping "Restart into Android" the screen "prints logs and ls output until I kill
it" (text like `ls -l`, logs, "some other stuff"; a bit of the iOS picture visible at the top). With the first design the
phone never reset (slot a retry count stayed at 6, slot flags unchanged). The `ls -l`-looking text matches init's
self-test directory listing (`userland/src/init.c`, `list_tree`) and the loader/U-Boot logs, i.e. most likely the phone
is REBOOTING in a loop into the PC-less boot, with text mode on screen, instead of leaving. UNVERIFIED. Things to check:
what boots after the restart; whether `hakios_next_boot` is set again by something; U-Boot/loader print paths
(`screen_quiet` in `xnuloader/loader.c`; the loader keeps the screen quiet after a halt/restart in standalone mode:
commit `7ef1bf2`, flashed in boot_a).

Other pending items:
- A consolidated `boot_a` rewrite after the Android-restart works (needs the user's go-ahead; each flash is wear).
- Fix the KSU module's backup path.
- `NOTES.md`: add the IMEM finding and the RAM-marker design once settled.
- Skip the ~85 s first-boot data migration (com.apple.datamigrator runs every boot because `/private/var` is RAM-backed);
  /private/var persistence was declined for now ("nah not now").
- Parked: DRV2624 vibrator (GPIO11 write hangs), Metal for apps (upstream HakiOS will add QCOM/MTK Metal; "dont lose tokens
  on fixing Metal"), sound/Bluetooth, iOS 18/26 (needs non-PAC rewriting; the Pixel's cores have no PAC; the Xiaomi 15
  repo `superboyyy/hakios-arm64e-dada` does iOS 26 natively on Oryon with hardware PAC, kernel patch for xnu-12377).
- The user also has a Xiaomi 15 (dada) on wireless adb: it is NOT the Pixel; do not touch it.

## Update (end of session): both boot slots were overwritten

The user's own Android build/flash work wiped boot_a AND boot_b (reported 2026-11-08). So boot_a no longer holds the iOS
image and boot_b no longer holds a working Android boot image until the user reflashes it. Nothing was done to the phone
by this session after that. Recovery material (PC side, git-ignored): `~/ios-sunfish/scratch/boot-backups/` (boot_a
before the first iOS flash, and an iOS image backup at `/data/local/tmp/ios-boot-backups/` on the phone, which may be gone),
`~/hakios/build/ios-boot.img` (the RAM-marker iOS image, untested), rebuild with `devices/sunfish/mkinstall.sh`.
Ask the user what state the phone is in before doing anything; `fastboot boot` of a test image works without any boot slot.

## Phone state conventions

- Android's slot b is the normal one; `adb` serial of the Pixel is in the user's shell (not recorded here on purpose).
  When two adb transports exist use `adb -s SERIAL`.
- To get the phone back to Android after a PC run: `fastboot reboot` from the loader goes to the bootloader (~4 s), then
  `fastboot reboot` again boots Android (slot b active). Warn the user first.
- `sudo` works plainly (`sudo -n` lies). `ipsw` is allowed.

## First things to do in the new session

1. Read `NOTES.md` top block, `AGENTS.md`, this file, then `git -C ~/hakios status` and `git -C ~/hakios log -5`.
2. Ask the user whether the phone may be used again (they said not to touch it).
3. When allowed: read the slot flags, restore them, then decide whether the RAM-marker design works by repeating the
   `fastboot boot` test with a screen-watching user, and fix the log spam. Commit working pieces with pathspecs.
