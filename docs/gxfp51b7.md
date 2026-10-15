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
| Sensor config from OTP | `src/algo/chicago_hs_cfg.c` (+ test) |
| Session: init, TLS, finger detect, capture | `src/flow/chicago51b7.c`, demo `gxfp51b7_capture` |
| libfprint driver | `libfprint/libfprint/drivers/gxfp51b7.c` |
| Ridge matcher for SIGFM (phase correlation + ridge residual) | `sigfm/sigfm.cpp`, `fpi-print.c`, `ridge_threshold` class field (+ tests) |

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

No per-sensor files are needed: the config is built from the sensor's OTP
at every start (see [Config from OTP](#config-from-otp)).

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
| OTP | `a6 00 00` → 64 bytes; the config is built from it |
| reset, idle, timing, config, idle, timing | `a2`, `70 14 00`, `98 88 0c c8 00 c8 00 c8 00`, `90 <config256>` |
| TLS start / end / state | `d0 00 00`, handshake, `d4 00 00`, `ae 55` → byte 7 = 1 |
| first MANUAL | `36 0d 01` + cached bare table, or a zero table whose readings alone are kept |
| background | 3 × (`36` MANUAL + `20 01 00` GET_IMAGE), per-pixel median, then `36` |
| config check | `82 00 82 00 02 00` → high byte of `0082` = FDT delta of the config (only after a MANUAL; before one it read 40 for 31) |
| shutdown | `70 14 00`, `96 00 00` |

The config write is volatile; nothing is flashed.

The background needs a bare sensor. If MANUAL reports a touch against the
cached table (below; a finger put down before the prompt, or a touch to
wake the lock screen), the open polls it every 100 ms for up to 4 s and,
once the finger is lifted, waits until the mask has stayed clear for
0.5 s: frames taken right after the lift had noise99 20..64 instead of
11..12. Before, the open failed at once with `-EBUSY` ("Device or resource
busy" from fprintd).

**First MANUAL.** MANUAL sends the current DOWN table. Windows sends the
one an earlier session saved in `goodix.dat` (bytes 64..75, after the OTP);
without that file the table is still zero (`gf_update_all_base`, `5c738`).
The session does the same with its own cache, `fdt-base.bin` (14 bytes,
0600; libfprint keeps it next to the PSK, `FP_GXFP51B7_FDT_CACHE`
overrides). With a cache a finger that is already down shows in the touch
mask, as before. Without one the first MANUAL carries a zero table: the
touch mask of that reply is meaningless (all six channels report touch) and
is ignored, and the readings become the table of the next MANUAL. A resting
finger cannot be seen then, so the table is cached after calibration only
if the background noise is that of a bare sensor. A touch against a cached
table drops the cache; a stale one costs one failed open.

## Config from OTP

`gxfp_chicago_hs_build_config()` repeats what `gfspi` does:

1. `download_general_config` (`1c11c`) copies the 256-byte template for
   sensor type 12 from `.data` RVA `93510 + 12 * 0x100`.
   `device_enable_init_by_chip` (`54ef4`) sets type 12 for chip `0x2504`
   only, so the template does not depend on the sensor.
2. `ChicagoHU_check_sensor` (`2e920`) takes `c = OTP[0x2a]` if it is
   nonzero and the complement of `OTP[0x2b]`, else `OTP[0x2d]` under the same
   test, else `OTP[0x2a]` if it equals `OTP[0x2d]`. When one is accepted it
   computes, from `OTP[0x2a]` in every case (as the DLL does):
   `tcode = ((c >> 4) + 1) * 16 + 64` →
   register `005c` of the section at header bytes 9/10 (`1c66c`), and the
   FDT delta (formula above) → high byte of `0082` of the section at
   header bytes 5/6 (`1c200`). Otherwise the template values stay
   (tcode `0x100`, delta `0x15`).
3. Two of the three 2-bit copies in `OTP[0x1b]` (bits 0..1, 4..5, inverted
   2..3) give the FDT offset; a nonzero offset sets the low byte of `0056` in
   the 5/6 section to offset + 4 (`1c4c0`).
4. Each step rewrites the checksum: the 128 LE16 words sum to `0x5a5b` (`1c0b0`).

The three OTP CRC8s (polynomial `0x07`, result inverted; `2e1a0`) are
checked and logged. Windows carries on when they fail, and so does this.

On the test sensor the config built from its live OTP is byte for byte the
one Windows sent (`90`) in the early-startup capture. The test covers the
template, both tcode bytes, defaults, the `0x2d` path, the offset rules and
the CRC; `GXFP51B7_PRIVATE_VECTORS=<dir>` adds the comparison with a private
`otp.bin`/`config.bin` pair, printing only the verdict.

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

Plain correlation of aligned ridge images does not work either. Most 80x64
patches show near-parallel ridges of similar spacing. A rigid ECC fit of the
left index reached 0.73..0.78 against 0.81..0.95 for real touches, and
nothing in preprocessing or enrollment coverage moved that.

This series adds a ridge score next to SIGFM, built from two measures that
the parallel-ridge pattern does not carry:

1. **Phase-only correlation** (band-limited to half the Nyquist frequency)
   of the high-passed frames, the probe rotated in 2° steps up to ±30°.
   Whitening the spectrum gives ridge endings, bifurcations and pores the
   same weight as the dominant ridge frequency. The peak also gives the pose.
2. **Ridge residual**: each frame minus itself smoothed along the local
   ridge direction (structure tensor σ 4, Gaussian σ 3 along the ridge). It
   keeps what parallel ridges lack. Its correlation at the POC pose, refined
   by a rigid ECC, is near zero unless the pose is a true correspondence.

Each is a z-score against 6240 impostor pairs measured on this sensor (right
index frames against other fingers of the same person, both directions).
For those pairs the two are uncorrelated (r = 0.04), also in the tail: none
of them is ≥ 2 on both. An enrolled frame scores the sum of the two z-scores
when both are ≥ 2, else the smaller one. The best enrolled frame decides;
otherwise SIGFM still does.

- Enabled per driver: `FpImageDeviceClass.ridge_threshold` (0 = off, the
  default). `gxfp51b7` sets 12. `FP_SIGFM_RIDGE_THRESHOLD` overrides it.
- Only then the 8-bit frame is stored in the print (appended after the SIGFM
  data, +5 KiB per sample). Prints without it still load; older libfprint
  reads the new prints and ignores the frame.
- ~30 ms per enrolled frame on one core; 16 frames run in parallel
  (OpenCV `parallel_for_`).

One user, one enrollment of the right index finger (16 samples). The same
saved frames are scored offline by the matcher in this series and by the
previous rigid-ECC score (mean of the two best frames ≥ 0.80):

| Probe | Touches | Ridge score of matches | Matched | Before |
|---|---|---|---|---|
| right index, night, ordinary | 14 | 20.9..44.9 | 12 | 12 |
| right index, next day, ordinary | 10 | 16.1..39.0 | 8 | 8 |
| right index, next day, first series (merged ridges) | 3 | 37.2 | 1 | 1 |
| right index, deliberately off-centre / light | 11 | 16.2..58.0 | 8 | 6 |
| right index, an earlier session (an hour before enrollment) | 24 | 13.7..38.6 | 16 | 9 |
| left index (the closest impostor) | 15 | ≤ 1.6 | 0 | 0 |
| other fingers | 25 | ≤ 1.9 | 0 | 0 |

The ordinary misses have no true counterpart among the enrolled frames under
any method tried: two touched an area that only off-centre touches covered,
two matched no stored frame at all (one looks turned beyond the ±30°
search). Against an earlier enrollment (20 frames, an hour before), 32 of
the 40 later frames match (13 before).

A second enrollment, the left index (16 samples, 11 hours later): 12 of 15
earlier left index touches match it; 84 right index frames score at most 1.9
against it. Live, with this matcher installed, 6 right index touches checked
against the left index template were all rejected (scores -0.4..0.3); offline,
5 of them match the right index template (20..28.5). Of 4 live touches
against the right index template, 2 matched (18.4, 23.6); the other 2 found no
counterpart (POC at impostor level) and touched parts the 16 enrolled frames
do not cover. Live and offline scores agree to 0.01.

**Enrollment.** Touches that miss the enrolled frames are the remaining
genuine misses. In the first 16-stage enrollment 8 of the 16 frames nearly
repeated another (score 45..67). With `FpImageDeviceClass.ridge_duplicate`
(40 for `gxfp51b7`) a frame scoring at least that against an enrolled one is
refused with "move it a little" (at most 3 times in a row), and a blank or
partial frame always, with "press the whole finger". Replaying the saved
touches, refusing kept 16 distinct frames and raised later sessions'
matching touches from 32 to 37 of 48, as many as 24..32 frames without
refusing; a live 16-stage enrollment took 20 touches. 24 stages were tried
and dropped: little gain, a false accept rate that grows with the frame
count, and that trial let in two partial frames before they were refused.

The session matters more than the stages: each template matches its own
session's touches best (skin moisture, pressure). On the same 83 touches
from other sessions the first enrollment matched 60, the second 50, and
both together (32 frames) 69; impostors stayed at most 1.9 against all.
`scripts/fp-merge-sigfm.py OUT A B` merges enrollments of one finger (the
format round-trips byte for byte). Live, 9 of 10 touches matched the merged
template, about 0.13 s per verify.

**False accept rate.** Measured: 0 of 158 impostor attempts (the gallery
above, the earlier enrollment, and the left index as the enrolled finger
against all right index frames), 0 of 6240 impostor frame pairs with both
scores ≥ 2. Family members touched 18 times (two series, various fingers),
checked live and offline against both enrolled fingers (576 frame pairs):
all rejected, at most 1.3. One of those touches, a barely touching finger,
first scored 5.3; it was a blank frame (see below), now skipped. The
threshold had been raised from 10 to 12 before that was understood and was
kept as margin.

An estimate, not a measurement: exponential tails fitted to the same-person
z-scores (above 2.5 for POC, 114 pairs; above 1.5 for the residual, 130
pairs) and independence give ~7e-9 per frame pair at 12 (~1e-7 at 10), so
~2e-7 per attempt with two enrolled fingers (32 frames; PAM identifies
against all of them), ~7e-7 allowing three times the tail dependence seen
below. This rests on one person's fingers plus 18 touches of family
members; other people's skin may have heavier tails. Gaussian tails would be
far too optimistic: the POC tail is much heavier.

**Blank and partial frames.** The sensor has a fixed pattern of vertical
stripes. A barely touching finger gives a frame that, stretched, is mostly
that pattern, and two such frames correlate at zero shift and angle: a
family member's blank touch scored 16 against a partial frame of a trial
24-stage enrollment. So a frame whose vertical stripes (the fy = 0 row of
its spectrum) carry more than 15% of the energy is not compared (real
touches: at most 7.5%; blank ones 24..41%), and an enrolled frame must show
oriented ridges over at least half of its area (enrolled frames 76..100%,
the two partial ones 34% and 38%). No genuine match on the saved frames was
lost.

On 17384 pairs of non-overlapping 80x64 crops (16 px grid) of the AES3500
test image, the maximum is 9.2; 48 pairs are ≥ 2 on both. That sensor has
other statistics; in its own units the maximum is 7.9, but the tails depend
on each other more: P(POC ≥ 2 | residual ≥ 2) is 0.11 there, P(POC ≥ 2) 0.04.

Raising the threshold from 10 to 12 costs one earlier-session touch in the
table and two frames against the earlier enrollment; ordinary touches score
16.1 and more (live: 18.4 and 23.6).

Tried and not included, measured on the frames above: local contrast
normalisation, CLAHE and light smoothing before the rigid ECC; adding
next-day frames to the gallery (more genuine, impostor maximum 0.73→0.77);
block-wise correlation, orientation-field agreement and peak sharpness along
the ridges at the ECC pose; choosing among the ECC candidates by residual;
a ±60° search (one impostor's residual rose to 0.34); a check that the poses
of a probe against two enrolled frames agree (often uninformative: enrolled
frames that nearly coincide agree for any probe).

## Limits

- One laptop, one BIOS, one firmware, one sensor. The config generator was
  checked against that sensor only; other OTP values follow the DLL code
  paths but are untested on hardware.
- The DMA prerequisite needs a DMAR override from the initrd
  (`scripts/gxfp51b7-dmar-rmrr.py`) or `iommu=pt`; there is no kernel quirk.
- No PSK provisioning.
- Reboot and suspend (S3) were tested with the DMAR override and no window
  module. Surviving kernel updates was tested only with DKMS packaging that
  is not part of this series.
- `D2`, navigation (`50`) and the Windows per-pixel calibration, quality and
  coverage paths (`WBDI 12210`/`17740`) are not ported.
