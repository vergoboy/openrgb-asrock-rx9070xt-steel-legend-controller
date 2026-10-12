# Linux AMC driver route - findings (2026-09-02)

Status: **STOPPED - conclusive negative for generic-I2C Linux RGB control.**

## Goal
Get working, all-modes RGB control of the Arc B580 Steel Legend on Linux, driven
by OpenRGB, without the Windows VM. Chosen path per user: write a Linux kernel
I2C client driver bound to the GPU AMC (`28-0036`) and discover the lighting
protocol - the Linux analogue of ASRock's Windows `AsrNfDrv000.sys`.

## What was done
1. Installed matching kernel headers:
   `sudo pacman -S --needed linux-headers`  (kernel `7.1.9-arch1-2`).
2. Wrote an out-of-tree I2C client driver: `b580/linux/amc_driver/amc_i2c.c`.
   - `i2c_driver` id_table `{ "amc" }` binds the client `xe` registers at
     `28-0036`.
   - sysfs interface under `/sys/bus/i2c/devices/28-0036/`:
     `addr` (ro), `reg` (rw, 16-bit), `value` (rw, byte at `reg`),
     `wr` (wo, raw hex block write), `rd`/`dump` (ro, last block).
   - Reads/writes use the 2-byte (big-endian) register address that the earlier
     userspace probe proved was the correct AMC register-addressing form
     (one-byte addressing returns all-zeros and blocks higher registers).
3. Built and loaded the driver:
   - kern-API note: this kernel's `struct i2c_driver` still uses the old single-arg
     `int (*probe)(struct i2c_client *)` (verified in
     `/usr/lib/modules/7.1.9-arch1-2/build/include/linux/i2c.h:274`);
     `probe_new` is gone.
   - sysfs attrs must be added on the **device** via `device_add_groups()`
     in probe; separate `driver.groups` only populates the driver dir and does
     NOT create the device attrs.
   - Load: `sudo insmod amc_i2c.ko` auto-binds `28-0036` (driver symlink +
     `addr/reg/value/wr/rd/dump` appear on the device node).

## Definitive result (negative)
Under host `xe`, the AMC at `0x36` on `i2c-28`:
- **ACKs** I2C (no longer times out - improved since the VM recovery).
- **Register reads return all `0x00`** and **higher registers block/stall**,
  INCLUDING with the correct 2-byte register addressing. The earlier userspace
  `amc_readreg` values (`0x9c`, `0x04`, ...) were **echo artifacts**, not real
  register contents exposed via host I2C.
- The 12-byte `0x10` color packet **ACKs but does not change the LED**.
- PCODE mailbox (`0x54`) is **contended/unstable** under xe (returns `-110`,
  status `0xFF`) - the GPU is actively using it.

### Conclusion
On the host, `0x36` is the AMC's I2C **management** slave: it responds to bus
ACKs but the actual **lighting** registers are NOT reachable as generic I2C
register reads. The Intel/ASRock lighting path is a **separate memory-mapped
interface through the GPU** (the SOC_BASE / AMC MMIO window), which the kernel
driver exposes only to the driver itself, not to userspace I2C. Hence:

- OpenRGB's B580 controller (raw `0x10` write to `0x36`) cannot change the LED.
- No amount of I2C register probing finds the lighting state.

**Linux, using generic I2C, cannot drive the B580 RGB. The only proven
controller is the Intel AMC lighting API inside the Windows VM.**

## Reusable artifacts
- `b580/linux/amc_driver/amc_i2c.c` + `Makefile` - diagnostic I2C client driver.
  Build: `make`; Load: `sudo insmod amc_i2c.ko`; Unload: `sudo rmmod amc_i2c`.
  Use for future diagnostics only; does NOT drive lighting.
- Copy of these findings kept with the driver for reference.

## Recommended path to real Linux RGB (future, not pursued)
Plumbing the GPU's **memory-mapped AMC lighting window** through the `xe` driver
into an i2c/chardev userspace interface. This is a multi-day kernel-internals
effort, carries real risk to the card, and cannot be verified from userspace
alone. Capturing the exact bytes the working Windows Intel/ASRock driver sends
(re-attach VM) would be the first feasibility step - but even then, the command
must be delivered over the memory-mapped path, not generic I2C.

## Safety
The card is healthy (AMC recovered in the VM). No firmware was flashed. The
`amc_i2c` driver is reversible (`rmmod`). The B580 remains on host `xe`,
audio on `snd_hda_intel`.