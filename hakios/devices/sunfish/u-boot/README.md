# U-Boot for the Pixel 4a

This is not HakiOS's base (uefi/u-boot/BASE, v2026.07). Mainline does not have this SoC, so the base is sm7150-mainline's U-Boot,
branch `tauchgang`, commit `f8c04469`, which has its clocks, pins and device tree, with uefi/u-boot/hakios.patch and files on top:

    git clone <sm7150-mainline u-boot> u-boot && git -C u-boot checkout f8c04469
    git -C u-boot apply uefi/u-boot/hakios.patch && cp -r uefi/u-boot/files/. u-boot/ && cp -r devices/sunfish/u-boot/files/. u-boot/
    uefi/u-boot/build.sh sunfish

The device tree is the fork's with the HakiOS payload reservation and every fixed carve-out of the phone's Android tree added.
No `scsi scan`, and the loader writes no log to storage on this phone: a first run touches no storage.

`uefi/u-boot/build.sh sunfish_storage` (build-sunfish_storage/) is the same with `scsi scan` first, so the loader can read partitions. It leaves out EFI variable storage and fastboot flash, which could write once the disks are visible.
