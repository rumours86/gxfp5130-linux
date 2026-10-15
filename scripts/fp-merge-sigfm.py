#!/usr/bin/python3 -I
"""Merge enrollments of one finger (libfprint SIGFM prints) into one print.

    fp-merge-sigfm.py OUT IN1 IN2 [...]

Each print is "FP3" + a GVariant (issbymsmsia{sv}v) whose last member holds
the enrolled frames as (a(ay)). The frames are concatenated; the metadata
(driver, device, finger, user, enroll date) is taken from the last input and
must agree across inputs. A template from two sessions covers the finger
under two skin conditions: on the GXFP51B7 a 16+16 frame template matched
69 of 83 touches from other sessions, against 60 and 50 for its halves.

Stop fprintd and copy OUT over the finger's file in
/var/lib/fprint/<user>/<driver>/<device>/ (keep the originals). The output is
created 0600 and never overwrites an existing file.
"""
import os
import sys

import gi

gi.require_version("GLib", "2.0")
from gi.repository import GLib  # noqa: E402

TYPE = "(issbymsmsia{sv}v)"


def load(path):
    data = open(path, "rb").read()
    if data[:3] != b"FP3":
        sys.exit(f"{path}: not a libfprint print")
    v = GLib.Variant.new_from_bytes(GLib.VariantType(TYPE), GLib.Bytes.new(data[3:]), False)
    fields = v.unpack()
    if fields[0] != 3:  # FPI_PRINT_SIGFM
        sys.exit(f"{path}: not a SIGFM print (type {fields[0]})")
    frames = [bytes(f[0]) for f in fields[9][0]]
    return fields, frames


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    out, inputs = sys.argv[1], sys.argv[2:]
    loaded = [load(p) for p in inputs]
    meta = loaded[-1][0]
    for path, (fields, _) in zip(inputs, loaded):
        if fields[1:6] != meta[1:6]:
            sys.exit(f"{path}: driver/device/finger differ from {inputs[-1]}")
    frames = [f for _, fr in loaded for f in fr]
    inner = GLib.Variant("(a(ay))", ([(f,) for f in frames],))
    outer = GLib.Variant(TYPE, (*meta[:8], {}, inner))
    blob = b"FP3" + outer.get_data_as_bytes().get_data()
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(blob)
    print(f"{out}: {len(frames)} frames from " +
          ", ".join(f"{p} ({len(fr)})" for p, (_, fr) in zip(inputs, loaded)))


if __name__ == "__main__":
    main()
