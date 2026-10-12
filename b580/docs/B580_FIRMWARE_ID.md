# B580 RGB MCU Firmware - Positive Identification (2026-09)

Source of truth: the ASRock **Polychrome SYNC v1.0.51** install on the Win10 VM
(`C:\Program Files (x86)\ASRock Utility\ASRRGBLED\ASRISP\UpdateFW.ini`),
section `[Check_VGA_B580_SL]`, detected by PCI subsystem `0X60201849`
(`1849:6020`, exactly matches our B580's `subsystem_device=0x6020`).

## Exact firmware for the ASRock Arc B580 Steel Legend (BMG G21, NUC121 MCU)

| Part  | File                          | Size  | CKSUM | Version    | Date     |
|-------|-------------------------------|-------|-------|------------|----------|
| APROM | `APROM_VGA_250924_1B54.bin`   | 14528 | 0x1B54| 0x01010100 | 2025-09-24|
| LDROM | `LDROM_VGA_210715.bin`        | 4044  | 0x2907| 0x01000100 | 2021-07-15|
| DATA  | `ARC_B580_SL_250317.dat`      | 323   | 0x5303| -          | 2025-03-17|
| ISP   | `APROM_VGA_ISP_191101.bin`    | (ref) | -     | -          | -         |

Files copied to `b580/ref/ASRISP/`:
- `APROM_VGA_250924_1B54.bin`  sha256 ab33269d9643d8f417f07150da2ace8eb7bc4eed30ea65ec47f2a10cb94e9dcc
- `LDROM_VGA_210715.bin`       sha256 f752b1c2e60757b8e38cc5f9948746ae035e56fd99715732abe555da0e58f227
- `ARC_B580_SL_250317.dat`     sha256 4b443a634e031bcb339e7c8e7709543c93f53242cb857e4fbd1ff4ec3e4bf622
- `UpdateFW.ini`               sha256 31d565f33f4c711b51774c97e75850685350ee3674d91b1cfb0c3b0cfd6f3e3c

## Confirmation
- APROM image begins with Cortex-M0 vector table: initial SP `0x20001168`,
  reset vector `0x000000f1` -> genuine Nuvoton ARM firmware, not a stub.
- Both APROM/LDROM sizes (14.5KB / 4.0KB) match the NUC121 flash layout.
- The `.dat` (ARC_B580_SL_250317.dat) is the board LED/mode profile (mode bytes
  0x01..0x12), not the flash image.

## Flash intent (do NOT execute until transport is confirmed)
`AsrISP.exe /I 2 /AUTO` (from `Update.cmd`/`nfi2c.cmd`), using this exact APROM.
This is the ONLY correct firmware for this card - do NOT use RX 9070 XT or A770
images.