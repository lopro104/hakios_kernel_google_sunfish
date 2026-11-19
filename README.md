# ios-sunfish

**Apple's iOS 16.5 on the Google Pixel 4a (`sunfish`, Snapdragon 730G / SM7150).**

Genuine iOS 16.5 (20F66, iPhone X `iPhone10,3` userland) running on Apple's open-source XNU kernel (xnu-8796.121.2), directly on
the hardware: no Android underneath. The kernel is the [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) base with this
phone's drivers on top (`devices/sunfish` in the HakiOS tree). This repository holds the notes, research and tools of the port;
`NOTES.md` is the running log of what was done, measured and decided.

> [!NOTE]
> **State (2026-11-19):** iOS is installed on the phone's own storage and boots by itself from `boot_a`, with Android
> (EvolutionX) kept on slot b. It is usable day to day: home screen, apps, Wi-Fi, Bluetooth audio, sound, SSH.
> Battery life is the current work (idle draw is several times Android's).

---

## What works

| Subsystem | State | Notes |
| :--- | :---: | :--- |
| Boot | **Works** | Stock ABL → U-Boot (sm7150-mainline based) → HakiOS EFI loader → XNU. Installed in `boot_a`; root and data on the `darwin` partition. Nothing of Android's slot b is touched. |
| Display | **Works** | 1080×2340 Samsung `sofef01` command-mode OLED, pingpong autorefresh. iOS draws at 1125×2436 (iPhone X), fitted to the panel. |
| GPU | **Works** | Adreno 618 through Mesa freedreno (OpenGL); a Metal driver boots but is not the default yet. |
| Touch | **Works** | ST FTM5 over I²C (QUP SE7). |
| Buttons | **Works** | Power, volume (PM6150 PON, PM6150L GPIO). |
| Brightness | **Works** | DCS 0x51 over DSI0; Control Center and Settings slider. Panel sleep at screen off is being tested (`panel_power=1`). |
| Battery | **Works** | PM6150 fuel gauge, charge state, temperature; status bar and `powerd`. |
| CPU | **Works** | All 8 cores (PSCI), OSM frequency control with a load-following governor and thermal limits. |
| Storage | **Works** | UFS; APFS root, journaled HFS+ `/private/var` that persists across boots. |
| Wi-Fi | **Works** | WCN3990 through the modem (QMI/WLFW); join from Settings, real MAC from ABL's device tree. |
| Bluetooth | **Works** | WCN3990 UART; A2DP audio to speakers/headphones, Control Center toggle. |
| Sound | **Works** | ADSP (Q6) playback to the CS35L41 amplifier with its protection firmware. |
| Microphone | **Works** | Realtek RT5514 → TDM → ADSP capture; Voice Memos records. |
| Haptics | **Works** | TI DRV2624. |
| Flashlight | **Works** | PM6150L flash LED; Control Center button. |
| SSH | **Works** | OpenSSH over Wi-Fi, key login only. |
| Sensors | Not yet | They run inside the ADSP (SEE); the sensors process does not start yet. |
| Cellular, GNSS | Not yet | The modem boots (it serves Wi-Fi); no CommCenter link. |
| Fingerprint | Not yet | Same FPC secure app as the Pixel 5 port below: planned. |
| Camera | Not yet | |

---

## How it boots

```
Pixel 4a ABL (unlocked)
  └── boot_a: U-Boot (sunfish_storage)
        └── HakiOS loader.efi (reads ABL's device tree: MAC/BT addresses, reboot reason)
              └── XNU + Apple device tree
                    ├── root (APFS) and /private/var (HFS+) on the darwin partition
                    └── launchd → backboardd → SpringBoard
```

Test runs go through `devices/sunfish/run.sh` in the HakiOS tree, which boots a fresh kernel from RAM without writing the phone.
`devices/sunfish/mkinstall.sh` builds the image that is flashed to `boot_a`.

---

## Building and running

You need: a Pixel 4a (`sunfish`, not the 5G) with an **unlocked bootloader**, a Linux PC with `fastboot`, `adb`, `clang`/`lld`,
`python3` and an `aarch64-linux-gnu-` cross toolchain, the iOS 16.5 (20F66) iPhone X IPSW, and this phone's Android vendor
firmware (from an Android build or a factory image: GPU, modem/Wi-Fi, ADSP; it is not in any of these repositories).

**1. Get the trees.** The kernel tree is
[hakios_kernel_google_sunfish](https://github.com/lopro104/hakios_kernel_google_sunfish) (HakiOS with this phone's changes); the
phone's drivers and scripts are [hakios_device_google_sunfish](https://github.com/lopro104/hakios_device_google_sunfish), checked out
as `devices/sunfish` inside it. Everything below runs from the kernel tree's top directory.

**2. Build.**
```sh
make kernel                                                   # XNU with the sunfish drivers
make loader                                                   # the HakiOS EFI loader
CROSS_COMPILE=aarch64-linux-gnu- uefi/u-boot/build.sh sunfish_storage
FW_ADSP=/path/to/vendor/firmware devices/sunfish/build-flash.sh   # the root and boot volumes (with SSH: see below)
```

**3. Try it without writing anything.** With the phone in its bootloader:
```sh
FLASH=1 devices/sunfish/run.sh ["extra boot args" [SECONDS]]
```
This boots iOS from RAM, keeps a log in `logs/`, and hands the phone back after SECONDS (90 by default). The phone needs the
`darwin` partition with the iOS root on it first; NOTES.md ("STORAGE", "HFS DATA VOLUME") says how it was made on ours.
Read NOTES.md before you change partitions: never write `modemst1/2`, `fsg` or `persist`.

**4. Install.** `devices/sunfish/mkinstall.sh` makes `build/ios-boot.img` (U-Boot, loader, kernel and boot volume in one image).
Test it with `fastboot boot build/ios-boot.img`, then, when happy:
```sh
fastboot flash boot_a build/ios-boot.img
fastboot set_active a
fastboot reboot
```
Slot b keeps Android: `fastboot set_active b` goes back.

**SSH.** Put your public key in `ssh/authorized_keys` (git-ignored) before `build-flash.sh`; then `ssh root@<phone's Wi-Fi address>`.
Key login only, no passwords.

**Useful boot arguments** (given to `run.sh`): `panel_power=1` panel sleep at screen off; `bat_measure=SECONDS` logs the battery
current for a minute without unplugging; `cpu_gov=0` fixed top CPU speed; `metal=1` Metal instead of OpenGL. NOTES.md lists the rest.

---

## Repository layout

- `NOTES.md`: the project log: current state, traps, measurements, decisions.
- `docs/`: hardware notes (`docs/SUNFISH.md`) and older bring-up documents.
- `tools/`, `userland/`, `shim/`, `xnu/patches/`: the first ("classic") tree, from before the move to HakiOS; kept as reference.
- `research/`, `scratch/`: local only (git-ignored): dumps, Android captures, downloads.

The kernel, loader, drivers and userland build live in the HakiOS tree (`~/hakios`, branch `merge-upstream-2`, with
`devices/sunfish` as its own repository).

---

## Credits

This port stands on a lot of other people's work. Every repository we built on, ported from, or studied:

**Base**
- [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel): the HakiOS kernel tree this port is built on
  ([hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel), [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland),
  [hakios-uefi](https://github.com/oosexgang/hakios-uefi), [hakios-metal](https://github.com/oosexgang/hakios-metal),
  [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768)).
- [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet): the first tree of this project (Redmi Note 10 Pro, same SoC).
- [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu) and Apple's other open-source releases.
- [sm7150-mainline/u-boot](https://github.com/sm7150-mainline/u-boot) and the sm7150-mainline Linux work: U-Boot and register facts.

**Ports to the same or related phones (ported from or studied)**
- [Yofuness/ios-sunfish](https://github.com/Yofuness/ios-sunfish): another iOS port to the Pixel 4a; Bluetooth, audio and sensors work came from reading it.
- [DenialOfGravity/ios-redfin](https://github.com/DenialOfGravity/ios-redfin): Pixel 5; Touch ID, sensors, cellular.
- [DC28088/ios-sweet](https://github.com/DC28088/ios-sweet): Redmi Note 10 Pro, newer iOS versions.
- [Nik300/ios-viva](https://github.com/Nik300/ios-viva): CPU and GPU governors, panel sleep.
- [printlndarling/hakios-thyme](https://github.com/printlndarling/hakios-thyme), [pixelpixys/hakios-malmo](https://github.com/pixelpixys/hakios-malmo),
  [superboyyy/hakios-arm64e-dada](https://github.com/superboyyy/hakios-arm64e-dada), [iola1999/hackios-redmi-k90-annibale](https://github.com/iola1999/hackios-redmi-k90-annibale),
  [SakuraKyuo/hackios-xnu-kernel_corvette](https://github.com/SakuraKyuo/hackios-xnu-kernel_corvette): other HakiOS devices.
- [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz), [nishant713/ios-pipa](https://github.com/nishant713/ios-pipa),
  [lx200332/popsicle-ios-port](https://github.com/lx200332/popsicle-ios-port), [NWylynko/ios-flame](https://github.com/NWylynko/ios-flame),
  [ry-diffusion/Bergamota](https://github.com/ry-diffusion/Bergamota), [PixOS-Project/pixelios-kernel](https://github.com/PixOS-Project/pixelios-kernel),
  [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU): other ports and kernels studied.

**Android side (hardware facts)**
- Google's `android_kernel_google_msm-4.14` and the sunfish device tree (clocks, regulators, panel, audio paths, as built in EvolutionX).
- [EvolutionX](https://github.com/Evolution-X): the Android build used for comparison and firmware.

**Userland and tools**
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (freedreno), [Procursus](https://github.com/ProcursusTeam/Procursus) (SSH and shell),
  [ElleKit](https://github.com/evelyneee/ellekit), [Sileo](https://github.com/Sileo/Sileo).
- [leminlimez/Nugget](https://github.com/leminlimez/Nugget), [ethxnn88/VisibleIsland](https://github.com/ethxnn88/VisibleIsland): MobileGestalt and Dynamic Island research.
