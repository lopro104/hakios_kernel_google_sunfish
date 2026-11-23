# HakiOS kernel

Apple's XNU kernel (`xnu-12377.121.6`, the kernel of Darwin 26 and 27) with what it takes to start Darwin on
Qualcomm phones and in QEMU: our loader, our drivers, and stand-ins for Apple's closed parts.

Nothing of Apple's is in this repository: no firmware, no IPSW, no system images, no screenshots. They are made
on your machine from files you have.

## This branch: the Google Pixel 4a (sunfish, SM7150), iOS 27.2

Branch `sunfish-27` of lopro104/hakios_kernel_google_sunfish is upstream's `common` with the Pixel 4a's drivers.
The board package is in lopro104/hakios_device_google_sunfish (branch `sunfish-27`, directory `darwin26/`), with
how to boot it. On the phone, iOS 27.2 (24B119), November 2026:

| What | State |
| --- | --- |
| Boot | From RAM (`fastboot boot`, nothing flashed); the system volume is an Android DSU image (`ufs=2`, `init/stage0dsu.c`). Lock screen about 80 s after the kernel starts. |
| CPU | All 8 cores, OSM frequency scaling (`cpufreq=1`). |
| Display | The command-mode panel through `panel_autorefresh=1`; brightness from Control Center and Settings (`backlight=2`). |
| Touch, buttons, battery | FTM5 touch screen, power and volume keys, battery level from the PM6150. |
| Wi-Fi | The modem and the WCN3990: scan, WPA2 join from Settings, DHCP, Safari, SSH. Stopping the modem hangs (`mpss_no_shutdown=1`). |
| GPU | The Adreno 618 and the Metal plug-in built `CHIP=618`: backboardd draws with Metal, Liquid Glass included (`gpu=1 metal=1 metal_procs=backboardd`). SpringBoard and apps still draw in software. |
| Storage | `/private/var` kept between boots in the DSU's own data image (`ufs_write=2 var_store=1 var_store_dev=dsudata`): saved every 5 minutes, unpacked at boot. |
| Not yet | Flashlight, sound, camera, cellular, Bluetooth, the system on the phone's own partition. |

The drivers are listed in [Documentation/drivers.md](Documentation/drivers.md#the-google-pixel-4as-own-sunfish-sm7150).

## Where things are

| Directory | What |
| --- | --- |
| `arch/arm64/` | The machine: `boot/` (the loader and the UEFI port), `boot/dts/` (the boards), `kernel/`, `include/`, `configs/` (defconfigs), `tools/` |
| `drivers/` | The drivers (`board/`) and the IOKit stand-ins (`iokit/`) |
| `include/` | The headers the drivers share with user space |
| `fs/` | Our APFS, read and written, and the file system in memory |
| `crypto/`, `security/` | The kernel's cryptography provider; the AMFI stand-in |
| `init/`, `lib/` | The first programs Darwin runs, and their small C library |
| `components/` | What makes a part of Darwin work: Metal, Apple ID, the keystore, camera, sound, brightness, wallpaper, status bar, tweaks |
| `kernel/` | Apple's release, the patches to it, and the tools that assemble, check and build the kernel |
| `darwin/` | Darwin 26/27 user space: the image and library builders |
| `tools/`, `workbench/` | The tools; the tests that need no phone |
| `scripts/` | What you run by hand: set a machine up (`setup-ubuntu.sh`), release the sources (`release.sh`) |
| `samples/` | Starting points to copy: a phone's own repository, a defconfig of your own |
| `MAINTAINERS` | Each part of the project: its files, its tests, its documents (`scripts/tests-for.py FILE` says what to run) |
| `Documentation/` | Everything else ([layout.md](Documentation/layout.md) has the whole map) |

## Where it is

Said plainly, each with where it is recorded; "seen on a phone", "runs in QEMU" and "passes the host tests" are three
different statements.

| What | State |
| --- | --- |
| Darwin 27.2 (24B119, the iPhone12,1's files) | The home screen on a Snapdragon 780G phone, from RAM, drawn in software (`gpu=0`); in QEMU the lock screen. [darwin26-boot.md](Documentation/darwin26-boot.md) |
| Darwin 26.6.2 (23G90) | The home screen on the same phone, first on 31 October 2026. Same document. |
| Each driver | What it has been seen to do, and how tied it is to one phone: [drivers.md](Documentation/drivers.md); what a phone can turn on next: `tools/boards.py status NAME`. |
| New, not yet seen on a phone | Bluetooth for Darwin (`bt=2`: the controller's firmware, bluetoothd's service, channel and control), the kernel's own UFS driver in `phone_perf`, one image for a phone ([images.md](Documentation/images.md)). Held by the host tests and, where it can be, by QEMU. |
| Not there | What Darwin draws with the GPU -- applications' icons, Liquid Glass -- while the GPU is off ([metal-glass.md](Documentation/metal-glass.md)). |

A day with a phone, the runs in order and what each must say: [Documentation/phone-test.ru.md](Documentation/phone-test.ru.md).

## How do I build it

1. The kernel: `kernel/build.sh` (or `make kernel-check`, with LLVM's tools). It makes the tree beside the repository
   and builds `BUILD/obj/mach.development.bcm2837` in it. `build.config.arm64` says what kind of kernel.
2. The loader: `arch/arm64/boot/build.sh`.
3. Darwin's images: `darwin/userland/root26-darwin.sh` after `darwin/userland/inj26.sh` (`build.config.darwin27`).

What each needs: [Documentation/building.md](Documentation/building.md).

## How do I run it

* In QEMU: `make qemu` (the kernel booted as a phone boots it, its shell and its tests), or
  `tools/qemu-kernel.sh [--defconfig NAME] KERNEL`.
* On a phone: `./bench enter`, `./bench ramdisk IMAGE`, then `BENCH_DEFCONFIG=NAME ./bench run`, NAME going
  `phone_first`, `phone_look`, `phone_metal`, `phone_ufs_check`, `phone_perf` on a phone's first day.
  `tools/defconfig.py --list` names the defconfigs.

The Redwood device package and its U-Boot/Mu-Silicium port are kept in the separate `poco-x5-pro-device` and
`poco-x5-pro-uefi` repositories beside this checkout; the kernel stays device-generic.

A new phone: [Documentation/porting.md](Documentation/porting.md). A phone kept in a repository of its own
(its `board.dts` at the top) is built with `make phone FROM=URL`.

## How do I submit changes

1. BEST: change the file where it is kept (`drivers/board/thermal.c`, not a copy in an assembled tree), or work in
   the tree and take it back with `kernel/export.sh`.
2. LESS GOOD: a patch made elsewhere: apply it to an assembled tree, then `kernel/export.sh`
   (`kernel/import-patch.sh` does both).

Requirements:

* `make check` passes, and so do `kernel/verify.sh` and `kernel/syntax-check.sh` for a change to the kernel.
* One topic a commit. The subject starts with what it touches (`drivers/board/thermal:`, `Metal:`, `arch/arm64/boot:`),
  and the body says why.
* A driver starts only on the boards whose file lists its feature, and does nothing without its boot argument.
* Nothing of Apple's, of a phone maker's or of a vendor's is committed: `tools/check-publishable.sh` looks.

More: [CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

Our work is MIT (`LICENSES/preferred/MIT`); what extends Apple's kernel and what came from HakiOS is APSL 2.0
(`LICENSES/other/APSL-2.0`). [NOTICE.md](NOTICE.md) has the list.
