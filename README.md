# ios-sunfish

**Apple's iOS 16 on the Google Pixel 4a (and Redmi Note 10 Pro).**

This repository boots and runs genuine Apple iOS 16 (iPhone X `iPhone10,3` userland) on the Google Pixel 4a (`sunfish`), sharing the Qualcomm Snapdragon 730G/732G (SM7150) architecture with the Redmi Note 10 Pro (`sweet`, from [ios-sweet](https://github.com/dipa-studio/ios-sweet)).

The project runs Apple's open-source XNU kernel (xnu-8796, iOS 16 generation) directly on bare metal without Android running underneath.

> [!NOTE]
> **Status (Verified on Hardware):** 
> The Pixel 4a boots iOS 16 into a fully interactive SpringBoard UI with working touch, display brightness, hardware buttons, battery monitoring, sensors, and functional Wi-Fi networking with Safari internet access.

---

## Hardware Support Status

| Subsystem | sunfish (Pixel 4a) | sweet (Redmi Note 10 Pro) | Implementation Details |
| :--- | :---: | :---: | :--- |
| **Boot Chain** | **Works** | **Works** | Non-destructive `fastboot boot` via boot shim or U-Boot Header v2 (`0x80000` text offset, LZ4, 64 MiB AVB footer). |
| **Display** | **Works** | **Works** | Sunfish: 1080x2340 Samsung `sofef01` command-mode OLED via MDP pingpong autorefresh (`hakios.cmdpanel`). Sweet: 1080x2400 video-mode panel. |
| **iOS GUI** | **Works** | **Works** | SpringBoard, Lock Screen, Control Center, Settings, Calculator, Safari, Weather. |
| **Touchscreen** | **Works** | **Works** | Sunfish: In-cell ST Microelectronics **FTM5** (`0x49`) over I2C QUP SE7 with PM6150 GPIO4 load switch. Sweet: Goodix GT9896. Home bar swipe and multi-touch functional. |
| **Hardware Buttons** | **Works** | **Works** | Power and Volume Down (PM6150 PON), Volume Up (PM6150L GPIO). |
| **Display Backlight** | **Works** | **Works** | DCS 0x51 over DSI0 with AppleARMBacklight emulation; Control Center brightness slider fully functional. |
| **Battery & Charging** | **Works** | **Works** | PM6150 fuel gauge & ADC5 thermistor; status bar percentage, bolt icon, and `powerd` integration. |
| **Wi-Fi Networking** | **Works** | **Works** | Full `en0` Wi-Fi stack via WCN3990 modem subsystem; Settings > Wi-Fi scanning & WPA2 joining with blue checkmark; IPv4/IPv6 DHCP and internet access. |
| **Sensors & Orientation** | **Works** | **Works** | Accelerometer, gyroscope, and orientation detection (`SweetSensors` / `backboardd` integration). |
| **GPU & Acceleration** | **Works** | **Works** | Adreno 618 bring-up with Mesa freedreno and SweetMetal / HakiosMetal hardware-accelerated rendering. |
| **Storage & Rootfs** | **Works** | **Works** | Internal UFS storage (LUN 0); boots from existing `userdata` partition or RAM-disk (no GPT repartitioning required). |
| **Audio & Camera** | In Progress | In Progress | Audio DSP & camera sensor bring-up underway. |

---

## How It Boots

```
Pixel 4a ABL (Unlocked Bootloader)
  └── fastboot boot <image.img>          (Purely in RAM; boot partitions are never overwritten)
        ├── Option A: U-Boot + HKUB Ramdisk (HakiOS Tree)
        │     └── U-Boot -> loader.efi -> XNU Mach-O + Apple Device Tree (ADT)
        └── Option B: Bare-Metal Boot Shim (Classic Tree)
              └── Shim enables MDP autorefresh -> XNU Mach-O + ADT
                    └── XNU Kernel Initialisation
                          ├── Mounts UFS storage (or APFS RAM-disk)
                          ├── Launches dyld & launchd
                          └── Starts backboardd & SpringBoard (iOS 16 GUI)
```

iOS recognizes the device as an **iPhone X** (`iPhone10,3`, 1125x2436). The Pixel's 1080x2340 OLED panel displays the centered frame with full touch coordinate translation.

---

## Key Hardware Facts (Google Pixel 4a)

- **SoC:** Qualcomm Snapdragon 730G (SM7150-AB)
  - 6x Kryo 470 Silver (Cortex-A55) @ up to 1.80 GHz
  - 2x Kryo 470 Gold (Cortex-A76) @ up to 2.20 GHz
  - ARM GICv3 interrupt controller, PSCI over SMC, 19.2 MHz generic arch timer
- **RAM:** 6 GiB LPDDR4X (starts at `0x100000000`)
- **Display:** 5.81" 1080x2340 Samsung `sofef01` FHD+ OLED, DSI command mode
- **Touch:** STMicroelectronics FTM5 on I2C QUP SE7 (`0xa84000`, address `0x49`), PM6150 GPIO4 load switch
- **Storage:** Micron/Samsung UFS 2.1 at `0x1d84000`
- **Power Management:** Qualcomm PM6150 + PM6150L PMICs over SPMI bus arbiter
- **WLAN:** Qualcomm WCN3990 (integrated 802.11ac Wi-Fi & Bluetooth 5.0)

---

## Repository Structure

- `hakios/` (or `~/hakios`): Modern HakiOS base migration featuring clean driver manifests, modular Kconfig defconfigs, U-Boot EFI loader (`loader.efi`), and integrated SoC layer for SM7150.
- `shim/`: Bare-metal ARM64 boot shim generating Android boot image v2 with 4096-byte pages and AVB hash footer for Google Pixel ABL.
- `xnu/patches/`: The chronological XNU kernel patch series.
- `tools/`: Build scripts, Apple Device Tree compiler (`mkadt.py`), boot image packer (`mkubootimg.py`), userland root helpers, and diagnostics.
- `userland/`: Helper tools, tweaks (`SweetSensors`), network routing utilities, and UI benchmarks.
- `docs/`: Technical specifications, hardware differences ([`docs/SUNFISH.md`](docs/SUNFISH.md)), GPU driver documentation, and bring-up guides.
- `NOTES.md`: Active developer and handoff log recording exact commits, test runs, and hardware measurements.

---

## Getting Started

### Prerequisites
- Google Pixel 4a (`sunfish`, non-5G) with an **unlocked bootloader**.
- Linux development workstation with `fastboot`, `adb`, `python3`, `clang`, `lld`, and `avbtool`.
- iPhone X iOS 16.7.16 IPSW (`iPhone10,3_16.7.16_20H392_Restore.ipsw`) or iOS 16.5 IPSW.

### 1. Booting with Fastboot (Non-Destructive)
Device bring-up uses temporary RAM-loaded boot images. The bootloader, recovery, modem, and partition tables remain completely untouched:

```bash
# Boot the pre-assembled signed HakiOS / U-Boot image:
fastboot boot hakios/build/u-boot-sunfish-hakios-signed.img

# Or boot the classic shim image:
fastboot boot shim/out/shim-boot.img
```

### 2. Live Kernel Logging & Diagnostics
The kernel streams logs live over USB using CDC-ECM networking at `192.168.7.2`:

```bash
# Connect and view live kernel console output:
nc 192.168.7.2 1050

# Or SSH into the live iOS environment:
ssh root@192.168.7.2
```

---

## Credits & Acknowledgements

- [**ios-sweet**](https://github.com/dipa-studio/ios-sweet) & **dipa-studio**: The foundational project bringing iOS 16 to Qualcomm SM7150.
- [**HakiOS / oosexgang**](https://github.com/oosexgang): Clean XNU base architecture, EFI loader, and freedreno/DRM integration.
- **Apple Open Source**: Darwin XNU kernel (`xnu-8796`), IOHIDFamily, libpthread, and HFS/APFS components.
- [**ios-flame**](https://github.com/NWylynko/ios-flame), [**Bergamota**](https://github.com/ry-diffusion/Bergamota), and [**ios-topaz**](https://github.com/MakrSas/ios-topaz): Hardware research and compatibility patches across Snapdragon devices.
- **LineageOS & Mainline Linux**: Hardware DTS trees and driver specifications for `sunfish` and `sm7150`.
