# B580 RGB Interface Investigation

Status: **NO confirmed B580 RGB command on Linux yet** (2026-09).

Two previously suspected transports have each been **negatively tested** with
controlled single-variable experiments:

1. **I2C `0x10` to `/dev/i2c-28`/`0x36`** — a raw 12-byte `I2C_RDWR` block
   write **times out** (`Connection timed out`, no ACK). The AMC at `0x36` has
   no bound driver / `/dev` data path.
2. **PCODE `0x54/0x00`** — `pcode_rgb_write` reports **status OK** but a
   controlled red→green→blue sequence produced **no visible LED change**.
   `0x54` is a write-behind status/ACK register, not the LED command.

At least one visible RGB change was confirmed at some point (the GPU sits pure
red), but **its exact mechanism is not yet isolated**. The working command is
still unknown.

This document records the investigation performed to determine how the ASRock
Arc B580 Steel Legend GPU RGB is exposed and to locate a usable Linux control
path.

## Background

The ASRock Arc B580 Steel Legend ships with integrated ARGB LEDs, an ARGB
header, and ASRock **Polychrome SYNC** support. This document tracks the
search for a usable Linux control path. The **confirmed** path is the PCODE
mailbox `0x54/0x00` RGB write described above.

## Key findings

### 1. The I2C `0x10` path is disproven: raw I2C to `0x36` times out

The earlier OpenRGB `ASRockArcB580GPUController` sends a 12-byte `0x10` packet
(`[0x10, 0x00, channel, mode, R, G, B, speed, brightness, direction, 0x1A, 0x00]`)
to I2C address `0x36` on bus 28 (the Synopsys DesignWare adapter). Controlled
testing (via `diagnostics/b580_rgb_write.c`) shows this is **disproven**:

- A raw 12-byte `I2C_RDWR` block write to `/dev/i2c-28` addr `0x36`
  **times out** (`Connection timed out`, no ACK).
- The `amc` client `28-0036` has **no bound driver and no `/dev` node**, so
  there is no userspace data path to it.
- The `123.orp` "profile load succeeded" log line reflects successful parsing,
  not a proven LED change.

The `0x10`/I2C implementation is retained as historical evidence but is
**not** the active transport.

### 2. The GPU's I2C endpoint proves there is only ONE I2C MCU (the AMC at 0x36)

Read the B580's own BAR0 MMIO I2C-endpoint registers (the exact two the kernel
`xe_i2c_probe` reads to discover its I2C clients):

- `REG_SG_REMAP_ADDR_PREFIX`  = `SOC_BASE + 0x0164` = `0x280164`
- `REG_SG_REMAP_ADDR_POSTFIX` = `SOC_BASE + 0x0168` = `0x280168`

On this card they report:

```
REG_SG_REMAP_ADDR_PREFIX  = 0x000000DE
REG_SG_REMAP_ADDR_POSTFIX = 0x00000036
```

Decoding as the kernel does (`ep = { u8 cookie; u8 capabilities; u16 addr[3]; }`):

- cookie           = `0xDE`  (matches `XE_I2C_EP_COOKIE_DEVICE`)
- capabilities     = `0x00`
- addr[0] (unused) = `0x0000`
- addr[1] (**AMC**, `XE_I2C_CLIENT_AMC`=1) = `0x36`
- addr[2] (other)  = `0x0000`

So the **only** I2C MCU on the GPU's DesignWare bus is the **AMC at `0x36`**
(matching the `28-0036`/`29-0036` amc devices). **There is no second RGB
controller I2C slave.** Any RGB controller lives behind the AMC (via its
subcommand interface) or on a non-I2C path (PCODE/PCI).

Repro tool: `diagnostics/i2c_endpoint_probe.c` (read-only BAR0 MMIO read; run
`sudo ./i2c_endpoint_probe`).

### 3. No USB HID RGB controller is present

`lsusb` is not installed on the reference system, so the enumeration used
`/sys/bus/usb/devices`. No device matches ARC/ASRock/RGB-like USB IDs. The
only Intel Arc RGB controller with native OpenRGB support (the A770 Limited
Edition) is USB HID vendor `0x2516`, product `0x01B5` (Cooler Master) and is
**not present** on this B580 system.

### 4. The RGB is not part of Intel's official PCODE

Intel's `xe_pcode_api.h` documents the entire public PCODE command set:
min-freq QOS table (`0x8`/`0x9`), DGFX status (`0x7E`), power setup (`0x7C`),
thermal info (`0x25`), late binding (`0x5C`), frequency config (`0x6e`), and
fan speed control (`0x7D`). **There is no lighting/RGB PCODE command.**
Intel's public documentation states there is no RGB control application for
the B580 (the A770 Limited Edition is the only Intel Arc with RGB control),
and ASRock's B580 product page lists no RGB software download.

### 5. The PCODE mailbox is reachable from userspace and works

The one proven, reusable path on this system is the **PCODE mailbox** over
the GPU's PCI BAR0. This is the same mechanism fan-control reverse-engineering
projects (e.g. PerkyZZ999/XeDriver_FanPatch) used to reach the B580's
low-level AMC/PCODE controller:

| Register | Offset |
|---|---|
| `PCODE_MAILBOX` | `0x138124` |
| `PCODE_DATA0`   | `0x138128` |
| `PCODE_DATA1`   | `0x13812C` |

The mailbox works from userspace (with root) on this exact card:
`pcode_rgb_probe --baseline` successfully read fan count, thermal data,
late-binding capability, and power I1 (see `diagnostics/pcode_rgb_probe.c`).

### 6. PCODE `0x54/0x00` is a write-behind register, NOT the RGB command (disproven)

A monitored write-sweep over the proprietary PCODE command space
(`0x0F`-`0x1F`, `0x30`-`0x3F`, `0x54`, `0x6F`-`0x7B`) and the unknown
`FAN_SPEED_CONTROL 0x7D` subcommands was performed while watching the RGB.

- **`0x54/0x00` was the only command that accepted a write** (status OK), but a
  **controlled red→green→blue sequence (`diagnostics/pcode_rgb_write.c`)
  produced no visible LED change**. `0x54` reads back what was written
  (write-behind status/ACK register), so it is **not** the RGB command.
- No other command in the space accepted a write.
- The responsive read commands (`0x6f`/`0x70` clock-like, `0x71` a version
  string) are not lighting.

Earlier this command had been tentatively called "confirmed lighting
(`DC R G B`)" and the RGB state attributed to `DC ff 0 0`; the controlled test
above disproves that attribution. The LED-red state's actual mechanism is
**not yet isolated**.

## Conclusion

**No confirmed B580 RGB command on Linux yet.** Controlled testing disproved
two candidates:

- **I2C `0x10`/`0x36`** (`b580_rgb_write.c`): raw write times out; AMC has no
  userspace data path.
- **PCODE `0x54/0x00`** (`pcode_rgb_write.c`): status OK but **no LED change**
  across red/green/blue; it is a write-behind status/ACK register.

The GPU's only I2C MCU is the **AMC** at `0x36`; raw userspace I2C to it times
out. The LED sits pure red and at least one visible change was confirmed, but
its exact mechanism is not isolated. The real RGB command is likely issued by
ASRock's Windows software through the Intel driver's AMC API (which no
userspace Linux interface exposes here).

Next steps: (1) capture the actual RGB writes from ASRock Polychrome
RGB/ASRRGBLED — via QEMU `-d mmio` tracing (BAR0/PCODE/MMIO) with the B580
passed through to a Windows VM, or by RE of the ASRock tool / B580 LED
driver; (2) confirm the exact command with a controlled single-variable test;
(3) only then expose it through a proper OpenRGB controller.

## Reusable artifacts

- `diagnostics/pcode_rgb_write.c` — deterministic PCODE `0x54/0x00` RGB-color
  writer. **Reproduces the negative result** (status OK, no visible change) in
  a single command: `sudo ./pcode_rgb_write ff0000`.
- `diagnostics/pcode_rgb_probe.c` — userspace PCODE mailbox probe with
  baseline, scan, interrogate, interactive (`DC`/`DCZ`), and monitored
  write-probe modes.
- `diagnostics/pcode_protocol.h` — PCODE register/command constants.
- `diagnostics/i2c_endpoint_probe.c` — read-only BAR0 MMIO read of the GPU's
  I2C-endpoint registers (`0x280164`/`0x280168`) to enumerate all I2C MCUs.
- `diagnostics/b580_rgb_write.c` — I2C `0x10` reproducer. **Reproduces the
  negative result** (raw I2C to `0x36` times out).
- `diagnostics/Makefile` — build recipe (`make`).