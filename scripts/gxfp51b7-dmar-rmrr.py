#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Build an ACPI DMAR override that adds an RMRR for the GXFP51B7 mailbox.

The EC moves 51B7 frames in and out of the mailbox by DMA through the LPC
bridge (00:1f.0). The firmware's DMAR has no RMRR for that range, so with
the IOMMU in DMA mode the EC's accesses fault. With an RMRR the kernel
reports the range as a direct region of 00:1f.0 and identity-maps it in the
default DMA domain of the device's group (the same way the firmware's own
RMRRs are handled).

The output is the running firmware's DMAR with one RMRR added and the OEM
revision bumped, plus a newc cpio for the kernel's initrd table upgrade
(CONFIG_ACPI_TABLE_UPGRADE). Nothing is installed.
"""
import argparse
import os
import struct
import sys

PAGE = 0x1000
HDR = 36  # ACPI table header
DMAR_FIXED = 48  # header + host address width, flags, reserved
DRHD, RMRR = 0, 1


def die(msg):
    sys.exit("gxfp51b7-dmar-rmrr: " + msg)


def checksum_ok(data):
    return sum(data) & 0xFF == 0


def parse(data):
    if data[:4] != b"DMAR":
        die("not a DMAR table")
    length = struct.unpack_from("<I", data, 4)[0]
    if length != len(data) or length < DMAR_FIXED:
        die("DMAR length %d does not match %d bytes read" % (length, len(data)))
    if not checksum_ok(data):
        die("firmware DMAR checksum is wrong")
    subs, off = [], DMAR_FIXED
    while off < length:
        if off + 4 > length:
            die("truncated subtable at 0x%x" % off)
        typ, sublen = struct.unpack_from("<HH", data, off)
        if sublen < 4 or off + sublen > length:
            die("bad subtable length at 0x%x" % off)
        subs.append((off, typ, sublen))
        off += sublen
    return subs


def iomem_range(name):
    try:
        with open("/proc/iomem") as f:
            lines = f.read().splitlines()
    except OSError as e:
        die("cannot read /proc/iomem: %s" % e)
    for line in lines:
        span, _, owner = line.strip().partition(" : ")
        if owner.strip() == name:
            start, end = (int(x, 16) for x in span.split("-"))
            if start == 0 and end == 0:
                die("/proc/iomem shows zero addresses; run as root or pass --base/--size")
            return start, end
    die("%s not in /proc/iomem; pass --base and --size" % name)


def firmware_reserved(start, end):
    """True if one firmware memory map entry of type Reserved covers the range."""
    root = "/sys/firmware/memmap"
    try:
        entries = os.listdir(root)
    except OSError:
        return None
    for e in entries:
        try:
            with open(os.path.join(root, e, "start")) as f:
                s = int(f.read(), 16)
            with open(os.path.join(root, e, "end")) as f:
                t = int(f.read(), 16)
            with open(os.path.join(root, e, "type")) as f:
                typ = f.read().strip()
        except OSError:
            continue
        if s <= start and end <= t:
            return typ in ("Reserved", "ACPI Non-volatile Storage")
    return False


def rmrr(segment, base, end, bus, dev, fn):
    scope = struct.pack("<BBHBBBB", 1, 8, 0, 0, bus, dev, fn)  # PCI endpoint
    body = struct.pack("<HHQQ", 0, segment, base, end) + scope
    return struct.pack("<HH", RMRR, 4 + len(body)) + body


def newc(entries):
    out = bytearray()
    ino = 1
    mtime = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    for name, mode, data in entries + [("TRAILER!!!", 0, b"")]:
        nb = name.encode() + b"\0"
        hdr = "070701" + "".join("%08X" % v for v in (
            ino if name != "TRAILER!!!" else 0, mode, 0, 0,
            2 if mode & 0o040000 else 1, mtime, len(data),
            0, 0, 0, 0, len(nb), 0))
        out += hdr.encode() + nb
        out += b"\0" * (-len(out) % 4)
        out += data
        out += b"\0" * (-len(out) % 4)
        ino += 1
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dmar", default="/sys/firmware/acpi/tables/DMAR",
                    help="firmware DMAR to start from (default: running one)")
    ap.add_argument("--base", type=lambda x: int(x, 0),
                    help="mailbox start (default: GXFP51B7:00 in /proc/iomem)")
    ap.add_argument("--size", type=lambda x: int(x, 0))
    ap.add_argument("--bdf", default="00:1f.0",
                    help="DMA source on bus 0 (default 00:1f.0, the LPC bridge)")
    ap.add_argument("--segment", type=int, default=0)
    ap.add_argument("-o", "--outdir", required=True)
    a = ap.parse_args()

    with open(a.dmar, "rb") as f:
        data = f.read()
    subs = parse(data)

    if (a.base is None) != (a.size is None):
        die("--base and --size go together")
    if a.base is None:
        start, last = iomem_range("GXFP51B7:00")
    else:
        start, last = a.base, a.base + a.size - 1
    if start % PAGE or (last + 1) % PAGE or last < start:
        die("range 0x%x..0x%x is not page aligned" % (start, last))

    try:
        bus, devfn = a.bdf.split(":")
        dev, fn = devfn.split(".")
        bus, dev, fn = int(bus, 16), int(dev, 16), int(fn, 16)
    except ValueError:
        die("bad --bdf %r" % a.bdf)
    if bus != 0 or dev > 31 or fn > 7:
        die("only devices on bus 0 are supported (no bridge path)")

    res = firmware_reserved(start, last)
    if res is False:
        die("0x%x..0x%x is not firmware-reserved memory; an RMRR there would let "
            "the device DMA into RAM the kernel uses" % (start, last))

    rmrrs = []
    for off, typ, sublen in subs:
        if typ == RMRR:
            b, e = struct.unpack_from("<QQ", data, off + 8)
            if b <= last and start <= e:
                die("an RMRR already covers 0x%x..0x%x (0x%x..0x%x); nothing to do"
                    % (start, last, b, e))
            rmrrs.append((off, b))
    # Remapping structures go DRHD, RMRR, ...; keep the RMRRs ordered by base.
    after = [off for off, b in rmrrs if b > start]
    if after:
        at = after[0]
    elif rmrrs:
        at = max(off for off, _ in rmrrs)
        at += struct.unpack_from("<H", data, at + 2)[0]
    else:
        drhd = [(off, sublen) for off, typ, sublen in subs if typ == DRHD]
        if not drhd:
            die("no DRHD in DMAR")
        at = drhd[-1][0] + drhd[-1][1]

    new = bytearray(data[:at] + rmrr(a.segment, start, last, bus, dev, fn) + data[at:])
    struct.pack_into("<I", new, 4, len(new))
    rev = struct.unpack_from("<I", new, 24)[0]
    struct.pack_into("<I", new, 24, rev + 1)
    new[9] = 0
    new[9] = (-sum(new)) & 0xFF
    assert checksum_ok(new)
    parse(bytes(new))

    os.makedirs(a.outdir, exist_ok=True)
    aml = os.path.join(a.outdir, "DMAR.aml")
    with open(aml, "wb") as f:
        f.write(new)
    cpio = newc([
        ("kernel", 0o040755, b""),
        ("kernel/firmware", 0o040755, b""),
        ("kernel/firmware/acpi", 0o040755, b""),
        ("kernel/firmware/acpi/DMAR.aml", 0o100644, bytes(new)),
    ])
    out = os.path.join(a.outdir, "gxfp51b7-dmar.cpio")
    with open(out, "wb") as f:
        f.write(cpio)
    oem = new[10:16].decode(errors="replace"), new[16:24].decode(errors="replace")
    print("DMAR %s %s rev %d -> %d, RMRR %02x:%02x.%x 0x%x..0x%x%s"
          % (oem[0], oem[1].rstrip(), rev, rev + 1, bus, dev, fn, start, last,
             "" if res else " (firmware memory map not readable, not checked)"))
    print(aml)
    print(out)


if __name__ == "__main__":
    main()
