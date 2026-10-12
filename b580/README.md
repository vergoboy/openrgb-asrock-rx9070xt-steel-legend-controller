# OpenRGB ASRock Arc B580 Steel Legend Controller

Native OpenRGB controller work for the ASRock Arc B580 Steel Legend GPU RGB.

> **Status: NO confirmed B580 RGB command on Linux yet.**
>
> Controlled testing disproved two candidate transports:
>
> 1. **I2C `0x10` to `/dev/i2c-28`/`0x36`** — raw block write **times out**; the
>    AMC at `0x36` has no bound driver / `/dev` data path.
> 2. **PCODE `0x54/0x00`** — reports **status OK** but a controlled
>    red→green→blue sequence produced **no visible LED change** (it is a
>    write-behind status/ACK register, not the LED command).
>
> See [`docs/PCODE_INVESTIGATION.md`](docs/PCODE_INVESTIGATION.md) for the full
> record. At least one visible RGB change was confirmed at some point (the GPU
> sits pure red), but its exact mechanism is not yet isolated.

## Reproducers

Both negative results are reproducible in one command each
(`b580/diagnostics/`, root required):

```bash
cd b580/diagnostics
make
sudo ./pcode_rgb_write ff0000   # PCODE 0x54/0x00: status OK, no LED change
sudo ./b580_rgb_write 28 0x36   # I2C 0x10 to 0x36: connection timeout
```

## Status of the I2C `0x10` and PCODE `0x54/0x00` paths

Both are now classified as **disproven** by controlled single-variable
experiments. The repository retains these implementations as historical
evidence, not as the active transport.

## Next step

Capture the real RGB command from ASRock's Windows software (Polychrome RGB /
ASRRGBLED) — via QEMU `-d mmio` tracing (BAR0/PCODE/MMIO) with the B580 passed
through to a Windows VM, or by RE of the ASRock tool / B580 LED driver. The
Excel list is kept out of git; ASRock reference downloads belong in
`b580/ref/`.

## License

GPL-2.0-or-later, matching OpenRGB.