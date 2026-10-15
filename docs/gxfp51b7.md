# GXFP51B7 (Goodix ChicagoHS) — experimental support

Status: works end to end on **one** laptop: enrollment, verification, `sudo`,
polkit and the Cinnamon lock screen through `pam_fprintd`. Not ready for
general users; see [Prerequisites](#prerequisites) and [Limits](#limits).

| | |
|---|---|
| Laptop | Huawei MateBook X Pro 2020, DMI product `MACHC-WAX9`, BIOS 1.26 |
| ACPI | `GXFP51B7`, 32 KiB mailbox at `0x40200000` |
| Sensor | chip `0x2504` (ChicagoHS), 80x64, firmware `GF_9ELIBE_EC_19024` |
| Tested on | Linux Mint 22.3, kernel 7.0.0-38, Mbed TLS 3.6.7, fprintd 1.94.3 |
| Windows reference | `gfspi.dll` 1.1.151.18, SHA-256 `d98e873e02b00d03d90ccdd78ac69624676a6b9c3662ce66dfca220551b675cd`; RVAs below refer to it |

## What is in this series

| Part | Files |
|---|---|
| Kernel transport for `GXFP51B7` on the existing `/dev/gxfp` UAPI | `kernel/transport/gxfp_51b7.[ch]`, small hooks in `driver/`, `gxfp_main.c` |
| 48-byte TLS PSK | `src/tls/*`, `tls_constants.h` (+ OpenSSL interop test) |
| ChicagoHS image decoder | `src/algo/image/decoder.c` (+ test) |
| Six-channel FDT codec | `src/proto/milan_hu_fdt.c` (+ test; historical name, see below) |
| Session: init, TLS, finger detect, capture | `src/flow/chicago51b7.c`, demo `gxfp51b7_capture` |
| libfprint driver | `libfprint/libfprint/drivers/gxfp51b7.c` |
| Ridge-overlap matcher for SIGFM | `sigfm/sigfm.cpp`, `fpi-print.c`, `ridge_threshold` class field (+ tests) |

The `userspace/` sources are mirrored byte for byte into
`libfprint/libfprint/drivers/gxfpmoc/`, as for the 5130.

## Prerequisites

None of these are installed by this series; the first one has a helper
script.

1. **Identity DMA for the mailbox.** The EC reads TX frames from the mailbox
   and writes RX frames into it by DMA through the LPC bridge `00:1f.0`. The
   firmware has no RMRR for `0x40200000..0x40207fff`, so under a DMA domain
   these accesses fault (`PTE Read/Write access is not set`). The driver
   checks `iommu_iova_to_phys()` over the whole range and refuses to bind
   without an identity mapping. A DMAR override that adds the missing RMRR,
   or `iommu=pt`, satisfies it; both were tested, see
   [DMA for the mailbox](#dma-for-the-mailbox).
2. **The 48-byte PSK** (`/var/lib/fprint/gxfp51b7/psk.bin`, mode 0600).
   Windows keeps it sealed (SGX). On the test machine it was read from the
   EC; that procedure is specific to this EC firmware and is not published.
   5130 provisioning (32 bytes) does not apply.
3. **Per-sensor material** in `/var/lib/fprint/gxfp51b7/`: `otp.bin` (64),
   `config.bin` (256) and `manual-seed.bin` (14), taken from a Windows driver
   capture of the **same** sensor. The session refuses to start unless the
   live OTP equals `otp.bin` and the config checksum is `0x5a5b`.
   Generating the config from OTP and a model template is not done yet.

## DMA for the mailbox

The IOMMU group of `00:1f.0` also holds audio, SMBus and SPI
(`00:1f.3`..`00:1f.5`), so the group cannot be detached or given a domain of
its own. Two configurations pass the driver's check without any out-of-tree
module:

| | DMAR override with an RMRR | `iommu=pt` |
|---|---|---|
| What changes | one RMRR added to the firmware DMAR, loaded from the initrd | every device gets an identity domain |
| Group of `00:1f.0` | `DMA-FQ`; `reserved_regions` lists `0x40200000..0x40207fff direct` | `identity` |
| DMA isolation of other devices | kept | lost |
| Needs | `CONFIG_ACPI_TABLE_UPGRADE=y`, no kernel lockdown | a kernel parameter |
| Result | bind; verify 3/3; S3, lock screen unlocked by the finger, verify 3/3 | bind; verify 2/3 (the miss was a matcher no-match, the frame arrived) |
| DMAR faults | 0 | 0 |

Tested on 15 October 2026 with kernel 7.0.0-38, each after a reboot with
`module_blacklist=gxfp51b7_window`, so the earlier out-of-tree window module
could not load. `gxfp` was loaded by udev and bound on its own.

### DMAR override

`scripts/gxfp51b7-dmar-rmrr.py` reads the running DMAR, adds one RMRR for
`00:1f.0` over the `GXFP51B7:00` memory range from `/proc/iomem`, bumps the
OEM revision and writes `DMAR.aml` and a newc cpio holding
`kernel/firmware/acpi/DMAR.aml`. It stops if the range is not reserved in the
firmware memory map (the kernel's RMRR sanity check expects that, and an RMRR
over RAM would expose that RAM to the EC) or if an RMRR already covers it.
It installs nothing.

```sh
sudo python3 scripts/gxfp51b7-dmar-rmrr.py -o /tmp/gxfp51b7-dmar
sudo install -m 0644 /tmp/gxfp51b7-dmar/gxfp51b7-dmar.cpio /boot/
```

The cpio must come before the distribution initrd
(`Documentation/admin-guide/acpi/initrd_table_override.rst`). It was tested
by listing it first on the GRUB `initrd` line:

```
initrd /boot/gxfp51b7-dmar.cpio /boot/initrd.img-7.0.0-38-generic
```

On Debian and Ubuntu, `GRUB_EARLY_INITRD_LINUX_CUSTOM="gxfp51b7-dmar.cpio"`
in `/etc/default/grub` (or a file in `/etc/default/grub.d/`) and
`update-grub` write that line for every kernel, recovery entries included.
The test machine now boots this way with no window module installed: after
a normal reboot and after S3 every touch produced a frame and no DMAR fault
was logged (the matcher accepted 4 of 5 touches, then 1 of 3 after resume).
Expected kernel log:

```
ACPI: Table Upgrade: override [DMAR-HUAWEI- CFL-ULT]
DMAR: RMRR base: 0x00000040200000 end: 0x00000040207fff
```

The IOMMU core maps direct regions into the default DMA domain when the
device is added, as it does for the firmware's own RMRRs (USB, graphics).

- The override replaces the DMAR by OEM ID and table ID whatever the BIOS
  version. After a BIOS update, boot without the cpio and rebuild it: while
  the override is active, `/sys/firmware/acpi/tables/DMAR` is the overridden
  table and the script stops with "an RMRR already covers".
- A direct region pins `00:1f.0` to its default domain; the group cannot be
  passed to VFIO.
- A kernel quirk adding the range for `MACHC-WAX9` would remove the initrd
  step. It is not written.

## Transport (kernel)

- **TX**: plain MP frame (`a0`/`b0`, LE16 length, additive checksum) at
  mailbox `+0`, at most 512 bytes; the whole 512-byte slot is cleared first
  because the EC copies all of it. Then, holding the AML mutex `HWEC.ECMT`
  (100 ms timeout), wait up to 100 ms for EC register `0x3a` (CMDR) to read 0
  and write `0xf0` to it. The ACPI `ECMD` method is avoided: its wait loop is
  unbounded. Holding `ECMT` keeps AML's single CMDR reads and writes
  (`ECRD`/`ECWT`) out of this sequence, but `ECMD` takes `ECMT` per access,
  so an `ECMD` running at the same time could still slip its write in. On
  this firmware only the sensor's own `_DSM` functions 2..4 call `ECMD`,
  `ECM1` has no callers, and no other field aliases offset `0x3a`; nothing
  on Linux calls that `_DSM`.
- **RX**: frame at `+0x1000`. The GPIO IRQ is active **low** (set explicitly).
  The host-ready GPIO idles high; after copying a frame the driver drives it
  low, waits up to 20 ms for the IRQ line to go high, waits 5 ms and drives it
  high again (EC `bb72c`/`bb938`).
- Any RX error disables the IRQ and fails further TX until the module is
  reloaded, instead of spinning on a stuck level interrupt.
- No 5130 startup/recovery runs, and no `D2` at shutdown: `D2` means something
  else on this firmware.
- Binding is limited to `MACHC-WAX9` + BIOS 1.26 by DMI.

## Session (userspace)

Every request waits for its own ACK (`b0 <cmd> 01`) and, where expected, a
response of exact length; late `32`/`34` events are skipped. Timeouts: 2.5 s
per command, 15 s TLS, 5 s per image. Nothing is retried blindly.

| Step | Command |
|---|---|
| host driver present (twice) | `96 01 00` |
| firmware string | `a8 00 00` → `GF_9ELIBE_EC_19024` |
| reset, 200 ms | `a2 05 14` → `01 …` |
| chip ID | `82 00 00 00 04 00` → `04 a2 00 25` (pair-swapped `2504`) |
| OTP | `a6 00 00` → 64 bytes, must equal `otp.bin` |
| reset, idle, timing, config, idle, timing | `a2`, `70 14 00`, `98 88 0c c8 00 c8 00 c8 00`, `90 <config256>` |
| TLS start / end / state | `d0 00 00`, handshake, `d4 00 00`, `ae 55` → byte 7 = 1 |
| background | 3 × (`36` MANUAL + `20 01 00` GET_IMAGE), per-pixel median |
| shutdown | `70 14 00`, `96 00 00` |

The config write is volatile; nothing is flashed.

**TLS**: the sensor is the client. TLS 1.2, `TLS_PSK_WITH_AES_128_CBC_SHA256`
(`0x00ae`), identity `Client_identity`, 48-byte PSK. Mbed TLS must allow PSKs
of 48 bytes (`MBEDTLS_PSK_MAX_LEN`): true for a default 3.6 build, false for
2.28. The engine now says so instead of failing in `mbedtls_ssl_conf_psk`.

**Image**: one TLS application record per frame. Plaintext is a Goodix frame
`20`, payload 10569 bytes: 5-byte header, 10560 bytes of samples, 4-byte CRC
(`0x88` in the outer checksum means "disabled"). Samples are 12-bit, packed 4
per 6 bytes, 80 columns × 88 rows column-major; the first 64 rows are the
sensor, the last 24 are padding (geometry from `gfspi` sensor type 12 table,
RVA `93440 + 12*14`, and confirmed on 13 Windows and the Linux frames).
Capture takes 3 exposures after a 200 ms settle, takes the per-pixel median,
subtracts the background and stretches the 2nd..98th percentile. Contrast
below 4× the background noise returns `-ENODATA` (libfprint retry).

**Finger detection (FDT)**: six channels. Payload 14 bytes: `0c 01` DOWN,
`0e 01` UP, `0d 01` MANUAL, then six LE16 thresholds. Event payload 16 bytes:
status, touch mask, six raw readings. With `pack(x) = ((x & 0xff) << 8) | 0x80`:
`DOWN[i] = pack(raw[i] >> 1)`, `UP[i] = pack((raw[i] >> 1) + delta)`,
untouched channels `pack(delta - 2)` (gfspi `30fa8`, `310a4`). Readings with
`(raw >> 1) & 0xff` equal to `00` or `ff` are rejected (`30e8c`). `delta`
comes from OTP byte `0x2a` (`0x2d` is a copy, `0x2b` its complement), ChicagoHS formula
(`2e920`): `((((c & 15) + 2) * 100 * 256 / (((c >> 4) + 1) * 16 + 64)) / 3 >> 4) & 255`.
It gives 31 here, matching the config and register `0x0082`. The codec file is
still called `milan_hu_fdt` because the arithmetic was first matched against
the MilanHU path; the chip is ChicagoHS, and the MilanHU delta formula is wrong
for it (41).

## Matching

SIGFM (ratio 0.85 + CLAHE) almost never matches two separate touches on this
80x64 sensor: 0..11 of 40 between touches, while frames of one held touch
score 10^5 and more. It only tolerates 5% change of keypoint distances, and
pressure changes the ridge spacing more than that.

This series adds a ridge-overlap score next to SIGFM. Both frames are
high-passed, aligned by a rigid ECC (rotation + translation, 176 seeds, the
best 3 refined), and the correlation of the aligned frames is the score.
A match needs the mean of the two best enrolled frames ≥ `ridge_threshold`;
otherwise the SIGFM score still decides. An affine fit was tried first and
dropped: it stretched other fingers onto the enrolled one (0.79 for a wrong
finger, 0.69 for a real touch). Strong real matches needed almost no stretch
(anisotropy 1.01..1.04); the impostor fits used 1.07..3.3.

- Enabled per driver: `FpImageDeviceClass.ridge_threshold` (0 = off, the
  default). `gxfp51b7` sets 0.80. `FP_SIGFM_RIDGE_THRESHOLD` overrides it.
- Only then the 8-bit frame is stored in the print (appended after the SIGFM
  data, +5 KiB per sample). Prints without it still load; older libfprint
  reads the new prints and ignores the frame.
- ~0.6 s per verify with 16 enrolled frames on 8 threads (OpenCV `parallel_for_`).

One user, one enrollment of the right index finger (16 samples):

| Probe | Touches | Ridge score | Matched |
|---|---|---|---|
| right index, night, ordinary | 14 | 0.71..0.95 | 12 (misses: two lock-screen tries; the third matched) |
| right index, next day, ordinary | 10 | 0.67..0.92 | 8 |
| right index, next day, first series (merged ridges: moist or pressed hard) | 6 | 0.43..0.85 | 1 |
| right index, deliberately off-centre / light | 11 | 0.63..0.94 | 9 (3 by SIGFM) |
| left index (the closest impostor) | 15 | ≤ 0.73 | 0 |
| other fingers | 23 | ≤ 0.72 | 0 |

**The false accept rate is not established.** 0 of 38 impostor touches only
bounds it below ~8% per attempt (95%). On non-overlapping 80x64 crops of the
AES3500 test image, 1 of 25 probes reached exactly 0.80 against a 16..21 frame
gallery. Periodic ridges correlate. A check that the two best enrolled frames
also agree with each other did not separate those cases and is not included.

Tried and not included, measured on the 38 genuine and 38 impostor frames
saved above: local contrast normalisation, CLAHE before the high-pass and
light smoothing each gained at most one genuine touch while raising the
impostor maximum to 0.74..0.75. Adding 13 next-day frames to the 16-frame
gallery raised genuine touches ≥ 0.80 from 27 to 31 of 38, and the impostor
maximum from 0.73 to 0.77. The score itself, not preprocessing or
enrollment coverage, limits this matcher.

## Limits

- One laptop, one BIOS, one firmware, one sensor's config.
- The DMA prerequisite needs a DMAR override from the initrd or `iommu=pt`;
  there is no kernel quirk.
- No config generation from OTP; no PSK provisioning.
- Suspend (S3) was tested with the DMAR override and no window module.
  Surviving kernel updates was tested only with DKMS packaging that is not
  part of this series.
- `D2`, navigation (`50`) and the Windows per-pixel calibration, quality and
  coverage paths (`WBDI 12210`/`17740`) are not ported.
