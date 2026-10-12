# VFIO Pass-through Audit for the ASRock Arc B580 (project A Rem / LED recovery)

Status: **audit_done — blocked at firmware/BIOS (no DMAR ACPI table).**
Date: 2026-09.

Goal under audit: pass the B580 (`0000:03:00.0`) through to the existing Win10
VM so the official ASRock/Intel driver stack can re-flash the AMC over its real
transport, then return the card to Linux for a working OpenRGB PCODE controller.

Hard rule honored: **no boot-config change, no flashing, no undocumented Linux
AMC writes** until this report is reviewed and the exact B580 firmware image is
identified.

---

## 1. IOMMU / VT-d state (read-only)

- `/sys/kernel/iommu_groups` ......... **0 groups**
- `/sys/class/iommu` .................. missing (no IOMMU device registered)
- `/proc/iomem` DMAR/Remap region .... none
- `intel_iommu` module loaded ........ NO (no `intel_iommu`/`iommu` lsmod entry)
- `/proc/cmdline` ..................... contains `intel_iommu=on`
- `/etc/kernel/cmdline` ............... does **NOT** contain `intel_iommu=on`
- Kernel config (from /proc/config.gz):
  - `CONFIG_DMAR_TABLE=y`, `CONFIG_INTEL_IOMMU=y`
  - `CONFIG_VFIO=m`, `CONFIG_VFIO_IOMMU_TYPE1=m`, `CONFIG_VFIO_PCI=m`
  - `CONFIG_VFIO_PCI_IGD=y`, `CONFIG_VFIO_PCI_VGA=y`
  - `# CONFIG_INTEL_IOMMU_DEFAULT_ON is not set`

### Decisive root cause
`/sys/firmware/acpi/tables/` contains FACP/FADT, MCFG, HPET, multiple SSDTs,
WSMT ... but **NO `DMAR` table**.
→ The firmware exposes **no DMA Remapping table**. Without DMAR, the Intel
IOMMU driver cannot enumerate DRHD/remapping units, so **VT-d never
initializes** regardless of `intel_iommu=on` (and `INTEL_IOMMU_DEFAULT_ON` is
unset). Therefore **0 IOMMU groups ⇒ VFIO GPU passthrough is not possible as
currently booted.**

This is a **UEFI/BIOS-level** state (VT-d/DMAR disabled or not programmed on the
Gigabyte Z790 S DDR4), not a kernel-command-line problem. Enabling it requires a
BIOS setting change + reboot and takes effect only if the firmware then emits a
valid DMAR table.

## 2. B580 PCI topology (what must pass together)

- `0000:00:01.0` pcieport   (root port, x16)
- `0000:01:00.0` pcieport   (`8086:e2ff` switch upstream)
- `0000:02:01.0` pcieport   (`8086:e2f0`, downstream → GPU)
- `0000:02:02.0` pcieport   (`8086:e2f1`, downstream → audio)
- `0000:03:00.0` **B580 GPU**  `8086:e20b`  class 03:00:00, driver `xe`
- `0000:04:00.0` **B580 HDMI audio** `8086:e2f7` class 04:03, driver `snd_hda_intel`

Card = `03:00.0` (GPU) + `04:00.0` (audio). Both under the same `00:01.0` root
port → `01:00.0` switch. To VFIO it cleanly, the GPU + audio functions and their
bridges must share an IOMMU group (impossible today: 0 groups).

## 3. Host display unaffected

- `card0` = iGPU `8086:a782` (i915) at `0000:00:02.0`, owns `fb0` — host console.
- `card1` = B580 `8086:e20b` (xe) — the target.
Passthrough targets only `03:00.0`+`04:00.0`; iGPU remains the host framebuffer.

## 4. Existing Win10 VM (192.168.122.50)

- **QXL-only** (`qxl-vga`), no GPU passthrough, no vfio-pci bound devices.
- `192.168.122.50` works via SSH (askpass). `vgalcdutil.exe` currently faults
  (a symptom of the disconnected VGA driver stack, not the core issue).
- libvirt: `virsh` reports no active VM (the QEMU pid was observed with `-S`).

## 5. What must change to actually re-flash on this box (for review, NOT applied)

1. Enable **VT-d / DMAR** in the Z790 UEFI BIOS and confirm a `DMAR` table
   appears in `/sys/firmware/acpi/tables/`.
2. Make `intel_iommu=on` authoritative in `/etc/kernel/cmdline` (it is currently
   baked only into a stale UKI, not the canonical file) and rebuild the UKI.
3. Confirm ≥1 IOMMU group exists and `03:00.0`+`04:00.0` are isolated together.
4. Bind `03:00.0`+`04:00.0` to `vfio-pci` (or better, a libvirt hostdev, which
   detaches them automatically), leaving iGPU on i915/xe for the host.
5. Add a `<hostdev>` GPU+audio passthrough to the Win10 domain (reversible
   snapshot the existing XML first).
6. In Windows: install the Intel graphics driver + ASRock/Nuvoton SPB driver,
   then confirm the AMC/device is enumerated **before** any flash.
7. Only with the positively-identified B580 APROM image, run
   `AsrISP.exe /I 2 /AUTO` and capture full output.

## Firmware/APROM status (still needed)
- A positive B580-specific APROM image has **not** been identified yet. Do not
  flash until it is. `ref/ASRISP/` holds A770-related images/dats, not a
  confirmed B580 image.

## Blocker ownership
- **Layer that blocks passthrough today**: firmware (BIOS) not supplying DMAR.
  This is outside the kernel/OS.